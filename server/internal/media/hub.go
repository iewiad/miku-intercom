// Package media relays Opus without decoding it. Control remains authoritative.
package media

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/pion/interceptor"
	"github.com/pion/sdp/v3"
	"github.com/pion/webrtc/v4"
)

const MaxPeers = 64
const MaxPeerRecords = 256
const Protection = 60 * time.Second
const negotiationLimit = 5 * time.Second
const connectionLimit = 10 * time.Second
const writeBudget = 100 * time.Millisecond

type Options struct {
	Diagnostics bool
	Now         func() time.Time
}
type CreateRequest struct {
	PeerID         string                    `json:"peer_id"`
	ReplacePeerID  string                    `json:"replace_peer_id"`
	GroupID        string                    `json:"group_id"`
	CurrentVersion uint64                    `json:"current_version"`
	ServerEpoch    string                    `json:"server_epoch"`
	ValidUntil     time.Time                 `json:"valid_until"`
	Offer          webrtc.SessionDescription `json:"offer"`
}
type CreateResponse struct {
	PeerID         string                    `json:"peer_id"`
	GroupID        string                    `json:"group_id"`
	CurrentVersion uint64                    `json:"current_version"`
	ServerEpoch    string                    `json:"server_epoch"`
	Answer         webrtc.SessionDescription `json:"answer"`
}
type Status struct {
	Available          bool   `json:"available"`
	DiagnosticsEnabled bool   `json:"diagnostics_enabled"`
	Codec              string `json:"codec"`
	ClockRate          int    `json:"clock_rate"`
	ServerEpoch        string `json:"server_epoch"`
	PeerID             string `json:"peer_id"`
	Phase              string `json:"phase"`
	GroupID            string `json:"group_id"`
	CurrentVersion     uint64 `json:"current_version"`
}
type record struct {
	hash           [32]byte
	status         string
	protectedUntil time.Time
	peer           *peer
	answer         *CreateResponse
}
type Hub struct {
	mu          sync.Mutex
	control     *control.Service
	api         *webrtc.API
	now         func() time.Time
	diagnostics bool
	peers       map[string]*peer
	records     map[string]map[string]*record
	ctx         context.Context
	cancel      context.CancelFunc
	closed      bool
	workers     sync.WaitGroup
}
type peer struct {
	id, device, group, epoch string
	version                  uint64
	validUntil               time.Time
	created                  time.Time
	ctx                      context.Context
	cancel                   context.CancelFunc
	once                     sync.Once
	mu                       sync.Mutex
	pc                       *webrtc.PeerConnection
	down                     *webrtc.TrackLocalStaticRTP
	closed                   bool
	phase                    atomic.Value
	trackSeen                atomic.Bool
	writeStarted             atomic.Int64
	queue                    packetQueue
	write                    func(*outPacket) error
}

func problem(status int, code, msg string) error {
	return &control.Error{Status: status, Code: code, Message: msg}
}
func validID(id string) bool {
	if len(id) < 8 || len(id) > 80 {
		return false
	}
	for _, c := range id {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' || c == '-') {
			return false
		}
	}
	return true
}
func key(device, id string) string { return device + "/" + id }

