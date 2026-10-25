package httpapi

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

// encoding/json replaces malformed UTF-8 and unpaired UTF-16 escapes. Reject
// both locally before a voice proposal can reach the model with changed text.
type voiceProposalTranscript string

func (value *voiceProposalTranscript) UnmarshalJSON(data []byte) error {
	if !utf8.Valid(data) {
		return errors.New("valid Unicode transcript required")
	}
	var text string
	if err := json.Unmarshal(data, &text); err != nil {
		return err
	}
	for i := 0; i < len(data); i++ {
		if data[i] != '\\' {
			continue
		}
		i++
		if i >= len(data) || data[i] != 'u' {
			continue
		}
		code, _ := strconv.ParseUint(string(data[i+1:i+5]), 16, 16)
		i += 4
		if code >= 0xD800 && code <= 0xDBFF {
			if i+6 >= len(data) || data[i+1] != '\\' || data[i+2] != 'u' {
				return errors.New("valid Unicode transcript required")
			}
			low, err := strconv.ParseUint(string(data[i+3:i+7]), 16, 16)
			if err != nil || low < 0xDC00 || low > 0xDFFF {
				return errors.New("valid Unicode transcript required")
			}
			i += 6
		} else if code >= 0xDC00 && code <= 0xDFFF {
			return errors.New("valid Unicode transcript required")
		}
	}
	*value = voiceProposalTranscript(text)
	return nil
}

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
	if path == "/voice/proposals" {
		var req struct {
			RequestID       string                  `json:"request_id"`
			Transcript      voiceProposalTranscript `json:"transcript"`
			ExpectedVersion *uint64                 `json:"expected_version"`
			ServerEpoch     string                  `json:"server_epoch"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		if req.ExpectedVersion == nil {
			return invalid("invalid_request", "expected_version is required", 400)
		}
		recommendation, err := s.service.ProposeVoiceTarget(r.Context(), id, control.VoiceProposalRequest{
			RequestID: req.RequestID, Transcript: string(req.Transcript), ExpectedVersion: *req.ExpectedVersion, ServerEpoch: req.ServerEpoch,
		})
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"recommendation": recommendation})
		return nil
	}
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
