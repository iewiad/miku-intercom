package httpapi

import (
	"crypto/tls"
	"embed"
	"encoding/json"
	"errors"
	"io"
	"mime"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/lan"
	"github.com/iewiad/miku-intercom/server/internal/media"
	"github.com/iewiad/miku-intercom/server/internal/rta"
)

//go:embed web/*
var web embed.FS

type Server struct {
	service         *control.Service
	media           *media.Hub
	deviceHost      string
	voice           *rta.Proxy
	voiceAdmissions sync.Map
	voiceLogf       func(string, ...any)
}

func New(service *control.Service, hubs ...*media.Hub) http.Handler {
	s := &Server{service: service}
	if len(hubs) > 0 {
		s.media = hubs[0]
	}
	return s
}

// NewWithVoice opts the localhost product into a private RTA signalling proxy.
func NewWithVoice(service *control.Service, hub *media.Hub, proxy *rta.Proxy) http.Handler {
	return &Server{service: service, media: hub, voice: proxy}
}

// NewDevice exposes only authenticated APIs to an explicitly configured native TLS client.
func NewDevice(service *control.Service, hub *media.Hub, address string) (http.Handler, error) {
	if _, err := lan.ParseAddress(address); err != nil {
		return nil, err
	}
	return &Server{service: service, media: hub, deviceHost: address}, nil
}

// Native clients omit Origin entirely. Inspect every value, including empty and duplicate headers.
func nativeDeviceRequest(r *http.Request) bool {
	var sites []string
	present := false
	for name, values := range r.Header {
		if strings.EqualFold(name, "Origin") {
			return false
		}
		if strings.EqualFold(name, "Sec-Fetch-Site") {
			present = true
			sites = append(sites, values...)
		}
	}
	return !present || (len(sites) == 1 && (sites[0] == "none" || sites[0] == "same-origin"))
}

func send(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
func errorResponse(w http.ResponseWriter, err error) {
	var e *control.Error
	if !errors.As(err, &e) {
		e = &control.Error{Status: 500, Code: "internal_error", Message: "request failed"}
	}
	send(w, e.Status, map[string]any{"error": map[string]string{"code": e.Code, "message": e.Message}})
}
func invalid(code, message string, status int) error {
	return &control.Error{Status: status, Code: code, Message: message}
}
func localHost(host string) bool {
	u, err := url.Parse("//" + host)
	if err != nil || u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" || u.Host != host {
		return false
	}
	h := u.Hostname()
	if strings.EqualFold(h, "localhost") {
		return true
	}
	ip := net.ParseIP(h)
	return ip != nil && ip.IsLoopback()
}

