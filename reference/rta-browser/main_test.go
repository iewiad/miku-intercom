package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
)

var testRTICBaseURL string

func testServer(t *testing.T, handler http.HandlerFunc) *server {
	t.Helper()
	u := httptest.NewServer(handler)
	testRTICBaseURL = u.URL
	t.Cleanup(u.Close)
	return &server{client: u.Client(), inputRate: 16000, outputRate: 24000}
}
func testRequest(method, target string, body *strings.Reader) *http.Request {
	var requestBody io.Reader
	if body != nil {
		requestBody = body
	}
	r := httptest.NewRequest(method, target, requestBody)
	r.Header.Set(rticBaseURLHeader, testRTICBaseURL)
	r.Header.Set(rticTokenHeader, "backend-only-token")
	r.Header.Set(rtiAppIDHeader, "test-app")
	return r
}
func writeProfiles(w http.ResponseWriter, t string) {
	client, provider, _ := transports(t)
	writeJSON(w, 200, map[string]any{"profiles": []any{map[string]any{"profile_id": "lingxi-realtime-" + t, "profile_revision": 2, "model_profile": "lingxi-realtime", "provider": "lingxi", "voice_profiles": []string{"default", "second", "rtiv_clone"}, "voice_profile_details": []any{map[string]any{"voice_profile": "second", "name": "第二音色", "kind": "system"}}, "client_transports": []string{client}, "provider_transports": []string{provider}, "turn_detection_modes": []string{"manual", "server_vad"}}}})
}
func TestSessionTransportAndCredentialBoundary(t *testing.T) {
	for _, transport := range []string{"wss", "rtc"} {
		t.Run(transport, func(t *testing.T) {
			client, provider, _ := transports(transport)
			suffix, scheme := "ws", "wss"
			if transport == "rtc" {
				suffix, scheme = "rtc", "https"
			}
			s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Header.Get("Authorization") != "Bearer backend-only-token" {
					t.Error("missing server auth")
				}
				switch r.URL.Path {
				case "/rtic/v1/realtime/profiles":
					if r.URL.Query().Get("app_id") != "test-app" {
						t.Error("wrong app")
					}
					writeProfiles(w, transport)
				case "/rtic/v1/realtime/sessions":
					if r.Header.Get("X-Trace-Id") != "browser-test" {
						t.Errorf("custom header = %q", r.Header.Get("X-Trace-Id"))
					}
					var request map[string]any
					if json.NewDecoder(r.Body).Decode(&request) != nil {
						t.Error("bad request")
					}
					if request["client_transport"] != client || request["provider_transport_preference"] != provider || request["voice_profile"] != "second" || request["model_profile"] != "lingxi-realtime" {
						t.Errorf("payload %#v", request)
					}
					fmt.Fprintf(w, `{"session_id":"session-1","profile_id":"lingxi-realtime-%s","model_profile":"lingxi-realtime","provider":"lingxi","client_transport":"%s","client_endpoint":{"type":"%s","url":"https://rtid.example.com/v1/realtime/sessions/session-1/%s","access_token":"short-token","expires_at_ms":42},"provider_credentials":"must-not-leak"}`, transport, client, client, suffix)
				default:
					t.Error("unexpected path")
				}
			})
			rr := httptest.NewRecorder()
			s.createSession(rr, testRequest("POST", "/api/session", strings.NewReader(fmt.Sprintf(`{"model":"lingxi-realtime","voice":"second","transport":"%s","custom_header_key":"x-trace-id","custom_header_value":"browser-test"}`, transport))))
			if rr.Code != 201 || !strings.Contains(rr.Body.String(), scheme+"://rtid.example.com/v1/realtime/sessions/session-1/"+suffix) {
				t.Fatalf("status=%d body=%s", rr.Code, rr.Body)
			}
			if strings.Contains(rr.Body.String(), "backend-only-token") || strings.Contains(rr.Body.String(), "must-not-leak") {
				t.Fatal("credential leaked")
			}
			if rr.Header().Get("Cache-Control") != "no-store" {
				t.Fatal("missing no-store")
			}
		})
	}
}

