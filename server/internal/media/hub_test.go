package media

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/pion/interceptor"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

type mediaFixture struct {
	s       *control.Service
	h       *Hub
	devices []control.Device
	g1, g2  control.Group
}

func fixture(t *testing.T, count int) *mediaFixture {
	t.Helper()
	s, err := control.New(nil, nil, control.Options{})
	must(t, err)
	h, err := New(s, Options{})
	must(t, err)
	t.Cleanup(h.Close)
	f := &mediaFixture{s: s, h: h}
	for i := 0; i < count; i++ {
		d, _, err := s.CreateDevice(fmt.Sprintf("端%d", i))
		must(t, err)
		f.devices = append(f.devices, d)
	}
	g1, code, _, err := s.CreateGroup(f.devices[0].ID, "合作")
	must(t, err)
	f.g1 = g1
	g2, _, _, err := s.CreateGroup(f.devices[0].ID, "另一组")
	must(t, err)
	f.g2 = g2
	for i := 1; i < count; i++ {
		_, err = s.Join(f.devices[i].ID, code)
		must(t, err)
	}
	return f
}
func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}
func code(t *testing.T, err error, want string) {
	t.Helper()
	var e *control.Error
	if !errors.As(err, &e) || e.Code != want {
		t.Fatalf("error=%v want=%s", err, want)
	}
}
func eventually(t *testing.T, timeout time.Duration, fn func() bool) {
	t.Helper()
	until := time.Now().Add(timeout)
	for time.Now().Before(until) {
		if fn() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("condition did not become true")
}

type testClient struct {
	pc        *webrtc.PeerConnection
	up        *webrtc.TrackLocalStaticRTP
	received  chan *rtp.Packet
	ready     chan struct{}
	once      sync.Once
	sequence  atomic.Uint32
	timestamp atomic.Uint32
	req       CreateRequest
	done      chan struct{}
}

func offerClient(t *testing.T) *testClient {
	t.Helper()
	engine := &webrtc.MediaEngine{}
	must(t, engine.RegisterDefaultCodecs())
	registry := &interceptor.Registry{}
	must(t, webrtc.ConfigureRTCPReports(registry))
	settings := webrtc.SettingEngine{}
	settings.SetIncludeLoopbackCandidate(true)
	settings.SetIPFilter(func(ip net.IP) bool { return ip.IsLoopback() })
	api := webrtc.NewAPI(webrtc.WithMediaEngine(engine), webrtc.WithInterceptorRegistry(registry), webrtc.WithSettingEngine(settings))
	pc, err := api.NewPeerConnection(webrtc.Configuration{})
	must(t, err)
	up, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2}, "audio", "test")
	must(t, err)
	sender, err := pc.AddTrack(up)
	must(t, err)
	c := &testClient{pc: pc, up: up, received: make(chan *rtp.Packet, 1024), ready: make(chan struct{}), done: make(chan struct{})}
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		if state == webrtc.PeerConnectionStateConnected {
			c.once.Do(func() { close(c.ready) })
		}
	})
	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		go func() {
			for {
				p, _, err := track.ReadRTP()
				if err != nil {
					return
				}
				select {
				case c.received <- p.Clone():
				default:
				}
			}
		}()
	})
	go func() {
		for {
			if _, _, err := sender.ReadRTCP(); err != nil {
				return
			}
		}
	}()
	offer, err := pc.CreateOffer(nil)
	must(t, err)
	gathered := webrtc.GatheringCompletePromise(pc)
	must(t, pc.SetLocalDescription(offer))
	select {
	case <-gathered:
	case <-time.After(3 * time.Second):
		t.Fatal("client gathering timed out")
	}
	c.req.Offer = *pc.LocalDescription()
	t.Cleanup(func() { close(c.done); _ = pc.Close() })
	return c
}
func (f *mediaFixture) connect(t *testing.T, index int, id, replace string) *testClient {
	t.Helper()
	c := offerClient(t)
	device := f.devices[index]
	st, err := f.s.State(device.ID)
	must(t, err)
	c.req = CreateRequest{PeerID: id, ReplacePeerID: replace, GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch, ValidUntil: time.Now().UTC().Add(9 * time.Second), Offer: c.req.Offer}
	out, err := f.h.Create(context.Background(), device.ID, c.req)
	must(t, err)
	must(t, c.pc.SetRemoteDescription(out.Answer))
	select {
	case <-c.ready:
	case <-time.After(5 * time.Second):
		t.Fatal("client did not connect")
	}
	eventually(t, time.Second, func() bool { b, _, _ := f.s.MediaBinding(device.ID); return b.PeerID == id && b.Ready })
	// The downlink priming packet proves a complete real SRTP path before checks.
	select {
	case <-c.received:
	case <-time.After(time.Second):
		t.Fatal("no downlink priming RTP")
	}
	return c
}
func (f *mediaFixture) floor(t *testing.T, index int, peerID, requestID string) control.AcquireRequest {
	t.Helper()
	st, err := f.s.State(f.devices[index].ID)
	must(t, err)
	return control.AcquireRequest{FloorRequest: control.FloorRequest{PeerID: peerID, GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, RequestID: requestID, ServerEpoch: st.ServerEpoch}, ValidUntil: time.Now().UTC().Add(5 * time.Second)}
}

