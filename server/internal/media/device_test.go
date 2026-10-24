package media

import (
	"context"
	"net"
	"net/netip"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/lan"
	"github.com/pion/sdp/v3"
)

func TestDeviceMediaRejectsPartialOrUnsafeOptions(t *testing.T) {
	for _, options := range []Options{
		{DeviceIP: netip.MustParseAddr("8.8.8.8"), DeviceUDPMin: 40000, DeviceUDPMax: 40199},
		{DeviceIP: netip.MustParseAddr("192.168.50.5")},
		{DeviceUDPMin: 40000, DeviceUDPMax: 40199},
		{DeviceIP: netip.MustParseAddr("192.168.50.5"), DeviceUDPMin: 40199, DeviceUDPMax: 40000},
		{DeviceIP: netip.MustParseAddr("192.168.50.5"), DeviceUDPMin: 40000, DeviceUDPMax: 65536},
	} {
		hub, err := New(nil, options)
		if err == nil {
			hub.Close()
			t.Fatal("unsafe media options accepted")
		}
	}
}

func TestDefaultAndDeviceGatheredCandidateBoundaries(t *testing.T) {
	local, err := lan.LocalIPv4s()
	must(t, err)
	var selected netip.Addr
	for _, ip := range local {
		if lan.PrivateIPv4(ip) {
			selected = ip
			break
		}
	}
	for _, device := range []bool{false, true} {
		t.Run(map[bool]string{false: "default_loopback", true: "explicit_device_LAN"}[device], func(t *testing.T) {
			if device && !selected.IsValid() {
				t.Skip("no active assigned RFC1918 IPv4 for optional LAN gathering")
			}
			options := Options{}
			if device {
				options.DeviceIP = selected
				options.DeviceUDPMin = 40000
				options.DeviceUDPMax = 40199
			}
			f := fixture(t, 1)
			f.h.Close()
			hub, err := New(f.s, options)
			must(t, err)
			defer hub.Close()
			client := offerClient(t)
			state, err := f.s.State(f.devices[0].ID)
			must(t, err)
			request := CreateRequest{PeerID: "candidate_test", GroupID: state.CurrentGroupID, CurrentVersion: state.CurrentVersion, ServerEpoch: state.ServerEpoch,
				ValidUntil: time.Now().UTC().Add(9 * time.Second), Offer: client.req.Offer}
			answer, err := hub.Create(context.Background(), f.devices[0].ID, request)
			must(t, err)
			var description sdp.SessionDescription
			must(t, description.Unmarshal([]byte(answer.Answer.SDP)))
			count, selectedSeen := 0, false
			for _, media := range description.MediaDescriptions {
				for _, attribute := range media.Attributes {
					if attribute.Key != "candidate" {
						continue
					}
					count++
					fields := strings.Fields(attribute.Value)
					if len(fields) < 8 || strings.ToLower(fields[2]) != "udp" || fields[6] != "typ" || fields[7] != "host" {
						t.Fatal("unexpected non-host/UDP candidate")
					}
					ip := net.ParseIP(fields[4])
					if ip == nil || !lan.CandidateAllowed(options.DeviceIP, ip) {
						t.Fatal("candidate escaped the configured IP policy")
					}
					if device {
						port, err := strconv.Atoi(fields[5])
						if err != nil || port < 40000 || port > 40199 {
							t.Fatal("candidate escaped configured UDP range")
						}
						if ip.Equal(net.ParseIP(selected.String())) {
							selectedSeen = true
						}
					}
				}
			}
			if count == 0 {
				t.Fatal("no gathered loopback candidates")
			}
			if device && !selectedSeen {
				t.Fatal("assigned explicit LAN candidate missing")
			}
		})
	}
}
