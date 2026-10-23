package media

import (
	"crypto/rand"
	"encoding/binary"
	"sync"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

const queuePackets = 10
const queueAge = 200 * time.Millisecond
const queueBytes = 24 * 1024
const queueSamples = 9600

type frame struct {
	packet   *rtp.Packet
	permit   control.MediaPermit
	samples  uint32
	received time.Time
}
type packetQueue struct {
	mu      sync.Mutex
	frames  []frame
	bytes   int
	samples uint32
}

func newPacketQueue() packetQueue { return packetQueue{} }
func (q *packetQueue) removeFirst() frame {
	f := q.frames[0]
	q.frames[0] = frame{}
	q.frames = q.frames[1:]
	q.bytes -= f.packet.MarshalSize()
	q.samples -= f.samples
	return f
}
func (q *packetQueue) push(f frame, now time.Time) {
	q.mu.Lock()
	defer q.mu.Unlock()
	for len(q.frames) > 0 && now.Sub(q.frames[0].received) >= queueAge {
		q.removeFirst()
	}
	size := f.packet.MarshalSize()
	if size > queueBytes || f.samples > queueSamples {
		return
	}
	for len(q.frames) > 0 && (len(q.frames) >= queuePackets || q.bytes+size > queueBytes || q.samples+f.samples > queueSamples) {
		q.removeFirst()
	}
	q.frames = append(q.frames, f)
	q.bytes += size
	q.samples += f.samples
}
func (q *packetQueue) pop(now time.Time) (frame, bool) {
	q.mu.Lock()
	defer q.mu.Unlock()
	for len(q.frames) > 0 {
		f := q.removeFirst()
		if now.Sub(f.received) < queueAge {
			return f, true
		}
	}
	return frame{}, false
}
func (q *packetQueue) clear() {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.frames = nil
	q.bytes = 0
	q.samples = 0
}

type outPacket struct{ packet *rtp.Packet }
type sequencer struct {
	sequence     uint16
	timestamp    uint32
	initialized  bool
	lastSource   string
	lastSourceTS uint32
	lastSamples  uint32
	lastSent     time.Time
}

func newSequencer() (sequencer, error) {
	var b [6]byte
	if _, err := rand.Read(b[:]); err != nil {
		return sequencer{}, err
	}
	return sequencer{sequence: binary.BigEndian.Uint16(b[:2]), timestamp: binary.BigEndian.Uint32(b[2:])}, nil
}
func (s *sequencer) packet(f *frame, now time.Time) *outPacket {
	source := ""
	samples := uint32(960)
	payload := []byte{0xf8, 0xff, 0xfe}
	sourceTS := uint32(0)
	if f != nil {
		source = f.permit.Source.DeviceID + "/" + f.permit.Source.PeerID + "/" + f.permit.LeaseID
		samples = f.samples
		payload = f.packet.Payload
		sourceTS = f.packet.Timestamp
	}
	changed := source != s.lastSource
	if s.initialized {
		advance := s.lastSamples
		if source != "" && !changed {
			delta := sourceTS - s.lastSourceTS
			if delta == 0 || delta > 0x7fffffff || delta < s.lastSamples {
				return nil
			}
			if delta <= 480000 {
				advance = delta
			} else {
				changed = true
			}
		}
		if changed {
			elapsed := uint32(now.Sub(s.lastSent).Seconds() * 48000)
			if elapsed > advance {
				advance = elapsed
			}
		}
		s.timestamp += advance
		s.sequence++
	}
	out := &rtp.Packet{Header: rtp.Header{Version: 2, Marker: source != "" && (!s.initialized || changed), SequenceNumber: s.sequence, Timestamp: s.timestamp}, Payload: payload}
	s.initialized = true
	s.lastSource = source
	s.lastSourceTS = sourceTS
	s.lastSamples = samples
	s.lastSent = now
	return &outPacket{packet: out}
}
func (h *Hub) read(source *peer, track *webrtc.TrackRemote) {
	for {
		packet, _, err := track.ReadRTP()
		if err != nil {
			go h.end(source)
			return
		}
		samples, err := opusSamples(packet.Payload)
		if err != nil {
			continue
		}
		h.mu.Lock()
		peers := make([]*peer, 0, len(h.peers))
		for _, p := range h.peers {
			if p != source {
				peers = append(peers, p)
			}
		}
		h.mu.Unlock()
		for _, dest := range peers {
			permit, ok := h.control.AuthorizeMedia(source.device, source.id, dest.device, dest.id)
			if !ok || dest.ctx.Err() != nil {
				continue
			}
			now := h.now()
			dest.queue.push(frame{packet: packet.Clone(), permit: permit, samples: samples, received: now}, now)
		}
	}
}
func (h *Hub) write(p *peer) {
	seq, err := newSequencer()
	if err != nil {
		go h.end(p)
		return
	}
	timer := time.NewTimer(0)
	defer timer.Stop()
	for {
		select {
		case <-p.ctx.Done():
			return
		case <-timer.C:
		}
		duration := 20 * time.Millisecond
		if p.phase.Load().(string) == "connected" && h.control.MediaPeerValid(p.device, p.id) {
			now := h.now()
			var selected *frame
			for {
				f, ok := p.queue.pop(now)
				if !ok {
					break
				}
				if h.control.ValidateMedia(f.permit) {
					selected = &f
					break
				}
			}
			candidate := seq
			out := candidate.packet(selected, now)
			if out != nil {
				// A generated idle Opus silence packet primes browser playback. It contains
				// no source audio and cannot authorize a sender lacking a media lease.
				if selected != nil && !h.control.ValidateMedia(selected.permit) {
					timer.Reset(time.Millisecond)
					continue
				}
				if p.ctx.Err() != nil || !h.control.MediaPeerValid(p.device, p.id) {
					return
				}
				p.writeStarted.Store(time.Now().UnixNano())
				err = p.write(out)
				p.writeStarted.Store(0)
				if err != nil {
					go h.end(p)
					return
				}
				seq = candidate
				if selected != nil {
					duration = time.Duration(selected.samples) * time.Second / 48000
				}
			}
		}
		timer.Reset(duration)
	}
}