// These syntactically valid compressed payloads identify RTP paths. Browser
// diagnostics separately validate actual Opus decoding and received PCM energy.
func (c *testClient) send(tag byte) {
	seq := c.sequence.Add(1)
	ts := c.timestamp.Add(960)
	_ = c.up.WriteRTP(&rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: uint16(seq), Timestamp: ts}, Payload: []byte{0xf8, 0xff, tag}})
}
func (c *testClient) stream(t *testing.T, tag byte) func() {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		ticker := time.NewTicker(20 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-c.done:
				return
			case <-ticker.C:
				c.send(tag)
			}
		}
	}()
	t.Cleanup(cancel)
	return cancel
}
func tagged(p *rtp.Packet, tag byte) bool {
	return len(p.Payload) == 3 && p.Payload[0] == 0xf8 && p.Payload[2] == tag
}
func awaitTag(t *testing.T, c *testClient, tag byte) {
	t.Helper()
	timer := time.NewTimer(2 * time.Second)
	defer timer.Stop()
	for {
		select {
		case p := <-c.received:
			if tagged(p, tag) {
				return
			}
		case <-timer.C:
			t.Fatalf("no remote RTP tag %d", tag)
		}
	}
}
func noTag(t *testing.T, c *testClient, tag byte, d time.Duration) {
	t.Helper()
	timer := time.NewTimer(d)
	defer timer.Stop()
	for {
		select {
		case p := <-c.received:
			if tagged(p, tag) {
				t.Fatalf("unauthorized tag %d received", tag)
			}
		case <-timer.C:
			return
		}
	}
}
func drain(c *testClient) {
	for {
		select {
		case <-c.received:
		default:
			return
		}
	}
}

func TestRealPionRelayAuthorityBidirectionalFanoutAndIsolation(t *testing.T) {
	f := fixture(t, 3)
	a := f.connect(t, 0, "peer_a001", "")
	b := f.connect(t, 1, "peer_b001", "")
	c := f.connect(t, 2, "peer_c001", "")
	a.stream(t, 0x11)
	b.stream(t, 0x22)
	noTag(t, b, 0x11, 150*time.Millisecond)
	legacy := f.floor(t, 0, "", "legacy001")
	_, err := f.s.Acquire(f.devices[0].ID, legacy)
	must(t, err)
	noTag(t, b, 0x11, 120*time.Millisecond)
	must(t, f.s.Release(f.devices[0].ID, legacy.FloorRequest))
	ar := f.floor(t, 0, a.req.PeerID, "press_a01")
	_, err = f.s.Acquire(f.devices[0].ID, ar)
	must(t, err)
	awaitTag(t, b, 0x11)
	awaitTag(t, c, 0x11)
	must(t, f.s.Release(f.devices[0].ID, ar.FloorRequest))
	time.Sleep(100 * time.Millisecond)
	drain(b)
	drain(c)
	noTag(t, b, 0x11, 150*time.Millisecond)
	br := f.floor(t, 1, b.req.PeerID, "press_b01")
	_, err = f.s.Acquire(f.devices[1].ID, br)
	must(t, err)
	awaitTag(t, a, 0x22)
	awaitTag(t, c, 0x22)
	must(t, f.s.Release(f.devices[1].ID, br.FloorRequest))
}

