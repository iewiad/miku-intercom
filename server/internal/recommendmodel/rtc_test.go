package recommendmodel

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/rta"
	"github.com/pion/webrtc/v4"
)

func readyTurn(t *testing.T) *turn {
	t.Helper()
	s := newTurn("synthetic input")
	init := s.initialize()
	if !strings.Contains(string(init), `"modalities":["text"]`) || !strings.Contains(string(init), `"turn_detection":null`) || !strings.Contains(string(init), `"max_response_output_tokens":512`) {
		t.Fatal("text-only bounded configuration missing")
	}
	sends, done := s.receive([]byte(`{"type":"session.updated"}`))
	if done || len(sends) != 2 || !strings.Contains(string(sends[0]), `"type":"input_text"`) || !strings.Contains(string(sends[1]), `"modalities":["text"]`) {
		t.Fatal("explicit single text turn not sent")
	}
	if sends, _ = s.receive([]byte(`{"type":"session.updated"}`)); len(sends) != 0 {
		t.Fatal("duplicate update generated a second turn")
	}
	s.receive([]byte(`{"type":"response.created","response":{"id":"response_one"}}`))
	return s
}

func TestTurnFinalAndAllFailureBoundaries(t *testing.T) {
	s := readyTurn(t)
	s.receive([]byte(`{"type":"response.text.delta","response_id":"response_one","delta":"{\"suggestions\":[]}"}`))
	_, done := s.receive([]byte(`{"type":"response.done","response":{"id":"response_one","status":"completed"}}`))
	if !done || !s.completed || s.failed || s.text != `{"suggestions":[]}` {
		t.Fatal("matching completed text not accepted")
	}
	s.receive([]byte(`{"type":"response.text.delta","response_id":"response_one","delta":"late"}`))
	if s.text != `{"suggestions":[]}` {
		t.Fatal("late text mutated terminal result")
	}
	for label, event := range map[string][]byte{
		"wrong_response":     []byte(`{"type":"response.text.delta","response_id":"other","delta":"bad"}`),
		"missing_response":   []byte(`{"type":"response.done","response":{"status":"completed"}}`),
		"incomplete":         []byte(`{"type":"response.done","response":{"id":"response_one","status":"incomplete"}}`),
		"duplicate_response": []byte(`{"type":"response.created","response":{"id":"response_two"}}`),
		"audio":              []byte(`{"type":"response.audio.delta","delta":"not-decoded"}`),
		"tool":               []byte(`{"type":"response.function_call_arguments.delta","delta":"do-not-run"}`),
		"provider_error":     []byte(`{"type":"rtid.error","message":"must-not-be-logged"}`),
		"closed":             []byte(`{"type":"session.closed"}`),
		"bad_json":           []byte(`{`),
		"bad_utf8":           {0xff},
		"text_size":          encode(map[string]any{"type": "response.text.delta", "response_id": "response_one", "delta": strings.Repeat("x", 4097)}),
		"event_size":         []byte(strings.Repeat("x", 32769)),
	} {
		t.Run(label, func(t *testing.T) {
			s := readyTurn(t)
			_, done := s.receive(event)
			if !done || !s.failed {
				t.Fatal("invalid event accepted")
			}
		})
	}
	s = readyTurn(t)
	for i := 0; i < 257; i++ {
		s.receive([]byte(`{"type":"unrecognized"}`))
	}
	if !s.failed {
		t.Fatal("unbounded event stream accepted")
	}
	s = newTurn("synthetic")
	s.receive([]byte(`{"type":"response.text.delta","response_id":"unknown","delta":"early"}`))
	if !s.failed {
		t.Fatal("unsolicited response accepted")
	}
}

