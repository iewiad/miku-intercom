package httpapi

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"strings"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

// logVoiceResolution has a fixed schema: never add command text, request data,
// returned groups, or raw errors. A server-local hook keeps tests independent of
// the process-wide logger.
func (s *Server) logVoiceResolution(d control.VoiceResolutionDiagnostics, started time.Time) {
	logf := s.voiceLogf
	if logf == nil {
		logf = log.Printf
	}
	logf("voice_resolve input_evaluated=%t utf8_valid=%t input_runes=%d input_bytes=%d accepted_prefix=%s candidate_count=%d exact_match_count=%d outcome=%s error_code=%s elapsed_ms=%d",
		d.InputEvaluated, d.UTF8Valid, d.InputRunes, d.InputBytes, d.AcceptedPrefix, d.CandidateCount, d.ExactMatchCount, d.Outcome, d.ErrorCode, time.Since(started).Milliseconds())
}

func (s *Server) handleVoice(w http.ResponseWriter, r *http.Request, path, id string) error {
	if path == "/voice/resolve" {
		started := time.Now()
		diagnostic := control.VoiceResolutionDiagnostics{AcceptedPrefix: "none", Outcome: "invalid_input", ErrorCode: "invalid_input"}
		defer func() { s.logVoiceResolution(diagnostic, started) }()
		var req struct {
			Transcript string `json:"transcript"`
		}
		if e := decode(w, r, &req); e != nil {
			var controlError *control.Error
			if errors.As(e, &controlError) {
				switch controlError.Code {
				case "invalid_json", "body_too_large", "unsupported_media_type":
					diagnostic.ErrorCode = controlError.Code
				}
			}
			return e
		}
		group, diagnostic, e := s.service.ResolveVoiceWithDiagnostics(id, req.Transcript)
		if e != nil {
			return e
		}
		send(w, 200, map[string]string{"group_id": group.ID, "name": group.Name})
		return nil
	}
	if path == "/voice" {
		if s.voice == nil {
			send(w, 200, map[string]any{"available": false, "phase": "unavailable", "command_id": ""})
			return nil
		}
		send(w, 200, s.voice.Status(id))
		return nil
	}
	if s.voice == nil {
		return invalid("voice_unavailable", "voice commands require explicit server configuration", 503)
	}
	if path == "/voice/sessions" {
		var req struct {
			CommandID string `json:"command_id"`
		}
		if e := decode(w, r, &req); e != nil {
			return e
		}
		if e := s.voice.Start(id, req.CommandID); e != nil {
			return e
		}
		send(w, 201, map[string]string{"command_id": req.CommandID})
		return nil
	}
	parts := strings.Split(strings.TrimPrefix(path, "/voice/sessions/"), "/")
	if len(parts) != 2 {
		return invalid("not_found", "resource not found", 404)
	}
	if parts[1] == "stop" {
		// A body is deliberately unnecessary; clients can retry cleanup after navigation.
		if e := s.voice.Stop(id, parts[0]); e != nil {
			return e
		}
		send(w, 200, map[string]any{"stopped": true, "command_id": parts[0]})
		return nil
	}
	var req struct {
		Type string `json:"type"`
		SDP  string `json:"sdp"`
	}
	if e := decodeLimit(w, r, &req, 64*1024); e != nil {
		return e
	}
	if req.Type != "offer" || req.SDP == "" || len(req.SDP) > 60000 {
		return invalid("invalid_request", "audio RTC offer required", 400)
	}
	offer, _ := json.Marshal(req)
	answer, e := s.voice.Offer(r.Context(), id, parts[0], offer)
	if e != nil {
		return e
	}
	send(w, 200, answer)
	return nil
}
