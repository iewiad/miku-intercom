// Package rta is an opt-in localhost signalling proxy for the existing legacy RTA route.
// API and endpoint tokens never leave the server. Audio goes directly over WebRTC.
package rta

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/persist"
)

type Config struct {
	RTICURL       string `json:"rtic_url"`
	Token         string `json:"token"`
	AppID         string `json:"app_id"`
	RTCOrigin     string `json:"rtc_origin,omitempty"`
	RTCNodeDomain string `json:"rtc_node_domain,omitempty"`
	RTCNodePort   int    `json:"rtc_node_port,omitempty"`
	ModelProfile  string `json:"model_profile"`
	RoleProfile   string `json:"role_profile"`
	VoiceProfile  string `json:"voice_profile"`
	Region        string `json:"region"`
}

func origin(value string) (*url.URL, error) {
	u, e := url.Parse(value)
	if e != nil || u.Scheme != "https" || u.Host == "" || u.User != nil || (u.Path != "" && u.Path != "/") || u.RawQuery != "" || u.Fragment != "" || strings.ContainsAny(value, "?#") {
		return nil, errors.New("voice config requires HTTPS origins")
	}
	u.Path = ""
	return u, nil
}
func Load(path string, journalPath ...string) (*Proxy, error) {
	info, e := os.Lstat(path)
	if e != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
		return nil, errors.New("voice config must be a private regular file (0600)")
	}
	f, e := os.Open(path)
	if e != nil {
		return nil, errors.New("cannot open voice config")
	}
	defer f.Close()
	var c Config
	d := json.NewDecoder(io.LimitReader(f, 16385))
	d.DisallowUnknownFields()
	if d.Decode(&c) != nil || d.Decode(new(any)) != io.EOF {
		return nil, errors.New("invalid voice config")
	}
	p, e := New(c, nil)
	if e != nil {
		return nil, e
	}
	if len(journalPath) > 0 {
		if e = p.openJournal(journalPath[0]); e != nil {
			return nil, e
		}
	}
	return p, nil
}
func New(c Config, client *http.Client) (*Proxy, error) {
	base, e := origin(c.RTICURL)
	if e != nil {
		return nil, e
	}
	var rtc *url.URL
	if c.RTCOrigin != "" {
		if c.RTCNodeDomain != "" || c.RTCNodePort != 0 {
			return nil, errors.New("choose either fixed RTC origin or explicit numeric node policy")
		}
		rtc, e = origin(c.RTCOrigin)
		if e != nil {
			return nil, e
		}
	} else {
		c.RTCNodeDomain = strings.ToLower(c.RTCNodeDomain)
		if !nodeDomain.MatchString(c.RTCNodeDomain) || len(c.RTCNodeDomain) > 253 || !strings.ContainsAny(c.RTCNodeDomain, "abcdefghijklmnopqrstuvwxyz") || c.RTCNodePort < 1 || c.RTCNodePort > 65535 {
			return nil, errors.New("voice config requires fixed RTC origin or explicit numeric node domain and port")
		}
	}
	for _, v := range []string{c.Token, c.AppID, c.ModelProfile, c.RoleProfile, c.VoiceProfile, c.Region} {
		if v == "" || len(v) > 4096 || strings.ContainsAny(v, "\r\n") {
			return nil, errors.New("voice config requires all explicit fields")
		}
	}
	if client == nil {
		client = &http.Client{Timeout: 6 * time.Second}
	}
	clone := *client
	clone.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return &Proxy{config: c, base: base, rtc: rtc, client: &clone, records: map[string]*record{}, active: map[string]string{}, done: make(chan struct{})}, nil
}

type Proxy struct {
	mu        sync.Mutex
	config    Config
	base, rtc *url.URL
	client    *http.Client
	records   map[string]*record
	active    map[string]string
	done      chan struct{}
	closeOnce sync.Once
	closed    bool
	journal   *persist.File
	durable   map[string]journalEntry
}
type record struct {
	mu                                  sync.Mutex
	id, session, endpoint, token, phase string
	offer                               []byte
	answer                              json.RawMessage
}
type Status struct {
	Available bool   `json:"available"`
	CommandID string `json:"command_id"`
	Phase     string `json:"phase"`
}