func TestRealPionCrossGroupAndSwitchClosesOldBinding(t *testing.T) {
	f := fixture(t, 3)
	_, code2, _, err := f.s.CreateGroup(f.devices[2].ID, "隔离群")
	must(t, err)
	a := f.connect(t, 0, "peer_a001", "")
	b := f.connect(t, 1, "peer_b001", "")
	oldC := f.connect(t, 2, "peer_c001", "")
	_, _, err = f.s.Switch(f.devices[2].ID, control.SwitchRequest{OperationID: "switch_c1", GroupID: stateGroup(t, f.s, f.devices[2].ID, code2), ExpectedVersion: 1})
	must(t, err)
	eventually(t, time.Second, func() bool {
		f.h.mu.Lock()
		defer f.h.mu.Unlock()
		return f.h.peers[key(f.devices[2].ID, oldC.req.PeerID)] == nil
	})
	c := f.connect(t, 2, "peer_c002", "")
	a.stream(t, 0x11)
	c.stream(t, 0x33)
	ar := f.floor(t, 0, a.req.PeerID, "press_a01")
	_, err = f.s.Acquire(f.devices[0].ID, ar)
	must(t, err)
	cr := f.floor(t, 2, c.req.PeerID, "press_c01")
	_, err = f.s.Acquire(f.devices[2].ID, cr)
	must(t, err)
	awaitTag(t, b, 0x11)
	noTag(t, c, 0x11, 180*time.Millisecond)
	noTag(t, b, 0x33, 180*time.Millisecond)
	noTag(t, a, 0x33, 100*time.Millisecond)
}
func stateGroup(t *testing.T, s *control.Service, id, code string) string {
	t.Helper()
	st, err := s.Join(id, code)
	must(t, err)
	for _, g := range st.Groups {
		if g.Name == "隔离群" {
			return g.ID
		}
	}
	t.Fatal("group missing")
	return ""
}

