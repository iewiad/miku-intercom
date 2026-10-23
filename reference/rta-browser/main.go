package main

import (
	"bytes"
	"context"
	"embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/textproto"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

//go:embed index.html
var staticFiles embed.FS

type server struct {
	client                *http.Client
	diagnoseOrigin        *url.URL
	inputRate, outputRate int
}
type clientConfig struct {
	RTICBaseURL *url.URL
	Token       string
	AppID       string
}
type sessionRequest struct {
	Model             string `json:"model"`
	Voice             string `json:"voice"`
	Transport         string `json:"transport"`
	CustomHeaderKey   string `json:"custom_header_key"`
	CustomHeaderValue string `json:"custom_header_value"`
}

const (
	rticBaseURLHeader = "X-RTIC-Base-URL"
	rticTokenHeader   = "X-RTIC-API-Token"
	rtiAppIDHeader    = "X-RTI-App-ID"
	defaultListenAddr = "127.0.0.1:18081"
	defaultInputRate  = 16000
	defaultOutputRate = 24000
)

type profile struct {
	ID       string   `json:"profile_id"`
	Revision int64    `json:"profile_revision"`
	Model    string   `json:"model_profile"`
	Provider string   `json:"provider"`
	Voices   []string `json:"voice_profiles"`
	Details  []struct {
		ID   string `json:"voice_profile"`
		Name string `json:"name"`
		Kind string `json:"kind"`
	} `json:"voice_profile_details"`
	Clients   []string `json:"client_transports"`
	Providers []string `json:"provider_transports"`
	Turns     []string `json:"turn_detection_modes"`
}
type voice struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}
type sessionResponse struct {
	SessionID string `json:"session_id"`
	ProfileID string `json:"profile_id"`
	Model     string `json:"model_profile"`
	Provider  string `json:"provider"`
	Transport string `json:"client_transport"`
	Endpoint  struct {
		Type        string `json:"type"`
		URL         string `json:"url"`
		AccessToken string `json:"access_token"`
		ExpiresAtMS int64  `json:"expires_at_ms"`
	} `json:"client_endpoint"`
}

type wssDiagnoseRequest struct {
	SessionID   string `json:"session_id"`
	Endpoint    string `json:"endpoint"`
	AccessToken string `json:"access_token"`
}

