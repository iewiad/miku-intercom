package httpapi

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

func TestVoiceProposalHTTPUnicodeBeforeModel(t *testing.T) {
	for name, tc := range map[string]struct{ raw, code string }{
		"bad_utf8":              {`"broken` + string([]byte{0xff}) + `"`, "invalid_json"},
		"high":                  {`"\uD800"`, "invalid_json"},
		"low":                   {`"\uDC00"`, "invalid_json"},
		"reverse":               {`"\uDC00\uD800"`, "invalid_json"},
		"unpaired":              {`"\uD800x"`, "invalid_json"},
		"control":               {`"切到\u0000后勤"`, "invalid_recommendation_request"},
		"valid_pair":            {`"\uD83D\uDE00"`, ""},
		"literal_escape":        {`"literal\\uD800"`, ""},
		"replacement_character": {`"\uFFFD"`, ""},
	} {
		t.Run(name, func(t *testing.T) {
			model := &apiRecommendationModel{}
			s, err := control.New(nil, nil, control.Options{Recommender: model})
			if err != nil {
				t.Fatal(err)
			}
			device, token, err := s.CreateDevice("Unicode合成终端")
			if err != nil {
				t.Fatal(err)
			}
			_, _, _, err = s.CreateGroup(device.ID, "合成候选群")
			if err != nil {
				t.Fatal(err)
			}
			st, _ := s.State(device.ID)
			body, _ := json.Marshal(control.VoiceProposalRequest{RequestID: "unicode_voice_001", Transcript: "PLACEHOLDER", ExpectedVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch})
			bodyText := strings.Replace(string(body), `"PLACEHOLDER"`, tc.raw, 1)
			reply := request(New(s), "POST", "/api/v1/voice/proposals", bodyText, token, nil)
			if tc.code != "" {
				errorCode(t, reply, 400, tc.code)
				if model.calls != 0 {
					t.Fatal("invalid Unicode/control transcript reached model")
				}
			} else {
				response(t, reply, 200)
				if model.calls != 1 {
					t.Fatal("valid Unicode was rejected or rewritten")
				}
			}
		})
	}
}

func TestVoiceProposalHTTPExactAndExistingConfirmation(t *testing.T) {
	f := fixture(t)
	if _, _, _, err := f.s.CreateGroup(f.a.ID, "后勤组"); err != nil {
		t.Fatal(err)
	}
	before, err := f.s.State(f.a.ID)
	if err != nil {
		t.Fatal(err)
	}
	req := control.VoiceProposalRequest{RequestID: "http_voice_exact_001", Transcript: "切到后勤组",
		ExpectedVersion: before.CurrentVersion, ServerEpoch: before.ServerEpoch}
	body, _ := json.Marshal(req)
	out := response(t, request(f.h, "POST", "/api/v1/voice/proposals", string(body), f.tokenA, nil), 200)
	var rec control.Recommendation
	if err = json.Unmarshal(out["recommendation"], &rec); err != nil {
		t.Fatal(err)
	}
	if rec.Source != "exact" || len(rec.Suggestions) != 1 || rec.Suggestions[0].Reason != control.VoiceTargetReasonNote {
		t.Fatalf("missing exact review contract: %+v", rec)
	}
	after, _ := f.s.State(f.a.ID)
	if after.CurrentGroupID != before.CurrentGroupID || after.CurrentVersion != before.CurrentVersion {
		t.Fatal("HTTP proposal switched without confirmation")
	}
	confirm := control.RecommendationConfirmation{RecommendationID: rec.RecommendationID, OperationID: "http_voice_confirm_001",
		GroupID: rec.Suggestions[0].GroupID, ExpectedVersion: rec.CurrentVersion, ServerEpoch: rec.ServerEpoch}
	body, _ = json.Marshal(confirm)
	out = response(t, request(f.h, "PUT", "/api/v1/recommendations/confirm", string(body), f.tokenA, nil), 200)
	var op control.Operation
	if err = json.Unmarshal(out["operation"], &op); err != nil {
		t.Fatal(err)
	}
	if op.Status != "applied" || op.RecommendationID != rec.RecommendationID {
		t.Fatal("voice review did not use recommendation confirmation")
	}
	response(t, request(f.h, "PUT", "/api/v1/recommendations/confirm", string(body), f.tokenA, nil), 200)
}