func New(service *control.Service, opts Options) (*Hub, error) {
	if opts.Now == nil {
		opts.Now = time.Now
	}
	engine := &webrtc.MediaEngine{}
	if err := engine.RegisterCodec(webrtc.RTPCodecParameters{RTPCodecCapability: webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2, SDPFmtpLine: "minptime=10;useinbandfec=1"}, PayloadType: 111}, webrtc.RTPCodecTypeAudio); err != nil {
		return nil, err
	}
	registry := &interceptor.Registry{}
	// Reports only: no NACK/RTX retransmission cache may bypass the lease gate.
	if err := webrtc.ConfigureRTCPReports(registry); err != nil {
		return nil, err
	}
	settings := webrtc.SettingEngine{}
	settings.SetIncludeLoopbackCandidate(true)
	settings.SetIPFilter(func(ip net.IP) bool { return ip.IsLoopback() })
	settings.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4, webrtc.NetworkTypeUDP6})
	api := webrtc.NewAPI(webrtc.WithMediaEngine(engine), webrtc.WithInterceptorRegistry(registry), webrtc.WithSettingEngine(settings))
	ctx, cancel := context.WithCancel(context.Background())
	h := &Hub{control: service, api: api, now: opts.Now, diagnostics: opts.Diagnostics, peers: map[string]*peer{}, records: map[string]map[string]*record{}, ctx: ctx, cancel: cancel}
	h.workers.Add(1)
	go h.watch()
	return h, nil
}
func (h *Hub) DiagnosticsEnabled() bool { return h.diagnostics }
func (h *Hub) Status(device string) (Status, error) {
	binding, epoch, err := h.control.MediaBinding(device)
	if err != nil {
		return Status{}, err
	}
	out := Status{Available: true, DiagnosticsEnabled: h.diagnostics, Codec: webrtc.MimeTypeOpus, ClockRate: 48000, ServerEpoch: epoch, Phase: "none"}
	if binding.PeerID == "" {
		return out, nil
	}
	h.mu.Lock()
	p := h.peers[key(device, binding.PeerID)]
	h.mu.Unlock()
	if p == nil || p.ctx.Err() != nil {
		return out, nil
	}
	out.PeerID = p.id
	out.Phase = p.phase.Load().(string)
	out.GroupID = p.group
	out.CurrentVersion = p.version
	return out, nil
}
func validateOffer(offer webrtc.SessionDescription) error {
	if offer.Type != webrtc.SDPTypeOffer || offer.SDP == "" {
		return problem(400, "invalid_offer", "audio Opus offer required")
	}
	var desc sdp.SessionDescription
	if err := desc.Unmarshal([]byte(offer.SDP)); err != nil || len(desc.MediaDescriptions) != 1 {
		return problem(400, "invalid_offer", "one audio transceiver required")
	}
	md := desc.MediaDescriptions[0]
	if md.MediaName.Media != "audio" || md.MediaName.Port.Value == 0 {
		return problem(400, "invalid_offer", "one active audio transceiver required")
	}
	opus := false
	for _, a := range md.Attributes {
		if a.Key == "recvonly" || a.Key == "sendonly" || a.Key == "inactive" {
			return problem(400, "invalid_offer", "sendrecv audio required")
		}
		if a.Key == "rtpmap" {
			fields := strings.Fields(a.Value)
			if len(fields) == 2 && strings.EqualFold(fields[1], "opus/48000/2") {
				opus = true
			}
		}
	}
	if !opus {
		return problem(400, "invalid_offer", "Opus 48000 Hz required")
	}
	return nil
}
func (h *Hub) cleanRecords(device string, now time.Time) map[string]*record {
	records := h.records[device]
	if records == nil {
		records = map[string]*record{}
		h.records[device] = records
	}
	for id, r := range records {
		if r.status == "ended" && !now.Before(r.protectedUntil) {
			delete(records, id)
		}
	}
	return records
}
func (h *Hub) Create(ctx context.Context, device string, req CreateRequest) (CreateResponse, error) {
	if !validID(req.PeerID) || req.ReplacePeerID != "" && !validID(req.ReplacePeerID) {
		return CreateResponse{}, problem(400, "invalid_request", "valid Peer identifiers required")
	}
	if err := validateOffer(req.Offer); err != nil {
		return CreateResponse{}, err
	}
	req.ValidUntil = req.ValidUntil.UTC()
	encoded, _ := json.Marshal(req)
	hash := sha256.Sum256(encoded)
	now := h.now().UTC()
	h.mu.Lock()
	if h.closed {
		h.mu.Unlock()
		return CreateResponse{}, problem(503, "media_closed", "media service is shutting down")
	}
	records := h.cleanRecords(device, now)
	if r := records[req.PeerID]; r != nil {
		if r.hash != hash && r.hash != ([32]byte{}) {
			h.mu.Unlock()
			return CreateResponse{}, problem(409, "peer_conflict", "peer_id was used with a different payload")
		}
		if r.status == "active" && r.answer != nil && h.control.MediaPeerValid(device, req.PeerID) {
			out := *r.answer
			h.mu.Unlock()
			return out, nil
		}
		code := "peer_ended"
		if r.status == "negotiating" {
			code = "peer_negotiating"
		}
		h.mu.Unlock()
		return CreateResponse{}, problem(409, code, "Peer is negotiating, canceled or no longer active")
	}
	if len(records) >= MaxPeerRecords {
		h.mu.Unlock()
		return CreateResponse{}, problem(429, "peer_capacity", "too many protected Peer identifiers")
	}
	if req.ValidUntil.IsZero() || req.ValidUntil.After(now.Add(10*time.Second)) {
		h.mu.Unlock()
		return CreateResponse{}, problem(400, "invalid_deadline", "valid_until must be within ten seconds of server time")
	}
	if !now.Before(req.ValidUntil) {
		records[req.PeerID] = &record{hash: hash, status: "ended", protectedUntil: now.Add(Protection)}
		h.mu.Unlock()
		return CreateResponse{}, problem(409, "request_expired", "Peer creation deadline expired")
	}
	if err := ctx.Err(); err != nil {
		records[req.PeerID] = &record{hash: hash, status: "ended", protectedUntil: now.Add(Protection)}
		h.mu.Unlock()
		return CreateResponse{}, problem(408, "request_canceled", "Peer creation canceled")
	}
	old := h.peers[key(device, req.ReplacePeerID)]
	if len(h.peers) >= MaxPeers && old == nil {
		h.mu.Unlock()
		return CreateResponse{}, problem(429, "media_capacity", "media Peer capacity reached")
	}
	oldID, err := h.control.BindMediaPeer(device, req.PeerID, req.ReplacePeerID, req.GroupID, req.CurrentVersion, req.ServerEpoch)
	if err != nil {
		h.mu.Unlock()
		return CreateResponse{}, err
	}
	peerCtx, cancel := context.WithCancel(h.ctx)
	p := &peer{id: req.PeerID, device: device, group: req.GroupID, version: req.CurrentVersion, epoch: req.ServerEpoch, validUntil: req.ValidUntil, created: now, ctx: peerCtx, cancel: cancel, queue: newPacketQueue()}
	p.phase.Store("negotiating")
	records[req.PeerID] = &record{hash: hash, status: "negotiating", peer: p}
	h.peers[key(device, req.PeerID)] = p
	h.workers.Add(1)
	h.mu.Unlock()
	defer h.workers.Done()
	if oldID != "" && old != nil {
		h.end(old)
	}
	response, err := h.negotiate(ctx, p, req.Offer)
	if err != nil {
		h.end(p)
		return CreateResponse{}, err
	}
	h.mu.Lock()
	r := h.records[device][p.id]
	if h.closed || p.ctx.Err() != nil || r == nil || r.peer != p || r.status != "negotiating" || !h.now().Before(p.validUntil) || !h.control.MediaPeerValid(device, p.id) {
		h.mu.Unlock()
		h.end(p)
		return CreateResponse{}, problem(409, "peer_ended", "Peer was canceled or group changed during negotiation")
	}
	r.status = "active"
	r.answer = &response
	if p.phase.Load().(string) != "connected" {
		p.phase.Store("connecting")
	}
	h.mu.Unlock()
	return response, nil
}
func (h *Hub) negotiate(requestCtx context.Context, p *peer, offer webrtc.SessionDescription) (CreateResponse, error) {
	if p.ctx.Err() != nil {
		return CreateResponse{}, problem(409, "peer_ended", "Peer was canceled")
	}
	pc, err := h.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return CreateResponse{}, problem(500, "peer_failed", "cannot initialize Peer")
	}
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		_ = pc.Close()
		return CreateResponse{}, problem(409, "peer_ended", "Peer was canceled")
	}
	p.pc = pc
	p.mu.Unlock()
	down, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2}, "audio", "intercom")
	if err != nil {
		return CreateResponse{}, problem(500, "peer_failed", "cannot initialize audio track")
	}
	sender, err := pc.AddTrack(down)
	if err != nil {
		return CreateResponse{}, problem(500, "peer_failed", "cannot initialize sender")
	}
	p.down = down
	p.write = func(packet *outPacket) error { return down.WriteRTP(packet.packet) }
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		switch state {
		case webrtc.PeerConnectionStateConnected:
			if h.control.SetMediaReady(p.device, p.id) {
				p.phase.Store("connected")
			} else {
				go h.end(p)
			}
		case webrtc.PeerConnectionStateDisconnected, webrtc.PeerConnectionStateFailed, webrtc.PeerConnectionStateClosed:
			go h.end(p)
		}
	})
	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		if track.Kind() != webrtc.RTPCodecTypeAudio || !strings.EqualFold(track.Codec().MimeType, webrtc.MimeTypeOpus) || track.Codec().ClockRate != 48000 || !p.trackSeen.CompareAndSwap(false, true) {
			go h.end(p)
			return
		}
		h.start(p, func() { h.read(p, track) })
	})
	h.start(p, func() {
		for {
			if _, _, err := sender.ReadRTCP(); err != nil {
				return
			}
		}
	})
	h.start(p, func() { h.write(p) })
	if err = pc.SetRemoteDescription(offer); err != nil {
		return CreateResponse{}, problem(400, "invalid_offer", "cannot negotiate audio offer")
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		return CreateResponse{}, problem(400, "invalid_offer", "cannot answer audio offer")
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err = pc.SetLocalDescription(answer); err != nil {
		return CreateResponse{}, problem(400, "invalid_offer", "cannot set audio answer")
	}
	timeout := negotiationLimit
	if remain := p.validUntil.Sub(h.now()); remain < timeout {
		timeout = remain
	}
	if timeout <= 0 {
		return CreateResponse{}, problem(409, "request_expired", "Peer deadline expired")
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case <-requestCtx.Done():
		return CreateResponse{}, problem(408, "request_canceled", "Peer creation canceled")
	case <-p.ctx.Done():
		return CreateResponse{}, problem(409, "peer_ended", "Peer creation canceled")
	case <-timer.C:
		return CreateResponse{}, problem(408, "negotiation_timeout", "Peer negotiation timed out")
	case <-gathered:
	}
	if requestCtx.Err() != nil {
		return CreateResponse{}, problem(408, "request_canceled", "Peer creation canceled")
	}
	local := pc.LocalDescription()
	if local == nil {
		return CreateResponse{}, problem(500, "peer_failed", "audio answer unavailable")
	}
	return CreateResponse{PeerID: p.id, GroupID: p.group, CurrentVersion: p.version, ServerEpoch: p.epoch, Answer: *local}, nil
}
func (h *Hub) Delete(device, id string) error {
	if !validID(id) {
		return problem(400, "invalid_request", "invalid peer_id")
	}
	if _, _, err := h.control.MediaBinding(device); err != nil {
		return err
	}
	h.mu.Lock()
	records := h.cleanRecords(device, h.now().UTC())
	r := records[id]
	if r == nil {
		if len(records) >= MaxPeerRecords {
			h.mu.Unlock()
			return problem(429, "peer_capacity", "too many protected Peer identifiers")
		}
		r = &record{}
		records[id] = r
	}
	p := r.peer
	r.status = "ended"
	r.protectedUntil = h.now().Add(Protection)
	h.mu.Unlock()
	h.control.UnbindMediaPeer(device, id)
	if p != nil {
		h.end(p)
	}
	return nil
}
func (h *Hub) start(p *peer, fn func()) {
	h.mu.Lock()
	if h.closed || p.ctx.Err() != nil {
		h.mu.Unlock()
		return
	}
	h.workers.Add(1)
	h.mu.Unlock()
	go func() { defer h.workers.Done(); fn() }()
}
func (h *Hub) end(p *peer) {
	p.once.Do(func() {
		p.cancel()
		h.control.UnbindMediaPeer(p.device, p.id)
		h.mu.Lock()
		if h.peers[key(p.device, p.id)] == p {
			delete(h.peers, key(p.device, p.id))
		}
		if r := h.records[p.device][p.id]; r != nil && r.peer == p {
			r.status = "ended"
			r.protectedUntil = h.now().Add(Protection)
		}
		h.mu.Unlock()
		p.queue.clear()
		p.mu.Lock()
		p.closed = true
		pc := p.pc
		p.mu.Unlock()
		if pc != nil {
			_ = pc.Close()
		}
	})
}
func (h *Hub) watch() {
	defer h.workers.Done()
	ticker := time.NewTicker(25 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-h.ctx.Done():
			return
		case <-ticker.C:
			h.mu.Lock()
			peers := make([]*peer, 0, len(h.peers))
			for _, p := range h.peers {
				peers = append(peers, p)
			}
			h.mu.Unlock()
			now := h.now()
			for _, p := range peers {
				phase := p.phase.Load().(string)
				writing := p.writeStarted.Load()
				if !h.control.MediaPeerValid(p.device, p.id) || phase == "negotiating" && !now.Before(p.validUntil) || phase == "connecting" && now.Sub(p.created) > connectionLimit || writing != 0 && time.Since(time.Unix(0, writing)) > writeBudget {
					h.end(p)
				}
			}
		}
	}
}
func (h *Hub) Close() {
	h.mu.Lock()
	if h.closed {
		h.mu.Unlock()
		return
	}
	h.closed = true
	h.cancel()
	peers := make([]*peer, 0, len(h.peers))
	for _, p := range h.peers {
		peers = append(peers, p)
	}
	h.mu.Unlock()
	for _, p := range peers {
		h.end(p)
	}
	h.workers.Wait()
}
