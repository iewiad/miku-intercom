package httpapi

import (
	"net/http"
	"strings"
	"testing"
)

func TestVoiceHTTPAuthSchemaResolveAndDefaultUnavailable(t *testing.T) {
	f := fixture(t)
	_, _, _, e := f.s.CreateGroup(f.a.ID, "后勤组")
	if e != nil {
		t.Fatal(e)
	}
	for _, tc := range []struct {
		method, path, body, token string
		status                    int
	}{
		{"POST", "/api/v1/voice/resolve", `{"transcript":"切到后勤组"}`, "", 401},
		{"POST", "/api/v1/voice/resolve", `{"transcript":"切到后勤组"}`, f.tokenA, 200},
		{"POST", "/api/v1/voice/resolve", `{"transcript":"切到后勤组"}`, f.tokenB, 422},
		{"POST", "/api/v1/voice/resolve", `{"transcript":"切到后勤组","final":true}`, f.tokenA, 400},
		{"GET", "/api/v1/voice/resolve", "", f.tokenA, 405},
		{"GET", "/api/v1/voice", "", f.tokenA, 200},
		{"POST", "/api/v1/voice/sessions", `{"command_id":"command_123"}`, f.tokenA, 503},
	} {
		r := request(f.h, tc.method, tc.path, tc.body, tc.token, nil)
		if r.Code != tc.status {
			t.Fatalf("%s %s status=%d want %d", tc.method, tc.path, r.Code, tc.status)
		}
		if tc.path == "/api/v1/voice" && !strings.Contains(r.Body.String(), `"available":false`) {
			t.Fatal("unavailable hidden")
		}
	}
	if r := request(f.h, http.MethodPost, "/api/v1/voice/resolve", `{"transcript":"切到后勤组"}`, f.tokenA, map[string]string{"Origin": "https://attacker.example"}); r.Code != 403 {
		t.Fatal("cross origin voice accepted")
	}
}

func TestVoiceHTTPPunctuationAmbiguityDoesNotSilentlyChooseAnotherGroup(t *testing.T) {
	f := fixture(t)
	for _, name := range []string{"后勤", "后勤！", "字面标点群！！"} {
		if _, _, _, e := f.s.CreateGroup(f.a.ID, name); e != nil {
			t.Fatal(e)
		}
	}
	before, _ := f.s.State(f.a.ID)
	r := request(f.h, "POST", "/api/v1/voice/resolve", `{"transcript":"切到后勤！"}`, f.tokenA, nil)
	if r.Code != 422 || !strings.Contains(r.Body.String(), `"code":"voice_target_ambiguous"`) {
		t.Fatalf("ambiguous target accepted status=%d", r.Code)
	}
	r = request(f.h, "POST", "/api/v1/voice/resolve", `{"transcript":"切换到字面标点群！！！。"}`, f.tokenA, nil)
	if r.Code != 200 || !strings.Contains(r.Body.String(), `"name":"字面标点群！！"`) {
		t.Fatalf("literal punctuation group rejected status=%d", r.Code)
	}
	after, _ := f.s.State(f.a.ID)
	if before.CurrentGroupID != after.CurrentGroupID || before.CurrentVersion != after.CurrentVersion {
		t.Fatal("read-only resolution mutated state")
	}
}
