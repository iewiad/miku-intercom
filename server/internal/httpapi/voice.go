package httpapi

import (
	"encoding/json"
	"net/http"
	"strings"
)

func (s *Server) handleVoice(w http.ResponseWriter, r *http.Request, path, id string) error {
	if path == "/voice/resolve" {
		var req struct {
			Transcript string `json:"transcript"`
		}
		if e := decode(w, r, &req); e != nil {
			return e
		}
		group, e := s.service.ResolveVoice(id, req.Transcript)
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