func TestVoiceProposalHTTPSchemaAuthAndUnavailable(t *testing.T) {
	f := fixture(t)
	if _, _, _, err := f.s.CreateGroup(f.a.ID, "后勤组"); err != nil {
		t.Fatal(err)
	}
	st, _ := f.s.State(f.a.ID)
	req := control.VoiceProposalRequest{RequestID: "http_voice_gate_001", Transcript: "帮我转到后秦组", ExpectedVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch}
	body, _ := json.Marshal(req)
	errorCode(t, request(f.h, "POST", "/api/v1/voice/proposals", string(body), "", nil), 401, "unauthorized")
	errorCode(t, request(f.h, "GET", "/api/v1/voice/proposals", "", f.tokenA, nil), 405, "method_not_allowed")
	errorCode(t, request(f.h, "POST", "/api/v1/voice/proposals", string(body), f.tokenA, map[string]string{"Origin": "https://hostile.example"}), 403, "origin_forbidden")
	errorCode(t, request(f.h, "POST", "/api/v1/voice/proposals", `{"request_id":"voice_missing_001","transcript":"切到后勤组","server_epoch":"epoch001"}`, f.tokenA, nil), 400, "invalid_request")
	errorCode(t, request(f.h, "POST", "/api/v1/voice/proposals", string(body[:len(body)-1])+`,"candidates":[]}`, f.tokenA, nil), 400, "invalid_json")
	errorCode(t, request(f.h, "POST", "/api/v1/voice/proposals", string(body[:len(body)-1])+`,"source":"exact"}`, f.tokenA, nil), 400, "invalid_json")
	errorCode(t, request(f.h, "POST", "/api/v1/voice/proposals", string(body), f.tokenA, nil), 503, "recommendation_unavailable")
	// Legacy resolve keeps its original response and strict matching semantics.
	out := response(t, request(f.h, "POST", "/api/v1/voice/resolve", `{"transcript":"切到后勤组"}`, f.tokenA, nil), 200)
	if out["group_id"] == nil || out["recommendation"] != nil {
		t.Fatal("legacy resolve response changed")
	}
}

func TestVoiceProposalHTTPModelAndNativeGate(t *testing.T) {
	model := &apiRecommendationModel{}
	s, err := control.New(nil, nil, control.Options{Recommender: model})
	if err != nil {
		t.Fatal(err)
	}
	device, token, err := s.CreateDevice("合成语音目标终端")
	if err != nil {
		t.Fatal(err)
	}
	_, _, _, err = s.CreateGroup(device.ID, "合成后勤组")
	if err != nil {
		t.Fatal(err)
	}
	st, _ := s.State(device.ID)
	body, _ := json.Marshal(control.VoiceProposalRequest{RequestID: "native_voice_001", Transcript: "帮我转去合成后秦组", ExpectedVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch})
	h, err := NewDevice(s, nil, deviceHost)
	if err != nil {
		t.Fatal(err)
	}
	for _, bad := range []struct {
		code   string
		mutate func(*http.Request)
	}{
		{"tls_required", func(r *http.Request) { r.TLS = nil }},
		{"host_forbidden", func(r *http.Request) { r.Host = "untrusted.example" }},
		{"origin_forbidden", func(r *http.Request) { r.Header.Set("Origin", "https://"+deviceHost) }},
	} {
		errorCode(t, nativeRequest(h, "POST", "/api/v1/voice/proposals", string(body), token, bad.mutate), 403, bad.code)
	}
	errorCode(t, nativeRequest(h, "POST", "/api/v1/voice/proposals", string(body), "forged", nil), 401, "unauthorized")
	if model.calls != 0 {
		t.Fatal("transport/auth rejection reached model")
	}
	out := response(t, nativeRequest(h, "POST", "/api/v1/voice/proposals", string(body), token, nil), 200)
	var rec control.Recommendation
	if json.Unmarshal(out["recommendation"], &rec) != nil || rec.Source != "model" || model.calls != 1 || model.input.Intent != control.VoiceTargetIntent {
		t.Fatal("native model proposal contract failed")
	}
	response(t, nativeRequest(h, "POST", "/api/v1/voice/proposals", string(body), token, nil), 200)
	if model.calls != 1 {
		t.Fatal("native retry repeated provider request")
	}
}