func TestRealPionLeaseExpiresWithoutControlRequests(t *testing.T) {
	f := fixture(t, 2)
	a := f.connect(t, 0, "peer_a001", "")
	b := f.connect(t, 1, "peer_b001", "")
	a.stream(t, 0x11)
	req := f.floor(t, 0, a.req.PeerID, "press_a01")
	floor, err := f.s.Acquire(f.devices[0].ID, req)
	must(t, err)
	awaitTag(t, b, 0x11)
	time.Sleep(time.Until(floor.ExpiresAt) + 120*time.Millisecond)
	drain(b)
	noTag(t, b, 0x11, 200*time.Millisecond)
	next := f.floor(t, 1, b.req.PeerID, "press_b01")
	_, err = f.s.Acquire(f.devices[1].ID, next)
	must(t, err)
}
func TestPeerCancelReplayCASAndOwnerIsolation(t *testing.T) {
	f := fixture(t, 2)
	offer := offerClient(t)
	st, err := f.s.State(f.devices[0].ID)
	must(t, err)
	req := CreateRequest{PeerID: "canceled1", GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch, ValidUntil: time.Now().Add(9 * time.Second), Offer: offer.req.Offer}
	must(t, f.h.Delete(f.devices[0].ID, req.PeerID))
	_, err = f.h.Create(context.Background(), f.devices[0].ID, req)
	code(t, err, "peer_ended")
	a := f.connect(t, 0, "same_id01", "")
	b := f.connect(t, 1, "same_id01", "")
	replay, err := f.h.Create(context.Background(), f.devices[0].ID, a.req)
	must(t, err)
	if replay.PeerID != a.req.PeerID {
		t.Fatal("replay changed Peer")
	}
	changed := a.req
	changed.GroupID = "other"
	_, err = f.h.Create(context.Background(), f.devices[0].ID, changed)
	code(t, err, "peer_conflict")
	must(t, f.h.Delete(f.devices[1].ID, b.req.PeerID))
	if !f.s.MediaPeerValid(f.devices[0].ID, a.req.PeerID) {
		t.Fatal("other owner deleted A")
	}
	stale := a.req
	stale.PeerID = "stale001"
	stale.ReplacePeerID = "not_current"
	_, err = f.h.Create(context.Background(), f.devices[0].ID, stale)
	code(t, err, "replace_conflict")
}
func TestReplacementOldCallbacksCannotRevokeNewLease(t *testing.T) {
	f := fixture(t, 2)
	old := f.connect(t, 0, "peer_old1", "")
	b := f.connect(t, 1, "peer_b001", "")
	oldFloor := f.floor(t, 0, old.req.PeerID, "oldpress1")
	_, err := f.s.Acquire(f.devices[0].ID, oldFloor)
	must(t, err)
	f.h.mu.Lock()
	oldPeer := f.h.peers[key(f.devices[0].ID, old.req.PeerID)]
	f.h.mu.Unlock()
	next := f.connect(t, 0, "peer_new1", old.req.PeerID)
	next.stream(t, 0x44)
	req := f.floor(t, 0, next.req.PeerID, "newpress1")
	_, err = f.s.Acquire(f.devices[0].ID, req)
	must(t, err)
	f.h.end(oldPeer)
	f.s.UnbindMediaPeer(f.devices[0].ID, old.req.PeerID)
	must(t, f.h.Delete(f.devices[0].ID, old.req.PeerID))
	awaitTag(t, b, 0x44)
	if !f.s.MediaPeerValid(f.devices[0].ID, next.req.PeerID) {
		t.Fatal("old callback removed replacement")
	}
}
func TestPeerLimitsDeadlinesAndCanceledContext(t *testing.T) {
	f := fixture(t, 1)
	offer := offerClient(t)
	st, err := f.s.State(f.devices[0].ID)
	must(t, err)
	req := CreateRequest{PeerID: "deadline1", GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch, ValidUntil: time.Now().Add(11 * time.Second), Offer: offer.req.Offer}
	_, err = f.h.Create(context.Background(), f.devices[0].ID, req)
	code(t, err, "invalid_deadline")
	req.ValidUntil = time.Now().Add(-time.Second)
	_, err = f.h.Create(context.Background(), f.devices[0].ID, req)
	code(t, err, "request_expired")
	req.PeerID = "cancelctx"
	req.ValidUntil = time.Now().Add(9 * time.Second)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err = f.h.Create(ctx, f.devices[0].ID, req)
	code(t, err, "request_canceled")
	for i := 2; i < MaxPeerRecords; i++ {
		must(t, f.h.Delete(f.devices[0].ID, fmt.Sprintf("cancel%04d", i)))
	}
	err = f.h.Delete(f.devices[0].ID, "overflow1")
	code(t, err, "peer_capacity")
}
func TestRequestContextEndsAfterSuccessfulAnswer(t *testing.T) {
	f := fixture(t, 1)
	c := offerClient(t)
	st, err := f.s.State(f.devices[0].ID)
	must(t, err)
	req := CreateRequest{PeerID: "lifetime1", GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch, ValidUntil: time.Now().Add(9 * time.Second), Offer: c.req.Offer}
	ctx, cancel := context.WithCancel(context.Background())
	out, err := f.h.Create(ctx, f.devices[0].ID, req)
	must(t, err)
	cancel()
	must(t, c.pc.SetRemoteDescription(out.Answer))
	eventually(t, 3*time.Second, func() bool { b, _, _ := f.s.MediaBinding(f.devices[0].ID); return b.PeerID == req.PeerID && b.Ready })
}