func TestCustomHeaderValidation(t *testing.T) {
	s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" {
			writeProfiles(w, "wss")
			return
		}
		t.Fatal("invalid custom header reached RTIC")
	})
	for _, body := range []string{
		`{"model":"lingxi-realtime","voice":"default","transport":"wss","custom_header_key":"Authorization","custom_header_value":"override"}`,
		`{"model":"lingxi-realtime","voice":"default","transport":"wss","custom_header_key":"X-Test","custom_header_value":"line\nvalue"}`,
	} {
		rr := httptest.NewRecorder()
		s.createSession(rr, testRequest("POST", "/api/session", strings.NewReader(body)))
		if rr.Code != http.StatusBadRequest {
			t.Errorf("request %s status=%d", body, rr.Code)
		}
	}
}
func TestEndpointsPinnedToOriginSessionAndTransport(t *testing.T) {
	for _, transport := range []string{"wss", "rtc"} {
		suffix, scheme := "ws", "wss"
		if transport == "rtc" {
			suffix, scheme = "rtc", "https"
		}
		path := "/v1/realtime/sessions/session-1/" + suffix
		for _, bad := range []string{scheme + "://user@rtid.example.com" + path, path + "?token=x", path + "?", path + "#fragment", strings.Replace(path, "session-1", "session-2", 1), "//evil.example" + path, path + "/extra", strings.Replace(path, "session-1", "%73ession-1", 1)} {
			if u, e := resolveEndpoint(bad, "session-1", transport); e == nil {
				t.Errorf("accepted %q as %s", bad, u)
			}
		}
		valid := scheme + "://rtid.example.com" + path
		if transport == "wss" {
			valid = "https://rtid.example.com" + path
		}
		if u, e := resolveEndpoint(valid, "session-1", transport); e != nil || u != scheme+"://rtid.example.com"+path {
			t.Errorf("valid endpoint %s %v", u, e)
		}
	}
	for _, id := range []string{"", "../foo", "foo/bar", "foo%2fbar"} {
		if _, e := resolveEndpoint("/v1/realtime/sessions/"+id+"/rtc", id, "rtc"); e == nil {
			t.Errorf("accepted id %q", id)
		}
	}
}
func TestVoiceListAndRequestsFailClosed(t *testing.T) {
	posts := 0
	s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			posts++
			t.Error("unexpected session creation")
		}
		writeProfiles(w, "wss")
	})
	rr := httptest.NewRecorder()
	s.listVoices(rr, testRequest("GET", "/api/voices?transport=wss", nil))
	if rr.Code != 200 || !strings.Contains(rr.Body.String(), "第二音色") {
		t.Fatalf("voices %d %s", rr.Code, rr.Body)
	}
	var result struct {
		Voices   []voice   `json:"voices"`
		Profiles []profile `json:"profiles"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &result); err != nil || len(result.Profiles) != 1 || !contains(result.Profiles[0].Providers, "provider_wss") {
		t.Fatalf("missing profile diagnostics: %s", rr.Body)
	}
	for _, voice := range result.Voices {
		if voice.ID == "rtiv_clone" {
			t.Fatal("cloned voice is selectable")
		}
	}
	for _, body := range []string{`{}`, `{"model":"other","voice":"default","transport":"wss"}`, `{"model":"lingxi-realtime","voice":"missing","transport":"wss"}`, `{"model":"lingxi-realtime","voice":"rtiv_clone","transport":"wss"}`, `{"model":"lingxi-realtime","voice":"default","transport":"tcp"}`, `{"model":"lingxi-realtime","voice":"default","transport":"wss","extra":1}`, `{"model":"lingxi-realtime","voice":"default","transport":"wss"} {}`, strings.Repeat("x", 5000)} {
		rr = httptest.NewRecorder()
		s.createSession(rr, testRequest("POST", "/api/session", strings.NewReader(body)))
		if rr.Code != 400 {
			t.Errorf("request %s status=%d", body[:min(len(body), 100)], rr.Code)
		}
	}
	if posts != 0 {
		t.Fatal("created rejected session")
	}
	rr = httptest.NewRecorder()
	s.listVoices(rr, testRequest("GET", "/api/voices?transport=rtc", nil))
	if rr.Code != 502 || !strings.Contains(rr.Body.String(), "lingxi-realtime-wss") || !strings.Contains(rr.Body.String(), "transport is not granted") {
		t.Fatal("accepted ungranted transport")
	}
}
func TestUpstreamFailuresAndIncompatibleSessions(t *testing.T) {
	for _, reply := range []string{`not json`, `{}`, `{"session_id":"session-1","profile_id":"lingxi-realtime-wss","model_profile":"lingxi-realtime","provider":"lingxi","client_transport":"platform_rtc","client_endpoint":{"type":"platform_rtc","url":"/v1/realtime/sessions/session-1/rtc","access_token":"token"}}`} {
		s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
			if r.Method == "GET" {
				writeProfiles(w, "wss")
			} else {
				fmt.Fprint(w, reply)
			}
		})
		rr := httptest.NewRecorder()
		s.createSession(rr, testRequest("POST", "/api/session", strings.NewReader(`{"model":"lingxi-realtime","voice":"default","transport":"wss"}`)))
		if rr.Code != 502 {
			t.Errorf("accepted incompatible response %s", reply)
		}
	}
	for _, code := range []int{401, 403, 500} {
		s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(code)
			fmt.Fprint(w, "secret upstream error")
		})
		rr := httptest.NewRecorder()
		s.listVoices(rr, testRequest("GET", "/api/voices?transport=wss", nil))
		if rr.Code != 502 || !strings.Contains(rr.Body.String(), "secret upstream error") || !strings.Contains(rr.Body.String(), "request_url") {
			t.Fatalf("upstream diagnostics missing: %s", rr.Body.String())
		}
	}
}

func TestIncompatibleSessionLogsDoNotExposeCredentials(t *testing.T) {
	for _, test := range []struct {
		name  string
		reply string
	}{
		{"stale_profile", `{"profile_id":"stale-profile","client_endpoint":{"access_token":"short-lived-secret"},"provider_credentials":"provider-secret"}`},
		{"wrong_transport", `{"profile_id":"lingxi-realtime-wss","model_profile":"lingxi-realtime","provider":"lingxi","client_transport":"platform_rtc","client_endpoint":{"access_token":"short-lived-secret"},"provider_credentials":"provider-secret"}`},
		{"invalid_json", `{"client_endpoint":{"access_token":"short-lived-secret"},"provider_credentials":"provider-secret"`},
		{"invalid_type", `{"client_endpoint":{"access_token":"short-lived-secret","expires_at_ms":"provider-secret"}}`},
	} {
		t.Run(test.name, func(t *testing.T) {
			var logs bytes.Buffer
			previousOutput := log.Writer()
			log.SetOutput(&logs)
			t.Cleanup(func() { log.SetOutput(previousOutput) })
			s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method == http.MethodGet {
					writeProfiles(w, "wss")
					return
				}
				fmt.Fprint(w, test.reply)
			})
			rr := httptest.NewRecorder()
			s.createSession(rr, testRequest("POST", "/api/session", strings.NewReader(`{"model":"lingxi-realtime","voice":"default","transport":"wss"}`)))
			if rr.Code != http.StatusBadGateway || !strings.Contains(rr.Body.String(), "incompatible RTIC session") {
				t.Fatalf("status=%d body=%s", rr.Code, rr.Body)
			}
			for _, secret := range []string{"short-lived-secret", "provider-secret", "backend-only-token"} {
				if strings.Contains(logs.String(), secret) || strings.Contains(rr.Body.String(), secret) {
					t.Fatal("credential leaked in logs or error response")
				}
			}
			if !strings.Contains(logs.String(), "incompatible RTIC session") || !strings.Contains(logs.String(), "response_bytes=") {
				t.Fatal("missing safe session failure diagnostics")
			}
		})
	}
}

func TestSessionUpstreamErrorIsReturned(t *testing.T) {
	const upstreamError = `{"error_code":"realtime_session_already_running","message":"a realtime session is already running","request_id":"req_test"}`
	s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" {
			writeProfiles(w, "wss")
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusConflict)
		fmt.Fprint(w, upstreamError)
	})
	rr := httptest.NewRecorder()
	s.createSession(rr, testRequest("POST", "/api/session", strings.NewReader(`{"model":"lingxi-realtime","voice":"default","transport":"wss"}`)))
	if rr.Code != http.StatusConflict {
		t.Fatalf("status=%d body=%s", rr.Code, rr.Body)
	}
	if rr.Header().Get("Content-Type") != "application/json" || !strings.Contains(rr.Body.String(), "realtime_session_already_running") || !strings.Contains(rr.Body.String(), "request_url") {
		t.Fatalf("content-type=%q body=%s", rr.Header().Get("Content-Type"), rr.Body)
	}
}

func TestEmptySessionUpstreamErrorHasResponseBody(t *testing.T) {
	s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" {
			writeProfiles(w, "wss")
			return
		}
		w.WriteHeader(http.StatusConflict)
	})
	rr := httptest.NewRecorder()
	s.createSession(rr, testRequest("POST", "/api/session", strings.NewReader(`{"model":"lingxi-realtime","voice":"default","transport":"wss"}`)))
	if rr.Code != http.StatusConflict || rr.Body.Len() == 0 {
		t.Fatalf("status=%d body=%q", rr.Code, rr.Body.String())
	}
	var response map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &response); err != nil {
		t.Fatalf("invalid error response: %v", err)
	}
	if response["message"] != "RTIC rejected session" || response["operation"] != "create_session" || response["request_url"] == "" {
		t.Fatalf("response=%#v", response)
	}
}

func TestDiagnoseWSSReturnsUpgradeFailure(t *testing.T) {
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer short-token" {
			t.Errorf("authorization = %q", r.Header.Get("Authorization"))
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		fmt.Fprint(w, `{"code":"access_token_invalid","message":"invalid access token"}`)
	}))
	defer upstream.Close()
	origin, err := url.Parse(upstream.URL)
	if err != nil {
		t.Fatal(err)
	}
	s := &server{client: upstream.Client(), diagnoseOrigin: origin}
	endpoint := strings.Replace(upstream.URL, "https://", "wss://", 1) + "/v1/realtime/sessions/session-1/ws"
	body := strings.NewReader(fmt.Sprintf(`{"session_id":"session-1","endpoint":%q,"access_token":"short-token"}`, endpoint))
	rr := httptest.NewRecorder()
	s.diagnoseWSS(rr, httptest.NewRequest(http.MethodPost, "/api/wss-diagnose", body))
	if rr.Code != http.StatusOK || !strings.Contains(rr.Body.String(), `"http_status":401`) || !strings.Contains(rr.Body.String(), "access_token_invalid") {
		t.Fatalf("status=%d body=%s", rr.Code, rr.Body)
	}
}

func TestDiagnoseWSSRejectsInvalidEndpoint(t *testing.T) {
	s := &server{client: http.DefaultClient}
	for _, endpoint := range []string{"wss://127.0.0.1/v1/realtime/sessions/other/ws", "wss://127.0.0.1/v1/realtime/sessions/session-1/ws?x=1"} {
		body := strings.NewReader(fmt.Sprintf(`{"session_id":"session-1","endpoint":%q,"access_token":"short-token"}`, endpoint))
		rr := httptest.NewRecorder()
		s.diagnoseWSS(rr, httptest.NewRequest(http.MethodPost, "/api/wss-diagnose", body))
		if rr.Code != http.StatusBadRequest {
			t.Fatalf("endpoint=%q status=%d body=%s", endpoint, rr.Code, rr.Body)
		}
	}
}

func TestDiagnoseWSSPinsConfiguredOrigin(t *testing.T) {
	requests := 0
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests++
		w.Header().Set("Location", "/redirected")
		w.WriteHeader(http.StatusTemporaryRedirect)
	}))
	defer upstream.Close()
	origin, err := url.Parse(upstream.URL)
	if err != nil {
		t.Fatal(err)
	}
	s := &server{client: upstream.Client(), diagnoseOrigin: origin}
	path := "/v1/realtime/sessions/session-1/ws"
	for _, endpoint := range []string{
		"wss://evil.example" + path,
		"wss://" + origin.Hostname() + path,
		"wss://" + origin.Host + ".evil.example" + path,
		"ws://" + origin.Host + path,
	} {
		body := strings.NewReader(fmt.Sprintf(`{"session_id":"session-1","endpoint":%q,"access_token":"short-token"}`, endpoint))
		rr := httptest.NewRecorder()
		s.diagnoseWSS(rr, httptest.NewRequest(http.MethodPost, "/api/wss-diagnose", body))
		if rr.Code != http.StatusBadRequest || requests != 0 {
			t.Fatalf("endpoint=%q status=%d requests=%d", endpoint, rr.Code, requests)
		}
	}
	body := fmt.Sprintf(`{"session_id":"session-1","endpoint":%q,"access_token":"short-token"}`, "wss://"+origin.Host+path)
	rr := httptest.NewRecorder()
	s.diagnoseWSS(rr, httptest.NewRequest(http.MethodPost, "/api/wss-diagnose", strings.NewReader(body)))
	if rr.Code != http.StatusOK || requests != 1 || !strings.Contains(rr.Body.String(), `"http_status":307`) {
		t.Fatalf("status=%d requests=%d body=%s", rr.Code, requests, rr.Body)
	}
	s.diagnoseOrigin = nil
	rr = httptest.NewRecorder()
	s.diagnoseWSS(rr, httptest.NewRequest(http.MethodPost, "/api/wss-diagnose", strings.NewReader(body)))
	if rr.Code != http.StatusBadRequest || requests != 1 {
		t.Fatalf("unconfigured origin: status=%d requests=%d", rr.Code, requests)
	}
}

func TestDiagnoseOriginConfiguration(t *testing.T) {
	for _, value := range []string{"http://rtid.example.com", "https://user@rtid.example.com", "https://rtid.example.com/path", "https://rtid.example.com?x=1"} {
		t.Setenv("LINGXI_WSS_DIAGNOSE_ORIGIN", value)
		if _, err := newServer(); err == nil {
			t.Errorf("accepted diagnosis origin %q", value)
		}
	}
	t.Setenv("LINGXI_WSS_DIAGNOSE_ORIGIN", "https://rtid.example.com:8443")
	s, err := newServer()
	if err != nil || s.diagnoseOrigin == nil || s.diagnoseOrigin.Host != "rtid.example.com:8443" {
		t.Fatalf("configured server=%+v err=%v", s, err)
	}
}

func TestStopSessionForwardsRequest(t *testing.T) {
	var stopped bool
	s := testServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/rtic/v1/realtime/sessions/session-1/stop" || r.Method != http.MethodPost {
			t.Fatalf("unexpected stop request: %s %s", r.Method, r.URL.Path)
		}
		if r.Header.Get("Authorization") != "Bearer backend-only-token" {
			t.Fatal("missing RTIC authorization")
		}
		if got := r.Header.Get("X-App-ID"); got != "test-app" {
			t.Fatalf("App ID = %q, want test-app", got)
		}
		var request struct {
			Reason string `json:"reason"`
		}
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil || request.Reason != "browser_disconnect" {
			t.Fatalf("stop request=%#v err=%v", request, err)
		}
		stopped = true
		writeJSON(w, http.StatusAccepted, map[string]any{"ok": true})
	})
	rr := httptest.NewRecorder()
	stopRequest := testRequest("POST", "/api/session/session-1/stop", strings.NewReader(`{"reason":"browser_disconnect"}`))
	stopRequest.SetPathValue("session_id", "session-1")
	s.stopSession(rr, stopRequest)
	if rr.Code != http.StatusAccepted || !stopped {
		t.Fatalf("status=%d stopped=%v body=%s", rr.Code, stopped, rr.Body.String())
	}
}

func TestTrustedBaseAndEnvironment(t *testing.T) {
	for _, v := range []string{"", "ftp://example.com", "https://user:pass@example.com", "https://example.com?x=1", "https://example.com#x", "https://example.com/path"} {
		if _, e := trustedBaseURL(v); e == nil {
			t.Errorf("accepted %q", v)
		}
	}
	t.Setenv("LISTEN_ADDR", "")
	t.Setenv("LINGXI_INPUT_SAMPLE_RATE", "")
	t.Setenv("LINGXI_OUTPUT_SAMPLE_RATE", "")
	if got := envOrDefault("LISTEN_ADDR", defaultListenAddr); got != defaultListenAddr {
		t.Fatalf("default listen address = %q, want %q", got, defaultListenAddr)
	}
	s, e := newServer()
	if e != nil {
		t.Fatal(e)
	}
	if s.inputRate != defaultInputRate || s.outputRate != defaultOutputRate {
		t.Fatalf("default sample rates = %d/%d, want %d/%d", s.inputRate, s.outputRate, defaultInputRate, defaultOutputRate)
	}
	if e = s.client.CheckRedirect(nil, nil); e != http.ErrUseLastResponse {
		t.Fatal("redirects permitted")
	}
	t.Setenv("LINGXI_INPUT_SAMPLE_RATE", "invalid")
	if _, e = newServer(); e == nil {
		t.Fatal("bad sample rate accepted")
	}
}
