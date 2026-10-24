package httpapi

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/media"
)

func mediaAPI(t *testing.T, diagnostics bool) *apiFixture {
	t.Helper()
	f := fixture(t)
	hub, err := media.New(f.s, media.Options{Diagnostics: diagnostics})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(hub.Close)
	f.h = New(f.s, hub)
	return f
}
func TestMediaHTTPAuthCapabilityAndStaticFlags(t *testing.T) {
	f := mediaAPI(t, false)
	errorCode(t, request(f.h, "GET", "/api/v1/media", "", "", nil), 401, "unauthorized")
	out := response(t, request(f.h, "GET", "/api/v1/media", "", f.tokenA, nil), 200)
	var status media.Status
	if err := json.Unmarshal(mustJSON(t, out), &status); err != nil {
		t.Fatal(err)
	}
	if !status.Available || status.DiagnosticsEnabled || status.Phase != "none" || status.PeerID != "" || status.Codec != "audio/opus" || status.ClockRate != 48000 || status.ServerEpoch == "" {
		t.Fatal("invalid capability")
	}
	for _, path := range []string{"/diagnostics", "/diagnostics.js", "/lifecycle.js", "/lifecycle.test.js", "/audio.test.js", "/diagnostics.test.js"} {
		errorCode(t, request(f.h, "GET", path, "", "", nil), 404, "not_found")
	}
	if w := request(f.h, "GET", "/audio.js", "", "", nil); w.Code != 200 {
		t.Fatal("audio.js unavailable")
	}
	yes := mediaAPI(t, true)
	for _, path := range []string{"/diagnostics", "/diagnostics.js", "/lifecycle.js"} {
		w := request(yes.h, "GET", path, "", "", nil)
		if w.Code != 200 || w.Header().Get("Cache-Control") != "no-store" {
			t.Fatal("diagnostics not gated correctly")
		}
	}
	errorCode(t, request(f.h, "POST", "/api/v1/media", "{}", f.tokenA, nil), 405, "method_not_allowed")
	errorCode(t, request(f.h, "DELETE", "/api/v1/media/peers/valid_id1", "", f.tokenA, map[string]string{"Origin": "http://attacker.example"}), 403, "origin_forbidden")
	response(t, request(f.h, "DELETE", "/api/v1/media/peers/valid_id1", "", f.tokenA, nil), 200)
}
func mustJSON(t *testing.T, value any) []byte {
	t.Helper()
	b, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return b
}
func TestMediaHTTPStrictSDPAndBodyException(t *testing.T) {
	f := mediaAPI(t, false)
	_, _, st, err := f.s.CreateGroup(f.a.ID, "合作")
	if err != nil {
		t.Fatal(err)
	}
	validSDP := "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\na=sendrecv\r\n"
	body := map[string]any{"peer_id": "media001", "replace_peer_id": "", "group_id": st.CurrentGroupID, "current_version": st.CurrentVersion, "server_epoch": st.ServerEpoch, "valid_until": time.Now().Add(9 * time.Second).Format(time.RFC3339Nano), "offer": map[string]any{"type": "offer", "sdp": validSDP}}
	nested := body["offer"].(map[string]any)
	nested["extra"] = true
	errorCode(t, request(f.h, "POST", "/api/v1/media/peers", string(mustJSON(t, body)), f.tokenA, nil), 400, "invalid_json")
	delete(nested, "extra")
	body["extra"] = true
	errorCode(t, request(f.h, "POST", "/api/v1/media/peers", string(mustJSON(t, body)), f.tokenA, nil), 400, "invalid_json")
	delete(body, "extra")
	// This offer exceeds 8 KiB but reaches SDP negotiation (and is then rejected
	// for missing mandatory ICE/DTLS attributes), proving the 64 KiB exception.
	nested["sdp"] = validSDP + "a=x-padding:" + strings.Repeat("x", 9000) + "\r\n"
	errorCode(t, request(f.h, "POST", "/api/v1/media/peers", string(mustJSON(t, body)), f.tokenA, nil), 400, "invalid_offer")
	body["peer_id"] = "media002"
	nested["sdp"] = validSDP + "a=x-padding:" + strings.Repeat("x", 66000)
	errorCode(t, request(f.h, "POST", "/api/v1/media/peers", string(mustJSON(t, body)), f.tokenA, nil), 413, "body_too_large")
	nested["sdp"] = validSDP
	delete(body, "replace_peer_id")
	errorCode(t, request(f.h, "POST", "/api/v1/media/peers", string(mustJSON(t, body)), f.tokenA, nil), 400, "invalid_request")
	body["replace_peer_id"] = ""
	body["valid_until"] = "invalid"
	errorCode(t, request(f.h, "POST", "/api/v1/media/peers", string(mustJSON(t, body)), f.tokenA, nil), 400, "invalid_deadline")
}
