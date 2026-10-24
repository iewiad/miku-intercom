package rta

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func testProxy(t *testing.T, handler func(http.ResponseWriter, *http.Request)) (*Proxy, *httptest.Server) {
	t.Helper()
	srv := httptest.NewTLSServer(http.HandlerFunc(handler))
	t.Cleanup(srv.Close)
	p, e := New(Config{RTICURL: srv.URL, RTCOrigin: srv.URL, Token: "test-private-key", AppID: "test-app", ModelProfile: "qwen3.5-omni-plus-realtime", RoleProfile: "default_assistant", VoiceProfile: "Tina", Region: "cn-east"}, srv.Client())
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(p.Close)
	return p, srv
}
func created(w http.ResponseWriter, base string) {
	fmt.Fprintf(w, `{"session_id":"session_123","client_endpoint":{"url":%q,"access_token":"short-private-token","expires_at_ms":%d}}`, base+"/v1/realtime/sessions/session_123/rtc", time.Now().Add(time.Minute).UnixMilli())
}
func terminal(w http.ResponseWriter) {
	fmt.Fprint(w, `{"session":{"session_id":"session_123","state":"ended","ended_at_ms":1}}`)
}
func TestProxyPrivateTokensIdempotencyAndScopedStop(t *testing.T) {
	var base string
	var creates, offers, stops atomic.Int32
	p, srv := testProxy(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/rtic/v1/realtime/sessions":
			creates.Add(1)
			if r.Header.Get("Authorization") != "Bearer test-private-key" {
				t.Error("missing API auth")
			}
			var b map[string]any
			_ = json.NewDecoder(r.Body).Decode(&b)
			if b["model_profile"] != "qwen3.5-omni-plus-realtime" || b["client_transport"] != "platform_rtc" {
				t.Error("route changed")
			}
			created(w, base)
		case "/v1/realtime/sessions/session_123/rtc":
			offers.Add(1)
			if r.Header.Get("Authorization") != "Bearer short-private-token" {
				t.Error("wrong endpoint token")
			}
			fmt.Fprint(w, `{"type":"answer","sdp":"v=0\r\n","access_token":"must-not-leak"}`)
		case "/rtic/v1/realtime/sessions/session_123":
			if r.Method != http.MethodGet || r.Header.Get("Authorization") != "Bearer test-private-key" {
				t.Error("incorrect terminal query scope")
			}
			terminal(w)
		case "/rtic/v1/realtime/sessions/session_123/stop":
			stops.Add(1)
			w.WriteHeader(202)
		default:
			t.Error("unexpected path")
			w.WriteHeader(404)
		}
	})
	base = srv.URL
	if e := p.Start("device-a", "command_123"); e != nil {
		t.Fatal(e)
	}
	if e := p.Start("device-a", "command_123"); e != nil {
		t.Fatal(e)
	}
	if e := p.Start("device-a", "command_456"); e == nil {
		t.Fatal("parallel session accepted")
	}
	if p.Status("device-b").CommandID != "" {
		t.Fatal("identity disclosure")
	}
	if _, e := p.Offer(context.Background(), "device-b", "command_123", nil); e == nil {
		t.Fatal("unscoped offer")
	}
	offer := json.RawMessage(`{"type":"offer","sdp":"v=0"}`)
	a, e := p.Offer(context.Background(), "device-a", "command_123", offer)
	if e != nil {
		t.Fatal(e)
	}
	if strings.Contains(string(a), "token") {
		t.Fatal("upstream token leaked")
	}
	if _, e = p.Offer(context.Background(), "device-a", "command_123", offer); e != nil {
		t.Fatal(e)
	}
	if _, e = p.Offer(context.Background(), "device-a", "command_123", json.RawMessage(`{}`)); e == nil {
		t.Fatal("changed offer accepted")
	}
	if e = p.Stop("device-a", "command_123"); e != nil {
		t.Fatal(e)
	}
	if e = p.Stop("device-a", "command_123"); e != nil {
		t.Fatal(e)
	}
	if p.Status("device-a").CommandID != "" || creates.Load() != 1 || offers.Load() != 1 || stops.Load() != 1 {
		t.Fatal("idempotency or cleanup failed")
	}
	if e = p.Start("device-a", "command_123"); e == nil {
		t.Fatal("ended session resurrected")
	}
}
func TestProxyStopFailureRemainsBlockedAndRetriesSameSession(t *testing.T) {
	var base string
	var stop atomic.Int32
	p, srv := testProxy(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			terminal(w)
			return
		}
		if strings.HasSuffix(r.URL.Path, "/stop") {
			if stop.Add(1) == 1 {
				w.WriteHeader(503)
			} else {
				w.WriteHeader(200)
			}
			return
		}
		created(w, base)
	})
	base = srv.URL
	if e := p.Start("a", "command_123"); e != nil {
		t.Fatal(e)
	}
	if e := p.Stop("a", "command_123"); e == nil {
		t.Fatal("failed stop confirmed")
	}
	if p.Status("a").Phase != "stop_unconfirmed" {
		t.Fatal("failed stop forgotten")
	}
	if e := p.Start("a", "command_456"); e == nil {
		t.Fatal("new session during unconfirmed cleanup")
	}
	if e := p.Stop("a", "command_123"); e != nil {
		t.Fatal(e)
	}
	if stop.Load() != 2 || p.Status("a").CommandID != "" {
		t.Fatal("retry did not finish")
	}
}
func TestProxyCancelBeforeCreateCannotResurrect(t *testing.T) {
	var n atomic.Int32
	p, _ := testProxy(t, func(w http.ResponseWriter, r *http.Request) { n.Add(1) })
	if e := p.Stop("a", "command_123"); e != nil {
		t.Fatal(e)
	}
	if e := p.Start("a", "command_123"); e == nil || n.Load() != 0 {
		t.Fatal("late create resurrected")
	}
}
func TestProxyUnknownCreateAndInvalidEndpointFailClosed(t *testing.T) {
	t.Run("unknown create", func(t *testing.T) {
		p, _ := testProxy(t, func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(502) })
		if p.Start("a", "command_123") == nil || p.Stop("a", "command_123") == nil || p.Start("a", "command_456") == nil {
			t.Fatal("unknown creation released")
		}
	})
	t.Run("endpoint not allowlisted", func(t *testing.T) {
		var stops atomic.Int32
		p, _ := testProxy(t, func(w http.ResponseWriter, r *http.Request) {
			if r.Method == http.MethodGet {
				terminal(w)
				return
			}
			if strings.HasSuffix(r.URL.Path, "/stop") {
				stops.Add(1)
				return
			}
			created(w, "https://untrusted.example")
		})
		if p.Start("a", "command_123") == nil {
			t.Fatal("untrusted endpoint accepted")
		}
		if p.Stop("a", "command_123") != nil || stops.Load() != 1 {
			t.Fatal("invalid endpoint session not cleaned")
		}
	})
}
func TestProxyRedirectNotFollowed(t *testing.T) {
	var followed atomic.Int32
	target := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { followed.Add(1) }))
	defer target.Close()
	p, _ := testProxy(t, func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, target.URL, 307) })
	if p.Start("a", "command_123") == nil || followed.Load() != 0 {
		t.Fatal("redirect followed")
	}
}
func TestPrivateConfigAndStrictFields(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "voice.json")
	_ = os.WriteFile(path, []byte(`{}`), 0644)
	if _, e := Load(path); e == nil {
		t.Fatal("public config accepted")
	}
	_ = os.Chmod(path, 0600)
	if _, e := Load(path); e == nil {
		t.Fatal("missing explicit config accepted")
	}
	for _, value := range []string{"http://example.com", "https://user:pass@example.com", "https://example.com/path", "https://example.com?token=x"} {
		if _, e := origin(value); e == nil {
			t.Fatal("unsafe origin accepted")
		}
	}
}