var validID = regexp.MustCompile(`^[A-Za-z0-9_-]{8,80}$`)
var nodeDomain = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$`)
var numericNode = regexp.MustCompile(`^[0-9]{1,63}$`)

func (p *Proxy) rtcPolicy() string {
	if p.rtc != nil {
		return p.rtc.String()
	}
	return "numeric-nodes:" + p.config.RTCNodeDomain + ":" + strconv.Itoa(p.config.RTCNodePort)
}
func (p *Proxy) endpoint(raw, id string) (*url.URL, error) {
	u, e := url.Parse(raw)
	// Check the received path before ResolveReference can normalize dot segments.
	if e != nil || u.Path != "/v1/realtime/sessions/"+id+"/rtc" || u.User != nil || u.RawPath != "" || strings.ContainsAny(raw, "?#") {
		return nil, errors.New("invalid RTC endpoint")
	}
	u = p.base.ResolveReference(u)
	if u.Scheme != "https" || u.User != nil || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" {
		return nil, errors.New("invalid RTC endpoint")
	}
	if p.rtc != nil {
		if !strings.EqualFold(u.Host, p.rtc.Host) {
			return nil, errors.New("RTC origin not authorized")
		}
	} else {
		suffix := "." + p.config.RTCNodeDomain
		host := strings.ToLower(u.Hostname())
		if !strings.HasSuffix(host, suffix) || !numericNode.MatchString(strings.TrimSuffix(host, suffix)) || u.Port() != strconv.Itoa(p.config.RTCNodePort) {
			return nil, errors.New("RTC numeric node not authorized")
		}
	}
	return u, nil
}

func fail(status int, code string) error {
	return &control.Error{Status: status, Code: code, Message: "voice session could not be confirmed"}
}
func key(device, id string) string { return device + "/" + id }
func (p *Proxy) Status(device string) Status {
	p.mu.Lock()
	id := p.active[device]
	r := p.records[key(device, id)]
	p.mu.Unlock()
	st := Status{Available: true, CommandID: id, Phase: "none"}
	if r != nil {
		r.mu.Lock()
		st.Phase = r.phase
		r.mu.Unlock()
	}
	return st
}
func (p *Proxy) exchange(ctx context.Context, endpoint, token string, payload any) ([]byte, int, error) {
	return p.exchangeMethod(ctx, http.MethodPost, endpoint, token, payload)
}
func (p *Proxy) exchangeMethod(ctx context.Context, method, endpoint, token string, payload any) ([]byte, int, error) {
	var input io.Reader
	if payload != nil {
		b, e := json.Marshal(payload)
		if e != nil {
			return nil, 0, e
		}
		input = bytes.NewReader(b)
	}
	req, e := http.NewRequestWithContext(ctx, method, endpoint, input)
	if e != nil {
		return nil, 0, e
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+token)
	resp, e := p.client.Do(req)
	if e != nil {
		return nil, 0, e
	}
	defer resp.Body.Close()
	body, e := io.ReadAll(io.LimitReader(resp.Body, 65537))
	if e != nil || len(body) > 65536 {
		return nil, resp.StatusCode, errors.New("oversize upstream response")
	}
	return body, resp.StatusCode, nil
}
func (p *Proxy) Start(device, id string) error {
	return p.start(device, id, false)
}

// StartText shares the original approved route, admission and durable cleanup.
// Recommendation turns explicitly disable provider search at session creation.
func (p *Proxy) StartText(device, id string) error {
	return p.start(device, id, true)
}

func (p *Proxy) start(device, id string, text bool) error {
	if !validID.MatchString(id) {
		return fail(400, "invalid_request")
	}
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return fail(503, "voice_unavailable")
	}
	if old := p.active[device]; old != "" && old != id {
		p.mu.Unlock()
		return fail(409, "voice_session_pending")
	}
	r := p.records[key(device, id)]
	if r == nil {
		if len(p.records) >= 4096 || len(p.active) >= 32 {
			p.mu.Unlock()
			return fail(429, "voice_capacity")
		}
		r = &record{id: id, phase: "creating"}
		p.records[key(device, id)] = r
		p.active[device] = id
		go p.reap(device, id)
	}
	p.mu.Unlock()
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.phase == "ready" {
		return nil
	}
	if r.phase != "creating" {
		return fail(409, "voice_session_pending")
	}
	// Do not tie upstream creation to a disappearing browser request. Its result must be retained for stop.
	r.phase = "create_unconfirmed"
	if e := p.saveRecord(device, r, false); e != nil {
		return e
	}
	ctx, cancel := context.WithTimeout(context.Background(), 7*time.Second)
	defer cancel()
	payload := map[string]any{"app_id": p.config.AppID, "user_id": device, "client_id": device, "model_profile": p.config.ModelProfile, "role_profile": p.config.RoleProfile, "voice_profile": p.config.VoiceProfile, "scenario": "shenglu_voice_switch", "client_transport": "platform_rtc", "region": p.config.Region}
	if text {
		payload["enable_search"] = false
		payload["scenario"] = "shenglu_group_recommendation"
	}
	body, status, e := p.exchange(ctx, p.base.String()+"/rtic/v1/realtime/sessions", p.config.Token, payload)
	var created struct {
		ID       string `json:"session_id"`
		Endpoint struct {
			URL     string `json:"url"`
			Token   string `json:"access_token"`
			Expires int64  `json:"expires_at_ms"`
		} `json:"client_endpoint"`
	}
	decoded := json.Unmarshal(body, &created) == nil
	if decoded && validID.MatchString(created.ID) {
		r.session = created.ID
		if e := p.saveRecord(device, r, false); e != nil {
			return e
		}
	}
	if e != nil || status < 200 || status >= 300 || !decoded || r.session == "" {
		return fail(502, "voice_create_unconfirmed")
	}
	u, e := p.endpoint(created.Endpoint.URL, r.session)
	if e != nil || created.Endpoint.Token == "" || created.Endpoint.Expires <= time.Now().UnixMilli() {
		return fail(502, "voice_endpoint_invalid")
	}
	r.endpoint = u.String()
	r.token = created.Endpoint.Token
	r.phase = "ready"
	return nil
}
func (p *Proxy) get(device, id string) (*record, error) {
	if !validID.MatchString(id) {
		return nil, fail(400, "invalid_request")
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	r := p.records[key(device, id)]
	if r == nil {
		return nil, fail(404, "voice_session_unknown")
	}
	return r, nil
}
func (p *Proxy) Offer(ctx context.Context, device, id string, offer json.RawMessage) (json.RawMessage, error) {
	r, e := p.get(device, id)
	if e != nil {
		return nil, e
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.phase != "ready" {
		return nil, fail(409, "voice_session_pending")
	}
	if r.offer != nil {
		if !bytes.Equal(r.offer, offer) {
			return nil, fail(409, "voice_offer_conflict")
		}
		if r.answer != nil {
			return r.answer, nil
		}
		return nil, fail(502, "voice_offer_unconfirmed")
	}
	r.offer = append([]byte(nil), offer...)
	body, status, e := p.exchange(ctx, r.endpoint, r.token, json.RawMessage(offer))
	var answer struct {
		Type      string `json:"type"`
		SDP       string `json:"sdp"`
		SessionID string `json:"session_id"`
	}
	if e != nil || status < 200 || status >= 300 || json.Unmarshal(body, &answer) != nil || answer.Type != "answer" || answer.SDP == "" || len(answer.SDP) > 60000 || (answer.SessionID != "" && answer.SessionID != r.session) {
		return nil, fail(502, "voice_offer_unconfirmed")
	}
	// Return only SDP, never unexpected upstream fields or credentials.
	r.answer, _ = json.Marshal(map[string]string{"type": "answer", "sdp": answer.SDP})
	return r.answer, nil
}
func (p *Proxy) Stop(device, id string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 7*time.Second)
	defer cancel()
	return p.StopContext(ctx, device, id)
}

// StopContext preserves the same terminal verification while letting a caller
// bound several asynchronous stop checks by one overall cleanup deadline.
func (p *Proxy) StopContext(ctx context.Context, device, id string) error {
	if !validID.MatchString(id) {
		return fail(400, "invalid_request")
	}
	p.mu.Lock()
	r := p.records[key(device, id)]
	if r == nil {
		if len(p.records) >= 4096 {
			p.mu.Unlock()
			return fail(429, "voice_capacity")
		}
		r = &record{id: id, phase: "stopped"}
		p.records[key(device, id)] = r
	}
	p.mu.Unlock()
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.phase == "stopped" {
		return p.saveRecord(device, r, true)
	}
	if r.session == "" {
		return fail(503, "voice_cleanup_unconfirmed")
	}
	_, status, e := p.exchange(ctx, p.base.String()+"/rtic/v1/realtime/sessions/"+r.session+"/stop", p.config.Token, map[string]string{"reason": "voice_command_finished"})
	if e != nil || status < 200 || status >= 300 {
		r.phase = "stop_unconfirmed"
		return fail(503, "voice_cleanup_unconfirmed")
	}
	// Stop is asynchronous: a 2xx acknowledgement (including 202) does not prove
	// the provider has ended. Query the same RTIC origin with the API key and verify
	// the actual session identity and terminal state before releasing the admission gate.
	body, queryStatus, queryErr := p.exchangeMethod(ctx, http.MethodGet, p.base.String()+"/rtic/v1/realtime/sessions/"+r.session, p.config.Token, nil)
	var queried struct {
		Session struct {
			ID    string `json:"session_id"`
			State string `json:"state"`
		} `json:"session"`
	}
	if queryErr != nil || queryStatus < 200 || queryStatus >= 300 || json.Unmarshal(body, &queried) != nil || queried.Session.ID != r.session || (queried.Session.State != "ended" && queried.Session.State != "failed" && queried.Session.State != "expired") {
		r.phase = "stop_unconfirmed"
		return fail(503, "voice_cleanup_unconfirmed")
	}
	if e := p.saveRecord(device, r, true); e != nil {
		r.phase = "stop_unconfirmed"
		return e
	}
	r.phase = "stopped"
	r.token = ""
	r.endpoint = ""
	r.answer = nil
	r.offer = nil
	p.mu.Lock()
	if p.active[device] == id {
		delete(p.active, device)
	}
	p.mu.Unlock()
	return nil
}

// A vanished browser must not keep a known session alive indefinitely. Unknown creation
// results stay blocked; neither timeout nor a failed stop is evidence of cleanup.
func (p *Proxy) reap(device, id string) {
	timer := time.NewTimer(45 * time.Second)
	defer timer.Stop()
	for {
		select {
		case <-p.done:
			return
		case <-timer.C:
			if p.Stop(device, id) == nil {
				return
			}
			timer.Reset(5 * time.Second)
		}
	}
}
func (p *Proxy) Close() {
	p.closeOnce.Do(func() {
		p.mu.Lock()
		p.closed = true
		close(p.done)
		ids := map[string]string{}
		for device, id := range p.active {
			ids[device] = id
		}
		p.mu.Unlock()
		var wg sync.WaitGroup
		for device, id := range ids {
			wg.Add(1)
			go func(device, id string) { defer wg.Done(); _ = p.Stop(device, id) }(device, id)
		}
		wg.Wait()
		if p.journal != nil {
			_ = p.journal.Close()
		}
	})
}