// The TLS gateway and remote Pion peer below are test doubles. They validate
// actual host DataChannel lifecycle, never real model accuracy or audio devices.
func fakeRTC(t *testing.T, terminal bool) (*rta.Proxy, *atomic.Int32) {
	t.Helper()
	var base string
	var peersMu sync.Mutex
	var peers []*webrtc.PeerConnection
	creates := new(atomic.Int32)
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/rtic/v1/realtime/sessions":
			creates.Add(1)
			var policy struct {
				EnableSearch *bool  `json:"enable_search"`
				Scenario     string `json:"scenario"`
			}
			if json.NewDecoder(r.Body).Decode(&policy) != nil || policy.EnableSearch == nil || *policy.EnableSearch || policy.Scenario != "shenglu_group_recommendation" {
				t.Error("recommendation create did not disable search")
			}
			fmt.Fprintf(w, `{"session_id":"synthetic_session","client_endpoint":{"url":%q,"access_token":"synthetic_token","expires_at_ms":%d}}`, base+"/v1/realtime/sessions/synthetic_session/rtc", time.Now().Add(time.Minute).UnixMilli())
		case "/v1/realtime/sessions/synthetic_session/rtc":
			var offer webrtc.SessionDescription
			if json.NewDecoder(r.Body).Decode(&offer) != nil {
				http.Error(w, "invalid", 400)
				return
			}
			pc, e := webrtc.NewPeerConnection(webrtc.Configuration{})
			if e != nil {
				t.Error("peer creation")
				http.Error(w, "failed", 500)
				return
			}
			peersMu.Lock()
			peers = append(peers, pc)
			peersMu.Unlock()
			pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
				go func() {
					for {
						if _, _, e := track.ReadRTP(); e != nil {
							return
						}
					}
				}()
			})
			pc.OnDataChannel(func(dc *webrtc.DataChannel) {
				dc.OnMessage(func(message webrtc.DataChannelMessage) {
					var e struct {
						Type string `json:"type"`
					}
					_ = json.Unmarshal(message.Data, &e)
					switch e.Type {
					case "session.update":
						_ = dc.SendText(`{"type":"session.updated"}`)
					case "response.create":
						_ = dc.SendText(`{"type":"response.created","response":{"id":"synthetic_response"}}`)
						_ = dc.SendText(`{"type":"response.text.done","response_id":"synthetic_response","text":"{\"suggestions\":[]}"}`)
						_ = dc.SendText(`{"type":"response.done","response":{"id":"synthetic_response","status":"completed"}}`)
					}
				})
			})
			if pc.SetRemoteDescription(offer) != nil {
				http.Error(w, "failed", 500)
				return
			}
			answer, e := pc.CreateAnswer(nil)
			if e != nil {
				http.Error(w, "failed", 500)
				return
			}
			gathered := webrtc.GatheringCompletePromise(pc)
			if pc.SetLocalDescription(answer) != nil {
				http.Error(w, "failed", 500)
				return
			}
			<-gathered
			_ = json.NewEncoder(w).Encode(pc.LocalDescription())
		case "/rtic/v1/realtime/sessions/synthetic_session/stop":
			w.WriteHeader(202)
		case "/rtic/v1/realtime/sessions/synthetic_session":
			state := "active"
			if terminal {
				state = "ended"
			}
			fmt.Fprintf(w, `{"session":{"session_id":"synthetic_session","state":%q}}`, state)
		default:
			http.NotFound(w, r)
		}
	}))
	base = srv.URL
	p, e := rta.New(rta.Config{RTICURL: base, RTCOrigin: base, Token: "test-token", AppID: "test-app", ModelProfile: "test-profile", RoleProfile: "test-role", VoiceProfile: "test-voice", Region: "test-region"}, srv.Client())
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() {
		p.Close()
		srv.Close()
		peersMu.Lock()
		defer peersMu.Unlock()
		for _, pc := range peers {
			_ = pc.Close()
		}
	})
	return p, creates
}

func TestRealHostRTCWithSyntheticGatewayAndCleanup(t *testing.T) {
	for _, terminal := range []bool{true, false} {
		t.Run(fmt.Sprint(terminal), func(t *testing.T) {
			p, creates := fakeRTC(t, terminal)
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			text, report, e := Text(ctx, p, "synthetic prompt")
			if !report.DataChannelOpen || !report.SessionUpdated || !report.ResponseDone || report.AudioEvents != 0 || report.MicrophonePaths != 0 || report.PlaybackPaths != 0 {
				t.Fatalf("unexpected report %+v", report)
			}
			if terminal {
				if e != nil || text != `{"suggestions":[]}` || !report.StopConfirmed {
					t.Fatalf("not completed: %+v %v", report, e)
				}
			} else {
				if e == nil || text != "" || report.StopConfirmed || report.Stage != "cleanup_unconfirmed" {
					t.Fatal("unconfirmed cleanup published result")
				}
				if _, _, e = Text(ctx, p, "must stay blocked"); e == nil || creates.Load() != 1 {
					t.Fatal("cleanup gate allowed new provider session")
				}
			}
		})
	}
}

func TestUnavailableAndCancelledNeverStartSession(t *testing.T) {
	if _, e := New(nil).Recommend(context.Background(), control.RecommendationInput{}); e == nil {
		t.Fatal("nil route accepted")
	}
	p, creates := fakeRTC(t, true)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, _, e := Text(ctx, p, "cancelled"); e == nil || creates.Load() != 0 {
		t.Fatal("cancelled request created session")
	}
	m := New(p)
	m.busy <- struct{}{}
	if _, e := m.Recommend(context.Background(), control.RecommendationInput{}); e == nil || creates.Load() != 0 {
		t.Fatal("concurrent request created session")
	}
	<-m.busy
	if _, e := m.Recommend(context.Background(), control.RecommendationInput{Need: strings.Repeat("x", 24577)}); e == nil || creates.Load() != 0 {
		t.Fatal("oversize prompt created session")
	}
}

func TestRecommendWithReportUsesProductionTurnAndCleanup(t *testing.T) {
	p, creates := fakeRTC(t, true)
	m := New(p)
	input := control.RecommendationInput{Need: "合成设备需求", Candidates: []control.RecommendationCandidate{{CandidateID: "c00", Name: "合成维修组", Purpose: "设备排障"}}}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	text, report, err := m.RecommendWithReport(ctx, input)
	if err != nil || text != `{"suggestions":[]}` || creates.Load() != 1 || report.Stage != "text_completed" ||
		!report.StopConfirmed || !report.ResponseDone || report.TextBytes != len(text) || report.AudioEvents != 0 || report.MicrophonePaths != 0 || report.PlaybackPaths != 0 {
		t.Fatalf("production observation failed: report=%+v err=%v", report, err)
	}
	if report.CreateMS < 0 || report.OfferMS < 0 || report.ResponseMS < 0 || report.CleanupMS < 0 ||
		report.TotalMS < report.CreateMS+report.OfferMS+report.ResponseMS+report.CleanupMS {
		t.Fatal("stage duration metadata is negative or exceeds the whole turn")
	}
	var absent *Model
	if text, report, err = absent.RecommendWithReport(ctx, input); err == nil || text != "" || report.Stage != "prepare" || report.StopConfirmed {
		t.Fatal("unavailable observation claimed a completed turn")
	}
}
