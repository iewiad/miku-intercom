package httpapi

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

type apiFixture struct {
	h              http.Handler
	s              *control.Service
	a, b           control.Device
	tokenA, tokenB string
}

func fixture(t *testing.T) *apiFixture {
	t.Helper()
	s, err := control.New(nil, nil, control.Options{})
	if err != nil {
		t.Fatal(err)
	}
	a, ta, err := s.CreateDevice("甲")
	if err != nil {
		t.Fatal(err)
	}
	b, tb, err := s.CreateDevice("乙")
	if err != nil {
		t.Fatal(err)
	}
	return &apiFixture{New(s), s, a, b, ta, tb}
}
func request(h http.Handler, method, path, body, token string, headers map[string]string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, "http://127.0.0.1:8080"+path, strings.NewReader(body))
	r.Header.Set("Content-Type", "application/json")
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	for key, value := range headers {
		if key == "Host" {
			r.Host = value
		} else {
			r.Header.Set(key, value)
		}
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}
func response(t *testing.T, w *httptest.ResponseRecorder, want int) map[string]json.RawMessage {
	t.Helper()
	if w.Code != want {
		t.Fatalf("status=%d want=%d body=%s", w.Code, want, w.Body.String())
	}
	if w.Header().Get("Cache-Control") != "no-store" {
		t.Fatal("response may be cached")
	}
	var out map[string]json.RawMessage
	if err := json.Unmarshal(w.Body.Bytes(), &out); err != nil {
		t.Fatal(err)
	}
	return out
}
func errorCode(t *testing.T, w *httptest.ResponseRecorder, status int, code string) {
	t.Helper()
	out := response(t, w, status)
	var e struct {
		Code string `json:"code"`
	}
	if err := json.Unmarshal(out["error"], &e); err != nil || e.Code != code {
		t.Fatalf("error code=%s want=%s err=%v", e.Code, code, err)
	}
}
func TestHTTPIdentityAndAuth(t *testing.T) {
	f := fixture(t)
	out := response(t, request(f.h, "POST", "/api/v1/devices", `{"name":"新终端"}`, "", nil), 201)
	var token string
	if err := json.Unmarshal(out["token"], &token); err != nil || len(token) != 43 {
		t.Fatal("missing random token")
	}
	response(t, request(f.h, "GET", "/api/v1/state", "", token, nil), 200)
	errorCode(t, request(f.h, "GET", "/api/v1/state", "", "", nil), 401, "unauthorized")
	errorCode(t, request(f.h, "GET", "/api/v1/state", "", "forged", nil), 401, "unauthorized")
	errorCode(t, request(f.h, "POST", "/api/v1/groups", `{"name":"x","device_id":"forged"}`, f.tokenA, nil), 400, "invalid_json")
}
func TestHTTPStrictBodiesAndMethods(t *testing.T) {
	f := fixture(t)
	cases := []struct {
		method, path, body string
		headers            map[string]string
		status             int
		code               string
	}{
		{"GET", "/api/v1/devices", "", nil, 405, "method_not_allowed"},
		{"POST", "/api/v1/devices", `{"name":"x"}`, map[string]string{"Content-Type": "text/plain"}, 415, "unsupported_media_type"},
		{"POST", "/api/v1/devices", `{"name":"x","extra":true}`, nil, 400, "invalid_json"},
		{"POST", "/api/v1/devices", `{"name":"x"} {}`, nil, 400, "invalid_json"},
		{"POST", "/api/v1/devices", "", nil, 400, "invalid_json"},
		{"POST", "/api/v1/devices", `{"name":"` + strings.Repeat("x", 8200) + `"}`, nil, 413, "body_too_large"},
		{"POST", "/api/v1/devices", `{"name":"x"}` + strings.Repeat(" ", 8200), nil, 413, "body_too_large"},
		{"PUT", "/api/v1/current-group", `{"operation_id":"switch001","group_id":"x"}`, nil, 400, "invalid_request"},
		{"POST", "/api/v1/floor/acquire", `{"group_id":"x","request_id":"press001","server_epoch":"x","valid_until":"bad"}`, nil, 400, "invalid_request"},
		{"POST", "/api/v1/floor/acquire", `{"group_id":"x","current_version":1,"request_id":"press001","server_epoch":"x","valid_until":"bad"}`, nil, 400, "invalid_deadline"},
		{"POST", "/api/v1/floor/release", `{"group_id":"x","current_version":1,"request_id":"press001","server_epoch":"x","lease_id":"extra"}`, nil, 400, "invalid_json"},
		{"GET", "/api/v1/unknown", "", nil, 404, "not_found"},
	}
	for i, tt := range cases {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			errorCode(t, request(f.h, tt.method, tt.path, tt.body, f.tokenA, tt.headers), tt.status, tt.code)
		})
	}
}
func TestHTTPSameOriginAndLoopbackHost(t *testing.T) {
	f := fixture(t)
	for _, headers := range []map[string]string{
		{"Origin": "http://evil.example"},
		{"Origin": "null"},
		{"Origin": "http://127.0.0.1:8080/path"},
		{"Sec-Fetch-Site": "cross-site"},
		{"Sec-Fetch-Site": "same-site"},
	} {
		errorCode(t, request(f.h, "POST", "/api/v1/devices", `{"name":"x"}`, "", headers), 403, "origin_forbidden")
	}
	for _, host := range []string{"evil.example:8080", "127.0.0.1.evil.example:8080", "192.168.1.1:8080", "[::2]:8080", "localhost@evil.example:8080"} {
		headers := map[string]string{"Host": host, "Origin": "http://" + host}
		errorCode(t, request(f.h, "POST", "/api/v1/devices", `{"name":"x"}`, "", headers), 403, "host_forbidden")
		errorCode(t, request(f.h, "GET", "/", "", "", headers), 403, "host_forbidden")
	}
	for _, host := range []string{"localhost", "localhost:8080", "127.0.0.1:8080", "127.0.0.2:8080", "[::1]:8080", "[::ffff:127.0.0.1]:8080"} {
		response(t, request(f.h, "GET", "/api/v1/state", "", f.tokenA, map[string]string{"Host": host, "Origin": "http://" + host, "Sec-Fetch-Site": "same-origin"}), 200)
	}
	response(t, request(f.h, "GET", "/api/v1/state", "", f.tokenA, nil), 200)
}
func TestStaticWhitelistAndCrossSiteNavigation(t *testing.T) {
	f := fixture(t)
	for path, typ := range map[string]string{"/": "text/html", "/app.js": "text/javascript", "/style.css": "text/css"} {
		w := request(f.h, "GET", path, "", "", map[string]string{"Sec-Fetch-Site": "cross-site", "Origin": "http://elsewhere.example"})
		if w.Code != 200 || !strings.HasPrefix(w.Header().Get("Content-Type"), typ) || w.Header().Get("Cache-Control") != "no-store" {
			t.Fatalf("static %s: %d", path, w.Code)
		}
	}
	for _, path := range []string{"/app.test.js", "/package.json", "/web/app.js", "/state.json", "/work/intercom/state.json", "/api/v1/devices/extra", "/api/v1/operations/id/extra"} {
		errorCode(t, request(f.h, "GET", path, "", "", nil), 404, "not_found")
	}
	errorCode(t, request(f.h, "POST", "/", "", "", nil), 405, "method_not_allowed")
}
func TestHTTPGroupsSwitchAndFloor(t *testing.T) {
	f := fixture(t)
	out := response(t, request(f.h, "POST", "/api/v1/groups", `{"name":"合作"}`, f.tokenA, nil), 201)
	var g control.Group
	var code string
	var st control.State
	if err := json.Unmarshal(out["group"], &g); err != nil {
		t.Fatal(err)
	}
	_ = json.Unmarshal(out["code"], &code)
	_ = json.Unmarshal(out["state"], &st)
	if st.MediaReady || st.CurrentVersion != 1 || st.CurrentGroupID != g.ID || len(code) != 8 {
		t.Fatal("invalid group state")
	}
	hidden := response(t, request(f.h, "GET", "/api/v1/state", "", f.tokenB, nil), 200)
	var bs control.State
	_ = json.Unmarshal(hidden["groups"], &bs.Groups)
	if len(bs.Groups) != 0 {
		t.Fatal("nonmember sees group")
	}
	floorPayload := func(st control.State, requestID string) string {
		b, _ := json.Marshal(control.AcquireRequest{FloorRequest: control.FloorRequest{GroupID: g.ID, CurrentVersion: st.CurrentVersion, RequestID: requestID, ServerEpoch: st.ServerEpoch}, ValidUntil: time.Now().UTC().Add(5 * time.Second)})
		return string(b)
	}
	errorCode(t, request(f.h, "POST", "/api/v1/floor/acquire", floorPayload(st, "press0001"), f.tokenB, nil), 403, "not_member")
	join, _ := json.Marshal(map[string]string{"code": " " + strings.ToLower(code) + " "})
	response(t, request(f.h, "POST", "/api/v1/memberships", string(join), f.tokenB, nil), 200)
	response(t, request(f.h, "POST", "/api/v1/floor/acquire", floorPayload(st, "press0001"), f.tokenA, nil), 200)
	errorCode(t, request(f.h, "POST", "/api/v1/floor/acquire", floorPayload(st, "press0002"), f.tokenB, nil), 409, "floor_busy")
	switchBody, _ := json.Marshal(control.SwitchRequest{OperationID: "switch001", GroupID: "not_a_member", ExpectedVersion: 1})
	out = response(t, request(f.h, "PUT", "/api/v1/current-group", string(switchBody), f.tokenA, nil), 200)
	var op control.Operation
	_ = json.Unmarshal(out["operation"], &op)
	if op.Status != "rejected" || op.ErrorCode != "not_member" || op.CurrentGroupID != g.ID {
		t.Fatal("rejection terminal not returned")
	}
	response(t, request(f.h, "GET", "/api/v1/operations/switch001", "", f.tokenA, nil), 200)
	errorCode(t, request(f.h, "GET", "/api/v1/operations/switch001", "", f.tokenB, nil), 404, "operation_not_found")
	conflict, _ := json.Marshal(control.SwitchRequest{OperationID: "switch001", GroupID: g.ID, ExpectedVersion: 1})
	errorCode(t, request(f.h, "PUT", "/api/v1/current-group", string(conflict), f.tokenA, nil), 409, "operation_conflict")
}