func TestJournalRestoresCleanupOnlyAndRetainsUnknownCreation(t *testing.T) {
	for _, unknown := range []bool{false, true} {
		t.Run(fmt.Sprint(unknown), func(t *testing.T) {
			var base string
			var stops atomic.Int32
			p, srv := testProxy(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method == http.MethodGet {
					terminal(w)
					return
				}
				if strings.HasSuffix(r.URL.Path, "/stop") {
					stops.Add(1)
					return
				}
				if unknown {
					w.WriteHeader(502)
					return
				}
				created(w, base)
			})
			base = srv.URL
			dir := t.TempDir()
			_ = os.Chmod(dir, 0700)
			path := filepath.Join(dir, "voice.json")
			if e := p.openJournal(path); e != nil {
				t.Fatal(e)
			}
			err := p.Start("device_123", "command_123")
			if !unknown && err != nil {
				t.Fatal(err)
			}
			// Emulate process loss: close the private file lock without calling upstream stop.
			p.closeOnce.Do(func() { p.mu.Lock(); p.closed = true; close(p.done); p.mu.Unlock(); _ = p.journal.Close() })
			resumed, e := New(p.config, srv.Client())
			if e != nil {
				t.Fatal(e)
			}
			if e = resumed.openJournal(path); e != nil {
				t.Fatal(e)
			}
			defer resumed.Close()
			if resumed.Status("device_123").CommandID != "command_123" || resumed.Status("device_123").Phase != "stop_unconfirmed" {
				t.Fatal("pending session forgotten")
			}
			if resumed.Start("device_123", "command_456") == nil {
				t.Fatal("recovered session allowed new command")
			}
			e = resumed.Stop("device_123", "command_123")
			if unknown {
				if e == nil || stops.Load() != 0 {
					t.Fatal("unknown creation treated as stopped")
				}
			} else if e != nil || stops.Load() != 1 {
				t.Fatal("known session not cleaned after restart")
			}
			body, _ := os.ReadFile(path)
			if strings.Contains(string(body), "test-private-key") || strings.Contains(string(body), "short-private-token") {
				t.Fatal("secret journal content")
			}
		})
	}
}
func TestJournalFailurePreventsOutboundCreate(t *testing.T) {
	var calls atomic.Int32
	p, _ := testProxy(t, func(w http.ResponseWriter, r *http.Request) { calls.Add(1) })
	dir := t.TempDir()
	_ = os.Chmod(dir, 0700)
	if e := p.openJournal(filepath.Join(dir, "voice.json")); e != nil {
		t.Fatal(e)
	}
	_ = p.journal.Close()
	if p.Start("device_123", "command_123") == nil || calls.Load() != 0 {
		t.Fatal("created without durable pending marker")
	}
}

