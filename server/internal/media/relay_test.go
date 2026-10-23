package media

import (
	"context"
	"sync/atomic"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/pion/rtp"
)

func testFrame(source string, stamp uint32, samples uint32, now time.Time) frame {
	return frame{packet: &rtp.Packet{Header: rtp.Header{Version: 2, Timestamp: stamp, SequenceNumber: 333, SSRC: 123, Extension: true, CSRC: []uint32{7}}, Payload: []byte{0xf8, 0xff, 0x22}}, permit: control.MediaPermit{Source: control.MediaBinding{DeviceID: source, PeerID: source + "peer"}, LeaseID: source + "lease"}, samples: samples, received: now}
}
func TestRTPSequenceTimestampWrapSourceChangeAndLoss(t *testing.T) {
	now := time.Now()
	s := sequencer{sequence: 65535, timestamp: 0xffffff00}
	a := testFrame("a", 0xfffffc40, 960, now)
	first := s.packet(&a, now)
	if first.packet.SequenceNumber != 65535 || !first.packet.Marker {
		t.Fatal("initial sequence/marker")
	}
	a.packet.Timestamp = 0
	second := s.packet(&a, now.Add(20*time.Millisecond))
	if second.packet.SequenceNumber != 0 || second.packet.Timestamp != 704 || second.packet.Marker {
		t.Fatal("RTP wrap not preserved")
	}
	a.packet.Timestamp = 1920
	third := s.packet(&a, now.Add(60*time.Millisecond))
	if third.packet.Timestamp-second.packet.Timestamp != 1920 {
		t.Fatal("source packet loss lost time")
	}
	a.packet.Timestamp = 960
	if s.packet(&a, now.Add(65*time.Millisecond)) != nil {
		t.Fatal("out of order source accepted")
	}
	b := testFrame("b", 123456, 960, now.Add(80*time.Millisecond))
	changed := s.packet(&b, now.Add(80*time.Millisecond))
	if !changed.packet.Marker || changed.packet.SequenceNumber != third.packet.SequenceNumber+1 || changed.packet.Timestamp-third.packet.Timestamp != 960 {
		t.Fatal("source switch broke continuity")
	}
	if changed.packet.Extension || len(changed.packet.CSRC) != 0 || changed.packet.SSRC != 0 || changed.packet.PayloadType != 0 {
		t.Fatal("source headers leaked")
	}
	idle := s.packet(nil, now.Add(580*time.Millisecond))
	if idle.packet.Timestamp-changed.packet.Timestamp != 24000 || idle.packet.Marker {
		t.Fatal("idle clock gap mishandled")
	}
}
func TestQueueAllBoundsAndAge(t *testing.T) {
	now := time.Now()
	q := newPacketQueue()
	for i := 0; i < 30; i++ {
		f := testFrame("a", uint32(i*960), 960, now)
		q.push(f, now)
	}
	if len(q.frames) != 10 || q.samples != 9600 {
		t.Fatal("packet/audio duration cap")
	}
	first, ok := q.pop(now)
	if !ok || first.packet.Timestamp != 20*960 {
		t.Fatal("oldest packets were not discarded")
	}
	q.clear()
	for i := 0; i < 4; i++ {
		f := testFrame("a", 0, 2880, now)
		q.push(f, now)
	}
	if len(q.frames) != 3 || q.samples != 8640 {
		t.Fatal("duration cap failed")
	}
	q.clear()
	for i := 0; i < 4; i++ {
		f := testFrame("a", 0, 960, now)
		f.packet.Payload = make([]byte, 8192)
		q.push(f, now)
	}
	if q.bytes > queueBytes || len(q.frames) != 2 {
		t.Fatal("byte cap failed")
	}
	if _, ok = q.pop(now.Add(201 * time.Millisecond)); ok || q.bytes != 0 || q.samples != 0 {
		t.Fatal("stale queue retained")
	}
}
func TestOpusPacketFramingAndDuration(t *testing.T) {
	cases := []struct {
		payload []byte
		samples uint32
	}{
		{[]byte{0xf8, 0xff, 0xfe}, 960},
		{[]byte{0x80, 1}, 120},
		{[]byte{0x18, 1}, 2880},
		{[]byte{0xf9, 1, 2}, 1920},
		{[]byte{0xfa, 1, 11, 12}, 1920},
		{[]byte{0xfb, 2, 1, 2}, 1920},
		{[]byte{0xfb, 0x82, 1, 11, 12}, 1920},
	}
	for _, tt := range cases {
		got, err := opusSamples(tt.payload)
		if err != nil || got != tt.samples {
			t.Fatalf("payload=%v samples=%d err=%v", tt.payload, got, err)
		}
	}
	for _, bad := range [][]byte{nil, {0xf9, 1}, {0xfa}, {0xfa, 9, 1}, {0xfb}, {0xfb, 0}, {0xfb, 63}, {0xfb, 0x42, 9}, {0x1b, 3, 1, 2, 3}} {
		if _, err := opusSamples(bad); err == nil {
			t.Fatalf("invalid Opus accepted: %v", bad)
		}
	}
}
func TestSlowWriterClosesOnlyItsPeer(t *testing.T) {
	f := fixture(t, 2)
	var fast atomic.Int32
	makePeer := func(index int, id string, blocked bool) *peer {
		device := f.devices[index]
		st, err := f.s.State(device.ID)
		must(t, err)
		_, err = f.s.BindMediaPeer(device.ID, id, "", st.CurrentGroupID, st.CurrentVersion, st.ServerEpoch)
		must(t, err)
		if !f.s.SetMediaReady(device.ID, id) {
			t.Fatal("not ready")
		}
		ctx, cancel := context.WithCancel(f.h.ctx)
		p := &peer{id: id, device: device.ID, group: st.CurrentGroupID, version: st.CurrentVersion, epoch: st.ServerEpoch, ctx: ctx, cancel: cancel, queue: newPacketQueue(), created: time.Now()}
		p.phase.Store("connected")
		p.write = func(_ *outPacket) error {
			if blocked {
				<-p.ctx.Done()
			} else {
				fast.Add(1)
			}
			return nil
		}
		f.h.mu.Lock()
		f.h.peers[key(device.ID, id)] = p
		f.h.mu.Unlock()
		f.h.start(p, func() { f.h.write(p) })
		return p
	}
	slow := makePeer(0, "slowpeer1", true)
	good := makePeer(1, "fastpeer1", false)
	eventually(t, time.Second, func() bool { return slow.ctx.Err() != nil })
	if fast.Load() < 3 || good.ctx.Err() != nil || !f.s.MediaPeerValid(good.device, good.id) {
		t.Fatal("slow receiver blocked another Peer")
	}
}
func TestQueuedPermitDoesNotSurviveReleaseOrNewLease(t *testing.T) {
	f := fixture(t, 2)
	for i, id := range []string{"source001", "dest00001"} {
		st, err := f.s.State(f.devices[i].ID)
		must(t, err)
		_, err = f.s.BindMediaPeer(f.devices[i].ID, id, "", st.CurrentGroupID, st.CurrentVersion, st.ServerEpoch)
		must(t, err)
		if !f.s.SetMediaReady(f.devices[i].ID, id) {
			t.Fatal("not ready")
		}
	}
	req := f.floor(t, 0, "source001", "press0001")
	_, err := f.s.Acquire(f.devices[0].ID, req)
	must(t, err)
	permit, ok := f.s.AuthorizeMedia(f.devices[0].ID, "source001", f.devices[1].ID, "dest00001")
	if !ok {
		t.Fatal("permit missing")
	}
	must(t, f.s.Release(f.devices[0].ID, req.FloorRequest))
	if f.s.ValidateMedia(permit) {
		t.Fatal("released frame still authorized")
	}
	req = f.floor(t, 0, "source001", "press0002")
	_, err = f.s.Acquire(f.devices[0].ID, req)
	must(t, err)
	if f.s.ValidateMedia(permit) {
		t.Fatal("old frame rebound to new lease")
	}
}
