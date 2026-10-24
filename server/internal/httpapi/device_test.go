package httpapi

import (
	"crypto/tls"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/iewiad/miku-intercom/server/internal/media"
)

const deviceHost = "192.168.50.5:8443"

func nativeRequest(handler http.Handler, method, path, body, token string, mutate func(*http.Request)) *httptest.ResponseRecorder {
	request := httptest.NewRequest(method, "https://"+deviceHost+path, strings.NewReader(body))
	request.TLS = &tls.ConnectionState{Version: tls.VersionTLS13}
	request.Header.Set("Content-Type", "application/json")
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	if mutate != nil {
		mutate(request)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}

func TestDeviceTLSHostOriginAndAuth(t *testing.T) {
	f := fixture(t)
	handler, err := NewDevice(f.s, nil, deviceHost)
	if err != nil {
		t.Fatal(err)
	}
	for _, token := range []string{"", "forged"} {
		errorCode(t, nativeRequest(handler, "GET", "/api/v1/state", "", token, nil), 401, "unauthorized")
	}
	response(t, nativeRequest(handler, "GET", "/api/v1/state", "", f.tokenA, nil), 200)
	for _, site := range []string{"none", "same-origin"} {
		response(t, nativeRequest(handler, "GET", "/api/v1/state", "", f.tokenA, func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", site) }), 200)
	}
	for _, test := range []struct {
		name, code string
		mutate     func(*http.Request)
	}{
		{"cleartext", "tls_required", func(r *http.Request) { r.TLS = nil; r.Header.Set("X-Forwarded-Proto", "https") }},
		{"old_tls", "tls_required", func(r *http.Request) { r.TLS.Version = tls.VersionTLS11 }},
		{"other_host", "host_forbidden", func(r *http.Request) { r.Host = "192.168.50.6:8443"; r.Header.Set("X-Forwarded-Host", deviceHost) }},
		{"missing_port", "host_forbidden", func(r *http.Request) { r.Host = "192.168.50.5" }},
		{"loopback_host", "host_forbidden", func(r *http.Request) { r.Host = "localhost:8443" }},
		{"other_port", "host_forbidden", func(r *http.Request) { r.Host = "192.168.50.5:443" }},
		{"origin", "origin_forbidden", func(r *http.Request) { r.Header.Set("Origin", "https://"+deviceHost) }},
		{"empty_origin", "origin_forbidden", func(r *http.Request) { r.Header["Origin"] = []string{""} }},
		{"nil_origin", "origin_forbidden", func(r *http.Request) { r.Header["origin"] = nil }},
		{"duplicate_origin", "origin_forbidden", func(r *http.Request) { r.Header["Origin"] = []string{"", "https://" + deviceHost} }},
		{"cross_site", "origin_forbidden", func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", "cross-site") }},
		{"same_site", "origin_forbidden", func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", "same-site") }},
		{"duplicate_site", "origin_forbidden", func(r *http.Request) { r.Header["Sec-Fetch-Site"] = []string{"none", "none"} }},
		{"case_duplicate_site", "origin_forbidden", func(r *http.Request) {
			r.Header["Sec-Fetch-Site"] = []string{"none"}
			r.Header["sec-fetch-site"] = []string{"none"}
		}},
		{"combined_site", "origin_forbidden", func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", "none, cross-site") }},
	} {
		t.Run(test.name, func(t *testing.T) {
			errorCode(t, nativeRequest(handler, "GET", "/api/v1/state", "", f.tokenA, test.mutate), 403, test.code)
		})
	}
	// Even a valid device request cannot widen the original handler's Host boundary.
	errorCode(t, nativeRequest(f.h, "GET", "/api/v1/state", "", f.tokenA, nil), 403, "host_forbidden")
	for _, value := range []string{"", "0.0.0.0:8443", "127.0.0.1:8443", "device.local:8443", "8.8.8.8:8443"} {
		if _, err := NewDevice(f.s, nil, value); err == nil {
			t.Fatal("unsafe device handler configuration accepted")
		}
	}
}

func TestDeviceOnlyAuthenticatedAPIAndSharedState(t *testing.T) {
	f := fixture(t)
	hub, err := media.New(f.s, media.Options{Diagnostics: true})
	if err != nil {
		t.Fatal(err)
	}
	defer hub.Close()
	handler, err := NewDevice(f.s, hub, deviceHost)
	if err != nil {
		t.Fatal(err)
	}
	for _, token := range []string{"", f.tokenA} {
		errorCode(t, nativeRequest(handler, "POST", "/api/v1/devices", `{"name":"not-provisioned"}`, token, nil), 403, "bootstrap_forbidden")
	}
	for _, path := range []string{"/", "/app.js", "/audio.js", "/style.css", "/diagnostics", "/diagnostics.js", "/lifecycle.js", "/api/v1/devices/", "/api/v1//devices", "//api/v1/devices", "/api/v1/../devices", "/api/v1/media/peers/id/extra"} {
		errorCode(t, nativeRequest(handler, "GET", path, "", f.tokenA, nil), 404, "not_found")
	}
	errorCode(t, nativeRequest(handler, "GET", "/api/v1/state", "", f.tokenA, func(r *http.Request) { r.URL.RawPath = "/api/v1/%73tate" }), 404, "not_found")
	out := response(t, nativeRequest(handler, "POST", "/api/v1/groups", `{"name":"device-test-group"}`, f.tokenA, nil), 201)
	var group struct {
		ID string `json:"id"`
	}
	if err := json.Unmarshal(out["group"], &group); err != nil {
		t.Fatal(err)
	}
	state, err := f.s.State(f.a.ID)
	if err != nil || state.CurrentGroupID != group.ID {
		t.Fatal("device listener did not use shared control state")
	}
	response(t, request(f.h, "GET", "/api/v1/state", "", f.tokenA, nil), 200)
	response(t, nativeRequest(handler, "GET", "/api/v1/media", "", f.tokenA, nil), 200)
	response(t, nativeRequest(handler, "DELETE", "/api/v1/media/peers/unknown_peer", "", f.tokenA, nil), 200)
	r := nativeRequest(handler, "GET", "/api/v1/state", "", f.tokenA, nil)
	if r.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatal("device API enabled CORS")
	}
	// Local bootstrap still works; the native gate does not modify New.
	response(t, request(f.h, "POST", "/api/v1/devices", `{"name":"local-provisioned"}`, "", nil), 201)
}