func main() {
	s, err := newServer()
	if err != nil {
		log.Fatal(err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /{$}", s.index)
	mux.HandleFunc("GET /api/voices", s.listVoices)
	mux.HandleFunc("POST /api/session", s.createSession)
	mux.HandleFunc("POST /api/session/{session_id}/stop", s.stopSession)
	mux.HandleFunc("POST /api/wss-diagnose", s.diagnoseWSS)
	addr := envOrDefault("LISTEN_ADDR", defaultListenAddr)
	log.Printf("Lingxi realtime browser example listening on %s", addr)
	log.Fatal(http.ListenAndServe(addr, mux))
}
func newServer() (*server, error) {
	in, err := sampleRate("LINGXI_INPUT_SAMPLE_RATE", defaultInputRate)
	if err != nil {
		return nil, err
	}
	out, err := sampleRate("LINGXI_OUTPUT_SAMPLE_RATE", defaultOutputRate)
	if err != nil {
		return nil, err
	}
	var diagnoseOrigin *url.URL
	if value := os.Getenv("LINGXI_WSS_DIAGNOSE_ORIGIN"); value != "" {
		diagnoseOrigin, err = trustedBaseURL(value)
		if err != nil || diagnoseOrigin.Scheme != "https" {
			return nil, errors.New("LINGXI_WSS_DIAGNOSE_ORIGIN must be an exact HTTPS service origin")
		}
	}
	return &server{inputRate: in, outputRate: out, diagnoseOrigin: diagnoseOrigin, client: &http.Client{Timeout: 30 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}, nil
}
func envOrDefault(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}
func sampleRate(key string, fallback int) (int, error) {
	v := os.Getenv(key)
	if v == "" {
		return fallback, nil
	}
	n, e := strconv.Atoi(v)
	if e != nil || n < 8000 || n > 96000 {
		return 0, fmt.Errorf("%s must be 8000..96000", key)
	}
	return n, nil
}
func (s *server) index(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	b, _ := staticFiles.ReadFile("index.html")
	_, _ = w.Write(b)
}
func transports(t string) (string, string, bool) {
	switch t {
	case "wss":
		return "platform_wss", "provider_wss", true
	case "rtc":
		return "platform_rtc", "provider_rtc", true
	}
	return "", "", false
}
func contains(values []string, v string) bool {
	for _, x := range values {
		if x == v {
			return true
		}
	}
	return false
}
func readResponseBody(resp *http.Response) ([]byte, error) {
	body, err := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	_ = resp.Body.Close()
	resp.Body = io.NopCloser(bytes.NewReader(body))
	return body, err
}
func logBackendError(req *http.Request, requestBody []byte, resp *http.Response, responseBody []byte, err error) {
	status := ""
	if resp != nil {
		status = resp.Status
	}
	log.Printf("RTIC backend request failed: method=%s url=%s request_body=%q status=%q response_body=%q error=%v", req.Method, req.URL.String(), string(requestBody), status, string(responseBody), err)
}
func writeBackendError(w http.ResponseWriter, status int, operation, requestURL string, responseBody []byte, err error, profiles ...[]profile) {
	message := err.Error()
	if len(responseBody) > 0 {
		message = strings.TrimSpace(string(responseBody))
	}
	response := map[string]any{
		"error":         "rtic_backend_request_failed",
		"message":       message,
		"operation":     operation,
		"request_url":   requestURL,
		"cause":         err.Error(),
		"response_body": strings.TrimSpace(string(responseBody)),
	}
	if len(profiles) > 0 && profiles[0] != nil {
		response["profiles"] = profiles[0]
	}
	writeJSON(w, status, response)
}
func clientConfigFromRequest(r *http.Request) (clientConfig, error) {
	base, err := trustedBaseURL(r.Header.Get(rticBaseURLHeader))
	if err != nil {
		return clientConfig{}, fmt.Errorf("RTIC base URL: %w", err)
	}
	token := strings.TrimSpace(r.Header.Get(rticTokenHeader))
	appID := strings.TrimSpace(r.Header.Get(rtiAppIDHeader))
	if token == "" || appID == "" {
		return clientConfig{}, errors.New("RTIC API token and RTI app ID are required")
	}
	return clientConfig{RTICBaseURL: base, Token: token, AppID: appID}, nil
}
func (s *server) authorizedProfile(ctx context.Context, cfg clientConfig, t string) (profile, []voice, []profile, error) {
	client, provider, ok := transports(t)
	if !ok {
		return profile{}, nil, nil, errors.New("invalid transport")
	}
	target := cfg.RTICBaseURL.ResolveReference(&url.URL{Path: "/rtic/v1/realtime/profiles", RawQuery: url.Values{"app_id": {cfg.AppID}}.Encode()})
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet, target.String(), nil)
	req.Header.Set("Authorization", "Bearer "+cfg.Token)
	log.Printf("RTIC profiles request: method=%s url=%s", req.Method, req.URL.String())
	resp, err := s.client.Do(req)
	if err != nil {
		logBackendError(req, nil, nil, nil, err)
		return profile{}, nil, nil, err
	}
	defer resp.Body.Close()
	responseBody, readErr := readResponseBody(resp)
	if readErr != nil {
		logBackendError(req, nil, resp, responseBody, readErr)
		return profile{}, nil, nil, readErr
	}
	if resp.StatusCode != 200 {
		err := errors.New("profile list rejected")
		logBackendError(req, nil, resp, responseBody, err)
		return profile{}, nil, nil, fmt.Errorf("RTIC profile request failed: %s: %s", resp.Status, strings.TrimSpace(string(responseBody)))
	}
	var list struct {
		Profiles []profile `json:"profiles"`
	}
	if err = json.Unmarshal(responseBody, &list); err != nil {
		logBackendError(req, nil, resp, responseBody, err)
		return profile{}, nil, nil, err
	}
	if list.Profiles == nil {
		list.Profiles = []profile{}
	}
	profilesJSON, _ := json.Marshal(list.Profiles)
	log.Printf("RTIC profiles response: app_id=%s transport=%s profiles=%s", cfg.AppID, t, profilesJSON)
	var selected profile
	for _, p := range list.Profiles {
		if p.Model == "lingxi-realtime" && p.Provider == "lingxi" && contains(p.Clients, client) && contains(p.Providers, provider) && p.Revision > selected.Revision {
			selected = p
		}
	}
	if selected.ID == "" {
		return profile{}, nil, list.Profiles, errors.New("transport is not granted")
	}
	voices := []voice{}
	seen := map[string]bool{}
	for _, id := range selected.Voices {
		if id == "" || len(id) > 128 || strings.HasPrefix(id, "rtiv_") || seen[id] {
			continue
		}
		seen[id] = true
		name := id
		cloned := false
		for _, d := range selected.Details {
			if d.ID == id {
				if d.Name != "" {
					name = d.Name
				}
				cloned = d.Kind == "cloned"
			}
		}
		if !cloned {
			voices = append(voices, voice{id, name})
		}
	}
	if len(voices) == 0 {
		return profile{}, nil, list.Profiles, errors.New("no authorized system voices")
	}
	return selected, voices, list.Profiles, nil
}
func (s *server) listVoices(w http.ResponseWriter, r *http.Request) {
	cfg, err := clientConfigFromRequest(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	t := r.URL.Query().Get("transport")
	if _, _, ok := transports(t); !ok {
		http.Error(w, "invalid transport", 400)
		return
	}
	p, v, profiles, e := s.authorizedProfile(r.Context(), cfg, t)
	if e != nil {
		requestURL := cfg.RTICBaseURL.ResolveReference(&url.URL{Path: "/rtic/v1/realtime/profiles", RawQuery: url.Values{"app_id": {cfg.AppID}}.Encode()}).String()
		writeBackendError(w, http.StatusBadGateway, "list_profiles", requestURL, nil, e, profiles)
		return
	}
	writeJSON(w, 200, map[string]any{"profile_id": p.ID, "voices": v, "turn_modes": p.Turns, "profiles": profiles})
}
func (s *server) createSession(w http.ResponseWriter, r *http.Request) {
	cfg, err := clientConfigFromRequest(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	var input sessionRequest
	d := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
	d.DisallowUnknownFields()
	if d.Decode(&input) != nil || d.Decode(&struct{}{}) != io.EOF {
		http.Error(w, "invalid request", 400)
		return
	}
	input.Voice = strings.TrimSpace(input.Voice)
	input.CustomHeaderKey = strings.TrimSpace(input.CustomHeaderKey)
	if input.CustomHeaderKey != "" {
		if len(input.CustomHeaderKey) > 128 || input.CustomHeaderValue == "" || len(input.CustomHeaderValue) > 4096 || strings.ContainsAny(input.CustomHeaderValue, "\r\n") || !validCustomHeader(input.CustomHeaderKey) {
			http.Error(w, "invalid custom header", http.StatusBadRequest)
			return
		}
	}
	client, provider, ok := transports(input.Transport)
	if !ok || input.Model != "lingxi-realtime" || input.Voice == "" || len(input.Voice) > 128 {
		http.Error(w, "invalid session request", 400)
		return
	}
	p, voices, profiles, err := s.authorizedProfile(r.Context(), cfg, input.Transport)
	if err != nil {
		requestURL := cfg.RTICBaseURL.ResolveReference(&url.URL{Path: "/rtic/v1/realtime/profiles", RawQuery: url.Values{"app_id": {cfg.AppID}}.Encode()}).String()
		writeBackendError(w, http.StatusBadGateway, "list_profiles", requestURL, nil, err, profiles)
		return
	}
	allowed := false
	for _, v := range voices {
		allowed = allowed || v.ID == input.Voice
	}
	if !allowed {
		http.Error(w, "voice is not authorized", 400)
		return
	}
	payload, _ := json.Marshal(map[string]any{"app_id": cfg.AppID, "user_id": "lingxi-browser", "client_id": "lingxi-browser", "model_profile": input.Model, "role_profile": "default_assistant", "voice_profile": input.Voice, "scenario": "lingxi_realtime_browser_example", "client_transport": client, "provider_transport_preference": provider, "region": "cn-beijing"})
	target := cfg.RTICBaseURL.ResolveReference(&url.URL{Path: "/rtic/v1/realtime/sessions"})
	req, _ := http.NewRequestWithContext(r.Context(), http.MethodPost, target.String(), bytes.NewReader(payload))
	req.Header.Set("Authorization", "Bearer "+cfg.Token)
	req.Header.Set("Content-Type", "application/json")
	if input.CustomHeaderKey != "" {
		req.Header.Set(input.CustomHeaderKey, input.CustomHeaderValue)
	}
	resp, err := s.client.Do(req)
	if err != nil {
		logBackendError(req, payload, nil, nil, err)
		writeBackendError(w, http.StatusBadGateway, "create_session", req.URL.String(), nil, err)
		return
	}
	defer resp.Body.Close()
	responseBody, readErr := readResponseBody(resp)
	if readErr != nil {
		logBackendError(req, payload, resp, responseBody, readErr)
		writeBackendError(w, http.StatusBadGateway, "create_session", req.URL.String(), responseBody, readErr)
		return
	}
	if resp.StatusCode != 200 && resp.StatusCode != 201 {
		logBackendError(req, payload, resp, responseBody, errors.New("RTIC rejected session"))
		writeBackendError(w, resp.StatusCode, "create_session", req.URL.String(), responseBody, errors.New("RTIC rejected session"))
		return
	}
	var created sessionResponse
	decodeErr := json.Unmarshal(responseBody, &created)
	if decodeErr != nil || created.ProfileID != p.ID || created.Model != input.Model || created.Provider != "lingxi" || created.Transport != client || created.Endpoint.Type != client || created.Endpoint.AccessToken == "" {
		logBackendError(req, payload, resp, nil, fmt.Errorf("incompatible RTIC session: decode_ok=%t response_bytes=%d", decodeErr == nil, len(responseBody)))
		http.Error(w, "incompatible RTIC session", 502)
		return
	}
	endpoint, err := resolveEndpoint(created.Endpoint.URL, created.SessionID, input.Transport)
	if err != nil {
		http.Error(w, "invalid client endpoint", 502)
		return
	}
	writeJSON(w, 201, map[string]any{"session_id": created.SessionID, "endpoint_url": endpoint, "transport": input.Transport, "access_token": created.Endpoint.AccessToken, "expires_at_ms": created.Endpoint.ExpiresAtMS, "input_sample_rate": s.inputRate, "output_sample_rate": s.outputRate, "turn_modes": p.Turns})
}

func (s *server) diagnoseWSS(w http.ResponseWriter, r *http.Request) {
	var input wssDiagnoseRequest
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
	if decoder.Decode(&input) != nil || input.SessionID == "" || input.AccessToken == "" {
		http.Error(w, "invalid WSS diagnosis request", http.StatusBadRequest)
		return
	}
	endpoint, err := resolveEndpoint(input.Endpoint, input.SessionID, "wss")
	if err != nil {
		http.Error(w, "invalid WSS endpoint", http.StatusBadRequest)
		return
	}
	target, _ := url.Parse(endpoint)
	if s.diagnoseOrigin == nil || target.Scheme != "wss" || !strings.EqualFold(target.Host, s.diagnoseOrigin.Host) {
		http.Error(w, "WSS diagnosis requires a configured matching HTTPS origin", http.StatusBadRequest)
		return
	}
	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet, "https://"+target.Host+target.EscapedPath(), nil)
	if err != nil {
		http.Error(w, "invalid WSS endpoint", http.StatusBadRequest)
		return
	}
	req.Header.Set("Authorization", "Bearer "+input.AccessToken)
	req.Header.Set("Connection", "Upgrade")
	req.Header.Set("Upgrade", "websocket")
	req.Header.Set("Sec-WebSocket-Version", "13")
	req.Header.Set("Sec-WebSocket-Key", "cndzcy1kaWFnbm9zdGljLWtleQ==")
	client := *s.client
	client.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	resp, err := client.Do(req)
	if err != nil {
		writeJSON(w, http.StatusOK, map[string]any{"ok": false, "phase": "network", "error": err.Error()})
		return
	}
	defer resp.Body.Close()
	body, readErr := io.ReadAll(io.LimitReader(resp.Body, 16<<10))
	if readErr != nil {
		writeJSON(w, http.StatusOK, map[string]any{"ok": false, "phase": "response", "http_status": resp.StatusCode, "error": readErr.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": resp.StatusCode == http.StatusSwitchingProtocols, "phase": "http_upgrade", "http_status": resp.StatusCode, "http_status_text": resp.Status, "body": strings.TrimSpace(string(body))})
}

func (s *server) stopSession(w http.ResponseWriter, r *http.Request) {
	cfg, err := clientConfigFromRequest(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	sessionID := r.PathValue("session_id")
	if sessionID == "" || strings.ContainsAny(sessionID, "/\\?#%") {
		http.Error(w, "invalid session id", http.StatusBadRequest)
		return
	}
	var requestBody []byte
	if r.Body != nil {
		var readErr error
		requestBody, readErr = io.ReadAll(io.LimitReader(r.Body, 4096))
		if readErr != nil {
			logBackendError(r, requestBody, nil, nil, readErr)
			http.Error(w, "invalid session stop request", http.StatusBadRequest)
			return
		}
	}
	target := cfg.RTICBaseURL.ResolveReference(&url.URL{Path: "/rtic/v1/realtime/sessions/" + url.PathEscape(sessionID) + "/stop"})
	req, _ := http.NewRequestWithContext(r.Context(), http.MethodPost, target.String(), bytes.NewReader(requestBody))
	req.Header.Set("Authorization", "Bearer "+cfg.Token)
	req.Header.Set("X-App-ID", cfg.AppID)
	req.Header.Set("Content-Type", "application/json")
	if r.Body == nil {
		req.Body = http.NoBody
	}
	resp, err := s.client.Do(req)
	if err != nil {
		logBackendError(req, requestBody, nil, nil, err)
		http.Error(w, "session stop unavailable", http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()
	responseBody, readErr := readResponseBody(resp)
	if readErr != nil {
		logBackendError(req, requestBody, resp, responseBody, readErr)
		http.Error(w, "session stop unavailable", http.StatusBadGateway)
		return
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		logBackendError(req, requestBody, resp, responseBody, errors.New("RTIC rejected session stop"))
		writeUpstreamError(w, resp, responseBody, "RTIC rejected session stop")
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"stopped": true, "session_id": sessionID})
}

func writeUpstreamError(w http.ResponseWriter, resp *http.Response, body []byte, fallback string) {
	if len(body) > 0 {
		if contentType := resp.Header.Get("Content-Type"); contentType == "application/json" || strings.HasPrefix(contentType, "application/json;") {
			w.Header().Set("Content-Type", contentType)
			w.WriteHeader(resp.StatusCode)
			_, _ = w.Write(body)
			return
		}
	}
	message := strings.TrimSpace(string(body))
	if message == "" {
		message = fallback
	}
	writeJSON(w, resp.StatusCode, map[string]any{
		"error":           "rtic_session_create_failed",
		"message":         message,
		"upstream_status": resp.StatusCode,
		"status":          resp.Status,
	})
}

func validCustomHeader(key string) bool {
	canonical := textproto.CanonicalMIMEHeaderKey(key)
	if canonical == "" {
		return false
	}
	switch canonical {
	case "Authorization", "Content-Length", "Content-Type", "Host", "Transfer-Encoding", "Connection", "Upgrade":
		return false
	default:
		return true
	}
}
func resolveEndpoint(value, id, t string) (string, error) {
	if id == "" || strings.ContainsAny(id, "/\\?#%") {
		return "", errors.New("invalid session id")
	}
	u, e := url.Parse(strings.TrimSpace(value))
	if e != nil || !u.IsAbs() {
		return "", errors.New("RTIC must return an absolute client endpoint")
	}
	suffix := "rtc"
	if t == "wss" {
		suffix = "ws"
		if u.Scheme == "http" {
			u.Scheme = "ws"
		}
		if u.Scheme == "https" {
			u.Scheme = "wss"
		}
	} else if t != "rtc" {
		return "", errors.New("invalid transport")
	}
	if (u.Scheme != "wss" && u.Scheme != "https" && t == "rtc") || (u.Scheme != "ws" && u.Scheme != "wss" && t == "wss") || u.User != nil || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" || u.RawPath != "" || u.Host == "" || u.Path != "/v1/realtime/sessions/"+id+"/"+suffix {
		return "", errors.New("invalid endpoint")
	}
	return u.String(), nil
}
func trustedBaseURL(v string) (*url.URL, error) {
	u, e := url.Parse(strings.TrimSpace(v))
	if e != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" || u.User != nil || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" || u.Path != "" && u.Path != "/" {
		return nil, errors.New("must be an exact HTTP(S) service origin")
	}
	return u, nil
}
func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
