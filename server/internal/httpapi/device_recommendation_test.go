package httpapi

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

// Native entry tests use the real control service and only replace the remote
// text model. They prove authorization and transaction handling, not semantics,
// acoustic recognition, firmware networking or media recovery.
type deviceNeedModel struct {
	calls int
	input control.RecommendationInput
}

func (m *deviceNeedModel) Recommend(_ context.Context, input control.RecommendationInput) (string, error) {
	m.calls++
	m.input = input
	for _, candidate := range input.Candidates {
		if candidate.Purpose == "设备排障" {
			return fmt.Sprintf(`{"suggestions":[{"candidate_id":%q,"reason":"职责包含设备排障"}]}`, candidate.CandidateID), nil
		}
	}
	return `{"suggestions":[]}`, nil
}

type deviceNeedSnapshot struct{ saved []byte }

func (s *deviceNeedSnapshot) Save(value []byte) error {
	s.saved = append(s.saved[:0], value...)
	return nil
}

func TestNativeRecommendationTransportAndAuthGate(t *testing.T) {
	f := fixture(t)
	h, err := NewDevice(f.s, nil, deviceHost)
	if err != nil {
		t.Fatal(err)
	}
	paths := []struct{ method, path, body string }{
		{"GET", "/api/v1/recommendations", ""},
		{"POST", "/api/v1/recommendations", `{}`},
		{"PUT", "/api/v1/recommendations/confirm", `{}`},
	}
	for _, endpoint := range paths {
		t.Run(endpoint.method+endpoint.path, func(t *testing.T) {
			for _, token := range []string{"", "forged"} {
				errorCode(t, nativeRequest(h, endpoint.method, endpoint.path, endpoint.body, token, nil), 401, "unauthorized")
			}
			for _, gate := range []struct {
				code   string
				change func(*http.Request)
			}{
				{"tls_required", func(r *http.Request) { r.TLS = nil }},
				{"tls_required", func(r *http.Request) { r.TLS.Version = tls.VersionTLS11 }},
				{"host_forbidden", func(r *http.Request) { r.Host = "127.0.0.1:18792" }},
				{"origin_forbidden", func(r *http.Request) { r.Header.Set("Origin", "https://"+deviceHost) }},
				{"origin_forbidden", func(r *http.Request) { r.Header["Origin"] = []string{""} }},
				{"origin_forbidden", func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", "cross-site") }},
				{"origin_forbidden", func(r *http.Request) { r.Header["Sec-Fetch-Site"] = []string{"none", "none"} }},
			} {
				errorCode(t, nativeRequest(h, endpoint.method, endpoint.path, endpoint.body, f.tokenA, gate.change), 403, gate.code)
			}
		})
	}
	out := response(t, nativeRequest(h, "GET", "/api/v1/recommendations", "", f.tokenA, nil), 200)
	if string(out["available"]) != "false" {
		t.Fatal("unconfigured device recommendation advertised")
	}
	st, _ := f.s.State(f.a.ID)
	body, _ := json.Marshal(control.RecommendationRequest{RequestID: "native_request_001", Need: "设备没有声音", ExpectedVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch})
	errorCode(t, nativeRequest(h, "POST", "/api/v1/recommendations", string(body), f.tokenA, nil), 503, "recommendation_unavailable")
	errorCode(t, nativeRequest(h, "PATCH", "/api/v1/recommendations", `{}`, f.tokenA, nil), 405, "method_not_allowed")
	errorCode(t, nativeRequest(h, "POST", "/api/v1/recommendations/confirm", `{}`, f.tokenA, nil), 405, "method_not_allowed")
	for _, path := range []string{"/recommendation.js", "/diagnostics", "/api/v1/recommendations/extra"} {
		errorCode(t, nativeRequest(h, "GET", path, "", f.tokenA, nil), 404, "not_found")
	}
}

func TestNativeRecommendationConfirmSurvivesLostReplyAndRestart(t *testing.T) {
	model, disk := &deviceNeedModel{}, &deviceNeedSnapshot{}
	s, err := control.New(nil, disk, control.Options{Recommender: model})
	if err != nil {
		t.Fatal(err)
	}
	d, token, err := s.CreateDevice("板端离线用例")
	if err != nil {
		t.Fatal(err)
	}
	original, _, _, err := s.CreateGroupWithPurpose(d.ID, "全员", "综合协作")
	if err != nil {
		t.Fatal(err)
	}
	target, _, _, err := s.CreateGroupWithPurpose(d.ID, "维修组", "设备排障")
	if err != nil {
		t.Fatal(err)
	}
	other, otherToken, err := s.CreateDevice("其他身份")
	if err != nil {
		t.Fatal(err)
	}
	_, _, _, err = s.CreateGroupWithPurpose(other.ID, "不可见群", "private purpose")
	if err != nil {
		t.Fatal(err)
	}
	h, err := NewDevice(s, nil, deviceHost)
	if err != nil {
		t.Fatal(err)
	}
	before, _ := s.State(d.ID)
	body, _ := json.Marshal(control.RecommendationRequest{RequestID: "native_request_002", Need: "音响没有声音", ExpectedVersion: before.CurrentVersion, ServerEpoch: before.ServerEpoch})
	// Untrusted clients may not supply candidates or an identity in the body.
	errorCode(t, nativeRequest(h, "POST", "/api/v1/recommendations", strings.TrimSuffix(string(body), "}")+`,"candidates":[]}`, token, nil), 400, "invalid_json")
	reply := nativeRequest(h, "POST", "/api/v1/recommendations", string(body), token, nil)
	out := response(t, reply, 200)
	if reply.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatal("native recommendation enabled CORS")
	}
	var rec control.Recommendation
	if err = json.Unmarshal(out["recommendation"], &rec); err != nil {
		t.Fatal(err)
	}
	after, _ := s.State(d.ID)
	if after.CurrentGroupID != original.ID || after.CurrentVersion != before.CurrentVersion || model.calls != 1 || len(model.input.Candidates) != 2 {
		t.Fatal("recommendation changed current group or used unauthorized candidates")
	}
	if len(rec.Suggestions) != 1 || rec.Suggestions[0].GroupID != target.ID {
		t.Fatal("candidate was not mapped to the authoritative group")
	}
	for _, candidate := range model.input.Candidates {
		if !strings.HasPrefix(candidate.CandidateID, "c") || candidate.Name == "不可见群" || candidate.CandidateID == target.ID {
			t.Fatal("model received an unauthorized group or persistent group identity")
		}
	}
	confirm := control.RecommendationConfirmation{RecommendationID: rec.RecommendationID, OperationID: "native_confirm_002", GroupID: target.ID, ExpectedVersion: rec.CurrentVersion, ServerEpoch: rec.ServerEpoch}
	body, _ = json.Marshal(confirm)
	// Ignore the first successful reply as if transport lost it after commit.
	response(t, nativeRequest(h, "PUT", "/api/v1/recommendations/confirm", string(body), token, nil), 200)
	applied, _ := s.State(d.ID)
	if applied.CurrentGroupID != target.ID || applied.CurrentVersion != before.CurrentVersion+1 {
		t.Fatal("confirmation did not apply exactly once")
	}
	// The same pending record cannot be reinterpreted as an ordinary switch.
	ordinary, _ := json.Marshal(control.SwitchRequest{OperationID: confirm.OperationID, GroupID: target.ID, ExpectedVersion: before.CurrentVersion})
	errorCode(t, nativeRequest(h, "PUT", "/api/v1/current-group", string(ordinary), token, nil), 409, "operation_conflict")
	response(t, nativeRequest(h, "PUT", "/api/v1/recommendations/confirm", string(body), token, nil), 200)
	loaded, err := control.New(disk.saved, disk, control.Options{})
	if err != nil {
		t.Fatal(err)
	}
	restarted, err := NewDevice(loaded, nil, deviceHost)
	if err != nil {
		t.Fatal(err)
	}
	terminal := response(t, nativeRequest(restarted, "GET", "/api/v1/operations/"+confirm.OperationID, "", token, nil), 200)
	var op control.Operation
	if err = json.Unmarshal(terminal["operation"], &op); err != nil {
		t.Fatal(err)
	}
	if op.Status != "applied" || op.RecommendationID != rec.RecommendationID || op.RecommendationEpoch != rec.ServerEpoch {
		t.Fatal("terminal lost recommendation binding")
	}
	response(t, nativeRequest(restarted, "PUT", "/api/v1/recommendations/confirm", string(body), token, nil), 200)
	current, _ := loaded.State(d.ID)
	if current.CurrentGroupID != target.ID || current.CurrentVersion != applied.CurrentVersion || current.ServerEpoch == before.ServerEpoch {
		t.Fatal("replay changed current group or version after restart")
	}
	errorCode(t, nativeRequest(restarted, "GET", "/api/v1/operations/"+confirm.OperationID, "", otherToken, nil), 404, "operation_not_found")
}