func TestRepeatedRealPionConnectCloseReleasesAllBindings(t *testing.T) {
	f := fixture(t, 1)
	for i := 0; i < 10; i++ {
		id := fmt.Sprintf("reconnect%02d", i)
		f.connect(t, 0, id, "")
		must(t, f.h.Delete(f.devices[0].ID, id))
		binding, _, err := f.s.MediaBinding(f.devices[0].ID)
		must(t, err)
		if binding.PeerID != "" {
			t.Fatal("closed Peer binding retained")
		}
	}
	f.h.Close()
	f.h.mu.Lock()
	defer f.h.mu.Unlock()
	if len(f.h.peers) != 0 {
		t.Fatalf("%d Peers leaked", len(f.h.peers))
	}
}
func TestGlobalPeerCapacityIsBounded(t *testing.T) {
	f := fixture(t, 65)
	// Reserve 64 valid control bindings without opening 64 UDP transports. This
	// exercises the same global reservation boundary used before Pion allocation.
	for i := 0; i < MaxPeers; i++ {
		device := f.devices[i]
		st, err := f.s.State(device.ID)
		must(t, err)
		_, err = f.s.BindMediaPeer(device.ID, "reserved1", "", st.CurrentGroupID, st.CurrentVersion, st.ServerEpoch)
		must(t, err)
		ctx, cancel := context.WithCancel(f.h.ctx)
		p := &peer{id: "reserved1", device: device.ID, group: st.CurrentGroupID, version: st.CurrentVersion, epoch: st.ServerEpoch, ctx: ctx, cancel: cancel, created: time.Now(), queue: newPacketQueue()}
		p.phase.Store("connected")
		f.h.mu.Lock()
		f.h.peers[key(device.ID, p.id)] = p
		if f.h.records[device.ID] == nil {
			f.h.records[device.ID] = map[string]*record{}
		}
		f.h.records[device.ID][p.id] = &record{status: "active", peer: p}
		f.h.mu.Unlock()
	}
	c := offerClient(t)
	st, err := f.s.State(f.devices[64].ID)
	must(t, err)
	req := CreateRequest{PeerID: "overflow1", GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch, ValidUntil: time.Now().Add(9 * time.Second), Offer: c.req.Offer}
	_, err = f.h.Create(context.Background(), f.devices[64].ID, req)
	code(t, err, "media_capacity")
	must(t, f.h.Delete(f.devices[0].ID, "reserved1"))
	_, err = f.h.Create(context.Background(), f.devices[64].ID, req)
	must(t, err)
}
func TestPeerProtectionExpiresButLateCreationDeadlineDoesNot(t *testing.T) {
	f := fixture(t, 1)
	var advance atomic.Int64
	base := time.Now()
	h, err := New(f.s, Options{Now: func() time.Time { return base.Add(time.Duration(advance.Load())) }})
	must(t, err)
	t.Cleanup(h.Close)
	for i := 0; i < MaxPeerRecords; i++ {
		must(t, h.Delete(f.devices[0].ID, fmt.Sprintf("cancel%04d", i)))
	}
	code(t, h.Delete(f.devices[0].ID, "overflow1"), "peer_capacity")
	advance.Store(int64(61 * time.Second))
	must(t, h.Delete(f.devices[0].ID, "overflow1"))
	c := offerClient(t)
	st, err := f.s.State(f.devices[0].ID)
	must(t, err)
	req := CreateRequest{PeerID: "cancel0000", GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch, ValidUntil: base.Add(9 * time.Second), Offer: c.req.Offer}
	_, err = h.Create(context.Background(), f.devices[0].ID, req)
	code(t, err, "request_expired")
}
