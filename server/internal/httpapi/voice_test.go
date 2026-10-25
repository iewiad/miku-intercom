package httpapi

import (
	"bytes"
	"encoding/json"
	"log"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/iewiad/miku-intercom/server/internal/control"
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

func voiceLogFields(t *testing.T, output string) map[string]string {
	t.Helper()
	if strings.Count(output, "\n") != 1 {
		t.Fatalf("expected one log record, got %q", output)
	}
	parts := strings.Fields(output)
	if len(parts) != 11 || parts[0] != "voice_resolve" {
		t.Fatalf("unexpected diagnostic schema: %q", output)
	}
	fields := map[string]string{}
	for _, part := range parts[1:] {
		pair := strings.SplitN(part, "=", 2)
		if len(pair) != 2 {
			t.Fatalf("invalid diagnostic field: %q", part)
		}
		fields[pair[0]] = pair[1]
	}
	for _, name := range []string{"input_evaluated", "utf8_valid", "input_runes", "input_bytes", "accepted_prefix", "candidate_count", "exact_match_count", "outcome", "error_code", "elapsed_ms"} {
		if _, ok := fields[name]; !ok {
			t.Fatalf("missing diagnostic field %q", name)
		}
	}
	if elapsed, err := strconv.ParseInt(fields["elapsed_ms"], 10, 64); err != nil || elapsed < 0 {
		t.Fatal("elapsed_ms must be a nonnegative integer")
	}
	return fields
}

func TestVoiceHTTPResolutionLogsFixedMetadata(t *testing.T) {
	cases := []struct {
		name, text, prefix, outcome, code string
		status, matches                   int
	}{
		{"success", "切到PRIVATE-GROUP", "switch_short", "success", "none", 200, 1},
		{"invalid command", "不要切到PRIVATE-GROUP", "none", "invalid_command", "invalid_voice_command", 400, 0},
		{"unknown target with log injection", "切换到PRIVATE-TRANSCRIPT\noutcome=success", "switch_to", "unknown_target", "voice_target_unknown", 422, 0},
		{"ambiguous punctuation", "进入PRIVATE-GROUP！", "enter", "ambiguous_target", "voice_target_ambiguous", 422, 2},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			f := fixture(t)
			var logOutput bytes.Buffer
			f.h.(*Server).voiceLogf = log.New(&logOutput, "", 0).Printf
			var groupIDs []string
			for _, name := range []string{"PRIVATE-GROUP", "PRIVATE-GROUP！"} {
				group, _, _, err := f.s.CreateGroup(f.a.ID, name)
				if err != nil {
					t.Fatal(err)
				}
				groupIDs = append(groupIDs, group.ID)
			}
			body, err := json.Marshal(map[string]string{"transcript": tc.text})
			if err != nil {
				t.Fatal(err)
			}
			r := request(f.h, "POST", "/api/v1/voice/resolve", string(body), f.tokenA, nil)
			if tc.code == "none" {
				response(t, r, tc.status)
			} else {
				errorCode(t, r, tc.status, tc.code)
			}
			fields := voiceLogFields(t, logOutput.String())
			want := map[string]string{
				"input_evaluated": "true", "utf8_valid": "true", "input_runes": strconv.Itoa(utf8.RuneCountInString(tc.text)), "input_bytes": strconv.Itoa(len(tc.text)),
				"accepted_prefix": tc.prefix, "candidate_count": "2", "exact_match_count": strconv.Itoa(tc.matches), "outcome": tc.outcome, "error_code": tc.code,
			}
			for key, value := range want {
				if fields[key] != value {
					t.Fatalf("%s = %q, want %q", key, fields[key], value)
				}
			}
			for _, secret := range append(groupIDs, tc.text, "PRIVATE-GROUP", "PRIVATE-TRANSCRIPT", f.tokenA, f.tokenB, f.a.ID, f.b.ID) {
				if strings.Contains(logOutput.String(), secret) {
					t.Fatal("diagnostic log contains private command, group, identity, or credential data")
				}
			}
		})
	}
}

func TestVoiceHTTPDecodeRejectLogDoesNotIncludePartialRequest(t *testing.T) {
	cases := []struct {
		name, body, contentType, code string
		status                        int
	}{
		{"unknown field", `{"transcript":"PRIVATE-TRANSCRIPT","sdp":"PRIVATE-SDP","token":"PRIVATE-TOKEN"}`, "application/json", "invalid_json", 400},
		{"wrong transcript type", `{"transcript":["PRIVATE-TRANSCRIPT"]}`, "application/json", "invalid_json", 400},
		{"malformed JSON", `{"transcript":"PRIVATE-TRANSCRIPT"`, "application/json", "invalid_json", 400},
		{"body limit", `{"transcript":"PRIVATE-TRANSCRIPT` + strings.Repeat("x", 8200) + `"}`, "application/json", "body_too_large", 413},
		{"content type", `{"transcript":"PRIVATE-TRANSCRIPT"}`, "text/plain", "unsupported_media_type", 415},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			f := fixture(t)
			var logOutput bytes.Buffer
			f.h.(*Server).voiceLogf = log.New(&logOutput, "", 0).Printf
			r := request(f.h, "POST", "/api/v1/voice/resolve", tc.body, f.tokenA, map[string]string{"Content-Type": tc.contentType})
			errorCode(t, r, tc.status, tc.code)
			fields := voiceLogFields(t, logOutput.String())
			for key, want := range map[string]string{
				"input_evaluated": "false", "utf8_valid": "false", "input_runes": "0", "input_bytes": "0", "accepted_prefix": "none", "candidate_count": "0", "exact_match_count": "0", "outcome": "invalid_input", "error_code": tc.code,
			} {
				if fields[key] != want {
					t.Fatalf("%s = %q, want %q", key, fields[key], want)
				}
			}
			for _, secret := range []string{"PRIVATE-TRANSCRIPT", "PRIVATE-SDP", "PRIVATE-TOKEN", f.tokenA, f.a.ID} {
				if strings.Contains(logOutput.String(), secret) {
					t.Fatal("decode rejection log contains request or identity data")
				}
			}
		})
	}
}

func TestVoiceHTTPServiceFailureLogsOnlyFixedCode(t *testing.T) {
	f := fixture(t)
	server := f.h.(*Server)
	var logOutput bytes.Buffer
	server.voiceLogf = log.New(&logOutput, "", 0).Printf
	r := httptest.NewRequest("POST", "/api/v1/voice/resolve", strings.NewReader(`{"transcript":"切到PRIVATE-TRANSCRIPT"}`))
	r.Header.Set("Content-Type", "application/json")
	err := server.handleVoice(httptest.NewRecorder(), r, "/voice/resolve", "PRIVATE-IDENTITY")
	if e, ok := err.(*control.Error); !ok || e.Code != "unauthorized" {
		t.Fatalf("unexpected service error: %v", err)
	}
	fields := voiceLogFields(t, logOutput.String())
	if fields["input_evaluated"] != "true" || fields["outcome"] != "service_error" || fields["error_code"] != "unauthorized" || fields["candidate_count"] != "0" || fields["exact_match_count"] != "0" {
		t.Fatalf("unexpected service diagnostics: %q", logOutput.String())
	}
	for _, secret := range []string{"PRIVATE-TRANSCRIPT", "PRIVATE-IDENTITY", "valid identity required"} {
		if strings.Contains(logOutput.String(), secret) {
			t.Fatal("service failure log contains private input or raw error text")
		}
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
