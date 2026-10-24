package lan

import (
	"net"
	"net/netip"
	"testing"
)

func TestDeviceAddressAndLocalOwnership(t *testing.T) {
	for _, value := range []string{"10.0.0.8:8443", "172.16.0.1:443", "172.31.255.254:65535", "192.168.50.5:8443"} {
		if _, err := ParseAddress(value); err != nil {
			t.Fatalf("valid address rejected: %v", err)
		}
	}
	for _, value := range []string{"", "0.0.0.0:8443", ":8443", "localhost:8443", "device.local:8443", "127.0.0.1:8443", "169.254.1.2:8443", "100.64.0.1:8443", "8.8.8.8:8443", "172.15.0.1:8443", "172.32.0.1:8443", "192.168.50.5", "192.168.50.5:0", "192.168.50.5:-1", "192.168.50.5:65536", "192.168.50.5:08443", "[::1]:8443", "[fd00::1]:8443", "[::ffff:192.168.50.5]:8443", "192.168.050.5:8443", "user@192.168.50.5:8443"} {
		if _, err := ParseAddress(value); err == nil {
			t.Fatalf("unsafe address accepted: %s", value)
		}
	}
	ip := netip.MustParseAddr("192.168.50.5")
	if RequireLocal(ip, []netip.Addr{ip}) != nil {
		t.Fatal("assigned address rejected")
	}
	if RequireLocal(ip, []netip.Addr{netip.MustParseAddr("192.168.50.6")}) == nil {
		t.Fatal("unassigned address accepted")
	}
}

func TestMediaConfigurationAndCandidatePolicy(t *testing.T) {
	selected := netip.MustParseAddr("192.168.50.5")
	if err := ValidateMedia(netip.Addr{}, 0, 0); err != nil {
		t.Fatal(err)
	}
	if err := ValidateMedia(selected, 40000, 40100); err != nil {
		t.Fatal(err)
	}
	for _, value := range []struct {
		ip               netip.Addr
		minimum, maximum int
	}{
		{netip.Addr{}, 40000, 40100}, {selected, 0, 40100}, {selected, 40000, 0}, {selected, -1, 40100}, {selected, 40100, 40000}, {selected, 40000, 65536}, {netip.MustParseAddr("8.8.8.8"), 40000, 40100}, {netip.MustParseAddr("fd00::1"), 40000, 40100},
	} {
		if ValidateMedia(value.ip, value.minimum, value.maximum) == nil {
			t.Fatal("partial/unsafe media configuration accepted")
		}
	}
	for _, value := range []string{"127.0.0.1", "127.0.0.2", "::1"} {
		if !CandidateAllowed(netip.Addr{}, net.ParseIP(value)) || !CandidateAllowed(selected, net.ParseIP(value)) {
			t.Fatal("loopback policy changed")
		}
	}
	if !CandidateAllowed(selected, net.ParseIP(selected.String())) {
		t.Fatal("explicit candidate rejected")
	}
	for _, value := range []string{"192.168.50.6", "10.0.0.8", "8.8.8.8", "0.0.0.0", "169.254.1.2", "fd00::1"} {
		if CandidateAllowed(selected, net.ParseIP(value)) || CandidateAllowed(netip.Addr{}, net.ParseIP(value)) {
			t.Fatalf("unselected candidate accepted: %s", value)
		}
	}
}
