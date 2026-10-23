package httpapi

import (
	"embed"
	"encoding/json"
	"errors"
	"io"
	"mime"
	"net"
	"net/http"
	"net/url"
	"strings"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

//go:embed web/*
var web embed.FS

type Server struct{ service *control.Service }

func New(service *control.Service) http.Handler { return &Server{service: service} }
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
	typ, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || typ != "application/json" {
		return invalid("unsupported_media_type", "Content-Type must be application/json", 415)
	}
	r.Body = http.MaxBytesReader(w, r.Body, 8*1024)
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err = dec.Decode(dst); err != nil {
		var limit *http.MaxBytesError
		if errors.As(err, &limit) {
			return invalid("body_too_large", "request body exceeds 8 KiB", 413)
		}
		return invalid("invalid_json", "valid JSON with only documented fields required", 400)
	}
	if err = dec.Decode(new(any)); err != io.EOF {
		var limit *http.MaxBytesError
		if errors.As(err, &limit) {
			return invalid("body_too_large", "request body exceeds 8 KiB", 413)
		}
		return invalid("invalid_json", "exactly one JSON value required", 400)
	}
	return nil
}
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if !localHost(r.Host) {
		errorResponse(w, invalid("host_forbidden", "localhost or loopback Host required", 403))
		return
	}
	static := map[string]string{"/": "index.html", "/app.js": "app.js", "/style.css": "style.css"}
	if file, ok := static[r.URL.Path]; ok {
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
		typ := map[string]string{"index.html": "text/html; charset=utf-8", "app.js": "text/javascript; charset=utf-8", "style.css": "text/css; charset=utf-8"}[file]
		w.Header().Set("Content-Type", typ)
		_, _ = w.Write(b)
		return
	}
	if !sameOrigin(r) {
		errorResponse(w, invalid("origin_forbidden", "same-origin request required", 403))
		return
	}
	path := strings.TrimPrefix(r.URL.Path, "/api/v1")
	methods := map[string]string{"/devices": "POST", "/state": "GET", "/groups": "POST", "/memberships": "POST", "/current-group": "PUT", "/floor/acquire": "POST", "/floor/renew": "POST", "/floor/release": "POST"}
	method, ok := methods[path]
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
		send(w, 200, state)
	case "/groups":
		var req struct {
			Name string `json:"name"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		g, c, state, err := s.service.CreateGroup(id, req.Name)
		if err != nil {
			return err
		}
		send(w, 201, map[string]any{"group": g, "code": c, "state": state})
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
		send(w, 200, map[string]any{"state": state})
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
		send(w, 200, map[string]any{"operation": op, "state": state})
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
		parsed := control.AcquireRequest{FloorRequest: control.FloorRequest{GroupID: req.GroupID, CurrentVersion: *req.CurrentVersion, RequestID: req.RequestID, ServerEpoch: req.ServerEpoch}}
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
		base = control.FloorRequest{GroupID: req.GroupID, CurrentVersion: *req.CurrentVersion, RequestID: req.RequestID, ServerEpoch: req.ServerEpoch}
		floor, err := s.service.Renew(id, control.RenewRequest{FloorRequest: base, LeaseID: req.LeaseID})
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"floor": floor})
	case "/floor/release":
		var req struct {
			GroupID        string  `json:"group_id"`
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
		base = control.FloorRequest{GroupID: req.GroupID, CurrentVersion: *req.CurrentVersion, RequestID: req.RequestID, ServerEpoch: req.ServerEpoch}
		if err := s.service.Release(id, base); err != nil {
			return err
		}
		send(w, 200, map[string]bool{"released": true})
	}
	return nil
}