func TestExplicitNumericNodePolicy(t *testing.T) {
	config := Config{RTICURL: "https://gateway.example", Token: "test-private-key", AppID: "test-app", RTCNodeDomain: "mikudns.com", RTCNodePort: 7882, ModelProfile: "test-model", RoleProfile: "test-role", VoiceProfile: "test-voice", Region: "test-region"}
	p, e := New(config, nil)
	if e != nil {
		t.Fatal(e)
	}
	defer p.Close()
	path := "/v1/realtime/sessions/session_123/rtc"
	for _, host := range []string{"2059633424.mikudns.com:7882", "2059633436.mikudns.com:7882", "123.MIKUDNS.COM:7882"} {
		if _, e := p.endpoint("https://"+host+path, "session_123"); e != nil {
			t.Errorf("valid node rejected: %s", host)
		}
	}
	for _, raw := range []string{
		"http://2059633424.mikudns.com:7882" + path,
		"https://node.mikudns.com:7882" + path,
		"https://2059633424.mikudns.com.evil.example:7882" + path,
		"https://2059633424.evil.mikudns.com:7882" + path,
		"https://2059633424mikudns.com:7882" + path,
		"https://mikudns.com:7882" + path,
		"https://2059633424.mikudns.com:7883" + path,
		"https://2059633424.mikudns.com" + path,
		"https://2059633424.mikudns.com:07882" + path,
		"https://user@2059633424.mikudns.com:7882" + path,
		"https://2059633424.mikudns.com:7882" + path + "?",
		"https://2059633424.mikudns.com:7882" + path + "#",
		"https://2059633424.mikudns.com:7882" + path + "?token=x",
		"https://2059633424.mikudns.com:7882" + path + "#x",
		"https://2059633424.mikudns.com:7882/short/123",
		"https://2059633424.mikudns.com:7882/a/.." + path,
		"https://2059633424.mikudns.com:7882/v1/realtime/sessions/another_123/rtc",
		"https://" + strings.Repeat("1", 64) + ".mikudns.com:7882" + path,
	} {
		if _, e := p.endpoint(raw, "session_123"); e == nil {
			t.Errorf("unsafe endpoint accepted: %s", raw)
		}
	}
	fixed := config
	fixed.RTCOrigin = "https://fixed.example:7882"
	if _, e := New(fixed, nil); e == nil {
		t.Fatal("mixed fixed and dynamic policy accepted")
	}
	for _, domain := range []string{"", ".mikudns.com", "*.mikudns.com", "mikudns.com.", "https://mikudns.com", "a..mikudns.com", "mikudns.com:7882", "127.0.0.1"} {
		bad := config
		bad.RTCNodeDomain = domain
		if q, e := New(bad, nil); e == nil {
			q.Close()
			t.Errorf("bad node domain accepted: %q", domain)
		}
	}
	changed := config
	changed.RTCNodePort = 7883
	q, e := New(changed, nil)
	if e != nil {
		t.Fatal(e)
	}
	defer q.Close()
	if p.binding() == q.binding() {
		t.Fatal("node port missing from journal binding")
	}
	fixed.RTCNodeDomain = ""
	fixed.RTCNodePort = 0
	f, e := New(fixed, nil)
	if e != nil {
		t.Fatal(e)
	}
	defer f.Close()
	if p.binding() == f.binding() {
		t.Fatal("policy mode missing from journal binding")
	}
}

