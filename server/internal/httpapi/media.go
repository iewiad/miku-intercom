package httpapi

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/media"
	"github.com/pion/webrtc/v4"
)

func (s *Server) handleMedia(w http.ResponseWriter, r *http.Request, path, id string) error {
	switch path {
	case "/media":
		status, err := s.media.Status(id)
		if err != nil {
			return err
		}
		send(w, 200, status)
	case "/media/peers":
		var req struct {
			PeerID         string          `json:"peer_id"`
			ReplacePeerID  *string         `json:"replace_peer_id"`
			GroupID        string          `json:"group_id"`
			CurrentVersion *uint64         `json:"current_version"`
			ServerEpoch    string          `json:"server_epoch"`
			ValidUntil     json.RawMessage `json:"valid_until"`
			Offer          struct {
				Type string `json:"type"`
				SDP  string `json:"sdp"`
			} `json:"offer"`
		}
		if err := decodeLimit(w, r, &req, 64*1024); err != nil {
			return err
		}
		if req.CurrentVersion == nil || req.ReplacePeerID == nil {
			return invalid("invalid_request", "current_version and replace_peer_id required", 400)
		}
		var deadline time.Time
		if len(req.ValidUntil) == 0 || json.Unmarshal(req.ValidUntil, &deadline) != nil {
			return invalid("invalid_deadline", "valid_until must be RFC3339", 400)
		}
		if req.Offer.Type != "offer" {
			return invalid("invalid_offer", "audio offer required", 400)
		}
		out, err := s.media.Create(r.Context(), id, media.CreateRequest{PeerID: req.PeerID, ReplacePeerID: *req.ReplacePeerID, GroupID: req.GroupID, CurrentVersion: *req.CurrentVersion, ServerEpoch: req.ServerEpoch, ValidUntil: deadline, Offer: webrtc.SessionDescription{Type: webrtc.SDPTypeOffer, SDP: req.Offer.SDP}})
		if err != nil {
			return err
		}
		send(w, 201, out)
	default:
		if err := s.media.Delete(id, strings.TrimPrefix(path, "/media/peers/")); err != nil {
			return err
		}
		send(w, 200, map[string]bool{"closed": true})
	}
	return nil
}