func sameOrigin(r *http.Request) bool {
	if site := r.Header.Get("Sec-Fetch-Site"); site != "" && site != "same-origin" && site != "none" {
		return false
	}
	origin := r.Header.Get("Origin")
	if origin == "" {
		return true
	}
	u, err := url.Parse(origin)
	if err != nil {
		return false
	}
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	return u.Scheme == scheme && strings.EqualFold(u.Host, r.Host) && u.User == nil && u.Path == "" && u.RawQuery == "" && u.Fragment == ""
}
func decode(w http.ResponseWriter, r *http.Request, dst any) error {
	return decodeLimit(w, r, dst, 8*1024)
}
func decodeLimit(w http.ResponseWriter, r *http.Request, dst any, limitBytes int64) error {
	typ, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || typ != "application/json" {
		return invalid("unsupported_media_type", "Content-Type must be application/json", 415)
	}
	r.Body = http.MaxBytesReader(w, r.Body, limitBytes)
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err = dec.Decode(dst); err != nil {
		var limit *http.MaxBytesError
		if errors.As(err, &limit) {
			return invalid("body_too_large", "request body exceeds limit", 413)
		}
		return invalid("invalid_json", "valid JSON with only documented fields required", 400)
	}
	if err = dec.Decode(new(any)); err != io.EOF {
		var limit *http.MaxBytesError
		if errors.As(err, &limit) {
			return invalid("body_too_large", "request body exceeds limit", 413)
		}
		return invalid("invalid_json", "exactly one JSON value required", 400)
	}
	return nil
}
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if s.deviceHost != "" {
		if r.TLS == nil || r.TLS.Version < tls.VersionTLS12 {
			errorResponse(w, invalid("tls_required", "TLS 1.2 or newer required", 403))
			return
		}
		if r.Host != s.deviceHost {
			errorResponse(w, invalid("host_forbidden", "configured device Host required", 403))
			return
		}
		if !nativeDeviceRequest(r) {
			errorResponse(w, invalid("origin_forbidden", "native device request required", 403))
			return
		}
		if r.URL.Path == "/api/v1/devices" {
			errorResponse(w, invalid("bootstrap_forbidden", "provision device identity through localhost", 403))
			return
		}
		if !strings.HasPrefix(r.URL.Path, "/api/v1/") || r.URL.RawPath != "" {
			errorResponse(w, invalid("not_found", "resource not found", 404))
			return
		}
	} else {
		if !localHost(r.Host) {
			errorResponse(w, invalid("host_forbidden", "localhost or loopback Host required", 403))
			return
		}
	}
	static := map[string]string{"/": "index.html", "/app.js": "app.js", "/style.css": "style.css", "/audio.js": "audio.js", "/voice.js": "voice.js", "/recommendation.js": "recommendation.js"}
	if s.media != nil && s.media.DiagnosticsEnabled() {
		static["/diagnostics"] = "diagnostics.html"
		static["/diagnostics.js"] = "diagnostics.js"
		static["/lifecycle.js"] = "lifecycle.js"
	}
	if file, ok := static[r.URL.Path]; ok && s.deviceHost == "" {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", "GET")
			errorResponse(w, invalid("method_not_allowed", "method not allowed", 405))
			return
		}
		b, err := web.ReadFile("web/" + file)
		if err != nil {
			errorResponse(w, err)
			return
		}
		typ := map[string]string{"index.html": "text/html; charset=utf-8", "app.js": "text/javascript; charset=utf-8", "style.css": "text/css; charset=utf-8", "audio.js": "text/javascript; charset=utf-8", "voice.js": "text/javascript; charset=utf-8", "recommendation.js": "text/javascript; charset=utf-8", "diagnostics.html": "text/html; charset=utf-8", "diagnostics.js": "text/javascript; charset=utf-8", "lifecycle.js": "text/javascript; charset=utf-8"}[file]
		w.Header().Set("Content-Type", typ)
		_, _ = w.Write(b)
		return
	}
	if s.deviceHost == "" && !sameOrigin(r) {
		errorResponse(w, invalid("origin_forbidden", "same-origin request required", 403))
		return
	}
	path := strings.TrimPrefix(r.URL.Path, "/api/v1")
	methods := map[string]string{"/devices": "POST", "/state": "GET", "/groups": "POST", "/memberships": "POST", "/current-group": "PUT", "/floor/acquire": "POST", "/floor/renew": "POST", "/floor/release": "POST", "/voice/resolve": "POST", "/voice": "GET", "/voice/sessions": "POST", "/recommendations": "GET", "/recommendations/confirm": "PUT"}
	method, ok := methods[path]
	if path == "/recommendations" {
		method = "GET, POST"
		if r.Method == "GET" || r.Method == "POST" {
			method = r.Method
		}
	}
	if strings.HasPrefix(path, "/voice/sessions/") {
		parts := strings.Split(strings.TrimPrefix(path, "/voice/sessions/"), "/")
		if len(parts) == 2 && (parts[1] == "offer" || parts[1] == "stop") {
			method = "POST"
			ok = true
		}
	}
	if s.media != nil {
		if path == "/media" {
			method = "GET"
			ok = true
		}
		if path == "/media/peers" {
			method = "POST"
			ok = true
		}
		if strings.HasPrefix(path, "/media/peers/") && !strings.Contains(strings.TrimPrefix(path, "/media/peers/"), "/") {
			method = "DELETE"
			ok = true
		}
	}
	if strings.HasPrefix(path, "/operations/") && !strings.Contains(strings.TrimPrefix(path, "/operations/"), "/") {
		method = "GET"
		ok = true
	}
	if !strings.HasPrefix(r.URL.Path, "/api/v1/") || !ok {
		errorResponse(w, invalid("not_found", "resource not found", 404))
		return
	}
	if r.Method != method {
		w.Header().Set("Allow", method)
		errorResponse(w, invalid("method_not_allowed", "method not allowed", 405))
		return
	}
	var id string
	if path != "/devices" {
		auth := r.Header.Get("Authorization")
		token := ""
		parts := strings.Fields(auth)
		if len(parts) == 2 && strings.EqualFold(parts[0], "Bearer") {
			token = parts[1]
		}
		var err error
		id, err = s.service.Authenticate(token)
		if err != nil {
			errorResponse(w, err)
			return
		}
	}
	if err := s.handle(w, r, path, id); err != nil {
		errorResponse(w, err)
	}
}
func (s *Server) handle(w http.ResponseWriter, r *http.Request, path, id string) error {
	if path == "/recommendations" || path == "/recommendations/confirm" {
		// Both native TLS and localhost clients use the same authenticated
		// candidate snapshot and atomic confirmation transaction. The native
		// Host/Origin/TLS gate above remains mandatory for every request.
		return s.handleRecommendations(w, r, path, id)
	}
	if s.voice != nil && (path == "/voice/sessions" || path == "/media/peers") {
		value, _ := s.voiceAdmissions.LoadOrStore(id, &sync.Mutex{})
		gate := value.(*sync.Mutex)
		gate.Lock()
		defer gate.Unlock()
		if path == "/media/peers" && s.voice.Status(id).CommandID != "" {
			return invalid("voice_session_pending", "voice cleanup must be confirmed before group audio", 409)
		}
		if path == "/voice/sessions" && s.media != nil {
			status, e := s.media.Status(id)
			if e != nil {
				return e
			}
			if status.PeerID != "" {
				return invalid("group_peer_pending", "close group audio before voice command", 409)
			}
		}
	}
	if path == "/voice" || strings.HasPrefix(path, "/voice/") {
		return s.handleVoice(w, r, path, id)
	}
	if s.media != nil && (path == "/media" || path == "/media/peers" || strings.HasPrefix(path, "/media/peers/")) {
		return s.handleMedia(w, r, path, id)
	}
	switch path {
	case "/devices":
		var req struct {
			Name string `json:"name"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		d, t, err := s.service.CreateDevice(req.Name)
		if err != nil {
			return err
		}
		send(w, 201, map[string]any{"device": d, "token": t})
	case "/state":
		state, err := s.service.State(id)
		if err != nil {
			return err
		}
		send(w, 200, s.stateView(state))
	case "/groups":
		var req struct {
			Name    string       `json:"name"`
			Purpose groupPurpose `json:"purpose"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		g, c, state, err := s.service.CreateGroupWithPurpose(id, req.Name, string(req.Purpose))
		if err != nil {
			return err
		}
		send(w, 201, map[string]any{"group": s.groupView(g), "code": c, "state": s.stateView(state)})
	case "/memberships":
		var req struct {
			Code string `json:"code"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		state, err := s.service.Join(id, req.Code)
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"state": s.stateView(state)})
	case "/current-group":
		var req struct {
			OperationID     string  `json:"operation_id"`
			GroupID         string  `json:"group_id"`
			ExpectedVersion *uint64 `json:"expected_version"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		if req.ExpectedVersion == nil {
			return invalid("invalid_request", "expected_version required", 400)
		}
		op, state, err := s.service.Switch(id, control.SwitchRequest{OperationID: req.OperationID, GroupID: req.GroupID, ExpectedVersion: *req.ExpectedVersion})
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"operation": op, "state": s.stateView(state)})
	case "/floor/acquire", "/floor/renew", "/floor/release":
		return s.handleFloor(w, r, path, id)
	default:
		op, err := s.service.Operation(id, strings.TrimPrefix(path, "/operations/"))
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"operation": op})
	}
	return nil
}
func (s *Server) handleFloor(w http.ResponseWriter, r *http.Request, path, id string) error {
	// Keep separate wire types so a lease field on acquire/release is rejected too.
	var base control.FloorRequest
	switch path {
	case "/floor/acquire":
		var req struct {
			GroupID        string          `json:"group_id"`
			PeerID         string          `json:"peer_id"`
			CurrentVersion *uint64         `json:"current_version"`
			RequestID      string          `json:"request_id"`
			ServerEpoch    string          `json:"server_epoch"`
			ValidUntil     json.RawMessage `json:"valid_until"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		if req.CurrentVersion == nil {
			return invalid("invalid_request", "current_version required", 400)
		}
		parsed := control.AcquireRequest{FloorRequest: control.FloorRequest{PeerID: req.PeerID, GroupID: req.GroupID, CurrentVersion: *req.CurrentVersion, RequestID: req.RequestID, ServerEpoch: req.ServerEpoch}}
		if len(req.ValidUntil) == 0 || json.Unmarshal(req.ValidUntil, &parsed.ValidUntil) != nil {
			return invalid("invalid_deadline", "valid_until must be RFC3339", 400)
		}
		floor, err := s.service.Acquire(id, parsed)
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"floor": floor})
	case "/floor/renew":
		var req struct {
			GroupID        string  `json:"group_id"`
			PeerID         string  `json:"peer_id"`
			CurrentVersion *uint64 `json:"current_version"`
			RequestID      string  `json:"request_id"`
			ServerEpoch    string  `json:"server_epoch"`
			LeaseID        string  `json:"lease_id"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		if req.CurrentVersion == nil {
			return invalid("invalid_request", "current_version required", 400)
		}
		base = control.FloorRequest{PeerID: req.PeerID, GroupID: req.GroupID, CurrentVersion: *req.CurrentVersion, RequestID: req.RequestID, ServerEpoch: req.ServerEpoch}
		floor, err := s.service.Renew(id, control.RenewRequest{FloorRequest: base, LeaseID: req.LeaseID})
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"floor": floor})
	case "/floor/release":
		var req struct {
			GroupID        string  `json:"group_id"`
			PeerID         string  `json:"peer_id"`
			CurrentVersion *uint64 `json:"current_version"`
			RequestID      string  `json:"request_id"`
			ServerEpoch    string  `json:"server_epoch"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		if req.CurrentVersion == nil {
			return invalid("invalid_request", "current_version required", 400)
		}
		base = control.FloorRequest{PeerID: req.PeerID, GroupID: req.GroupID, CurrentVersion: *req.CurrentVersion, RequestID: req.RequestID, ServerEpoch: req.ServerEpoch}
		if err := s.service.Release(id, base); err != nil {
			return err
		}
		send(w, 200, map[string]bool{"released": true})
	}
	return nil
}