func TestHTTPRenewReleaseAndCancelBeforeAcquire(t *testing.T) {
	f := fixture(t)
	g, _, st, err := f.s.CreateGroup(f.a.ID, "现场")
	if err != nil {
		t.Fatal(err)
	}
	base := control.FloorRequest{GroupID: g.ID, CurrentVersion: st.CurrentVersion, RequestID: "press001", ServerEpoch: st.ServerEpoch}
	acquire := control.AcquireRequest{FloorRequest: base, ValidUntil: time.Now().UTC().Add(5 * time.Second)}
	body, _ := json.Marshal(acquire)
	out := response(t, request(f.h, "POST", "/api/v1/floor/acquire", string(body), f.tokenA, nil), 200)
	var held control.Floor
	if err = json.Unmarshal(out["floor"], &held); err != nil {
		t.Fatal(err)
	}
	renew, _ := json.Marshal(control.RenewRequest{FloorRequest: base, LeaseID: held.LeaseID})
	response(t, request(f.h, "POST", "/api/v1/floor/renew", string(renew), f.tokenA, nil), 200)
	release, _ := json.Marshal(base)
	response(t, request(f.h, "POST", "/api/v1/floor/release", string(release), f.tokenA, nil), 200)
	errorCode(t, request(f.h, "POST", "/api/v1/floor/renew", string(renew), f.tokenA, nil), 409, "lease_not_held")
	base.RequestID = "canceled1"
	release, _ = json.Marshal(base)
	response(t, request(f.h, "POST", "/api/v1/floor/release", string(release), f.tokenA, nil), 200)
	acquire.FloorRequest = base
	body, _ = json.Marshal(acquire)
	errorCode(t, request(f.h, "POST", "/api/v1/floor/acquire", string(body), f.tokenA, nil), 409, "request_ended")
}