func TestStopAcknowledgementNeedsActualTerminalQuery(t *testing.T) {
	var base string
	var stops, queries atomic.Int32
	p, srv := testProxy(t, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/stop") {
			stops.Add(1)
			w.WriteHeader(202)
			return
		}
		if r.Method == http.MethodGet {
			if r.URL.Path != "/rtic/v1/realtime/sessions/session_123" || r.Header.Get("Authorization") != "Bearer test-private-key" {
				t.Error("terminal query escaped original gateway scope")
			}
			if queries.Add(1) == 1 {
				fmt.Fprint(w, `{"session":{"session_id":"session_123","state":"provider_connected"}}`)
			} else {
				terminal(w)
			}
			return
		}
		created(w, base)
	})
	base = srv.URL
	dir := t.TempDir()
	_ = os.Chmod(dir, 0700)
	path := filepath.Join(dir, "voice.json")
	if e := p.openJournal(path); e != nil {
		t.Fatal(e)
	}
	if e := p.Start("device_123", "command_123"); e != nil {
		t.Fatal(e)
	}
	if p.Stop("device_123", "command_123") == nil {
		t.Fatal("202 treated as terminal")
	}
	if p.Status("device_123").Phase != "stop_unconfirmed" || p.Start("device_123", "command_456") == nil {
		t.Fatal("nonterminal stop released gate")
	}
	body, _ := os.ReadFile(path)
	var snapshot journalSnapshot
	_ = json.Unmarshal(body, &snapshot)
	if snapshot.Records["device_123/command_123"].Stopped {
		t.Fatal("nonterminal stop committed")
	}
	if e := p.Stop("device_123", "command_123"); e != nil {
		t.Fatal(e)
	}
	if stops.Load() != 2 || queries.Load() != 2 || p.Status("device_123").CommandID != "" {
		t.Fatal("terminal retry failed")
	}
	body, _ = os.ReadFile(path)
	_ = json.Unmarshal(body, &snapshot)
	if !snapshot.Records["device_123/command_123"].Stopped {
		t.Fatal("terminal stop not committed")
	}
}
func TestStopQueryMustBeWellFormedMatchingAndSuccessful(t *testing.T) {
	cases := []struct {
		name, body string
		status     int
	}{
		{"wrong id", `{"session":{"session_id":"other_123","state":"ended"}}`, 200},
		{"malformed", `not json`, 200},
		{"unwrapped", `{"session_id":"session_123","state":"ended"}`, 200},
		{"missing state", `{"session":{"session_id":"session_123"}}`, 200},
		{"query failed", `{}`, 503},
		{"not found", `{}`, 404},
		{"oversize", strings.Repeat("x", 65537), 200},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var base string
			p, srv := testProxy(t, func(w http.ResponseWriter, r *http.Request) {
				if strings.HasSuffix(r.URL.Path, "/stop") {
					w.WriteHeader(202)
					return
				}
				if r.Method == http.MethodGet {
					w.WriteHeader(tc.status)
					fmt.Fprint(w, tc.body)
					return
				}
				created(w, base)
			})
			base = srv.URL
			if p.Start("device_123", "command_123") != nil {
				t.Fatal("create failed")
			}
			if p.Stop("device_123", "command_123") == nil || p.Status("device_123").Phase != "stop_unconfirmed" || p.Start("device_123", "command_456") == nil {
				t.Fatal("invalid terminal query released gate")
			}
		})
	}
	for _, state := range []string{"failed", "expired"} {
		t.Run(state, func(t *testing.T) {
			var base string
			p, srv := testProxy(t, func(w http.ResponseWriter, r *http.Request) {
				if strings.HasSuffix(r.URL.Path, "/stop") {
					w.WriteHeader(202)
					return
				}
				if r.Method == http.MethodGet {
					fmt.Fprintf(w, `{"session":{"session_id":"session_123","state":%q}}`, state)
					return
				}
				created(w, base)
			})
			base = srv.URL
			if p.Start("device_123", "command_123") != nil || p.Stop("device_123", "command_123") != nil {
				t.Fatal("valid terminal state rejected")
			}
		})
	}
}
