package httpapi

import (
	"github.com/iewiad/miku-intercom/server/internal/control"
	"net/http"
)

func (s *Server) handleRecommendations(w http.ResponseWriter, r *http.Request, path, id string) error {
	if path == "/recommendations" && r.Method == http.MethodGet {
		available := s.service.RecommendationAvailable()
		reason := ""
		if !available {
			reason = "原 RTA 文本推荐能力尚未验证或未配置"
		}
		send(w, 200, map[string]any{"available": available, "reason": reason})
		return nil
	}
	if path == "/recommendations" {
		var req struct {
			RequestID       string  `json:"request_id"`
			Need            string  `json:"need"`
			ExpectedVersion *uint64 `json:"expected_version"`
			ServerEpoch     string  `json:"server_epoch"`
		}
		if err := decode(w, r, &req); err != nil {
			return err
		}
		if req.ExpectedVersion == nil {
			return invalid("invalid_request", "expected_version is required", 400)
		}
		recommendation, err := s.service.Recommend(r.Context(), id, control.RecommendationRequest{RequestID: req.RequestID, Need: req.Need, ExpectedVersion: *req.ExpectedVersion, ServerEpoch: req.ServerEpoch})
		if err != nil {
			return err
		}
		send(w, 200, map[string]any{"recommendation": recommendation})
		return nil
	}
	var req struct {
		RecommendationID string  `json:"recommendation_id"`
		OperationID      string  `json:"operation_id"`
		GroupID          string  `json:"group_id"`
		ExpectedVersion  *uint64 `json:"expected_version"`
		ServerEpoch      string  `json:"server_epoch"`
	}
	if err := decode(w, r, &req); err != nil {
		return err
	}
	if req.ExpectedVersion == nil {
		return invalid("invalid_request", "expected_version is required", 400)
	}
	operation, state, err := s.service.ConfirmRecommendation(id, control.RecommendationConfirmation{RecommendationID: req.RecommendationID, OperationID: req.OperationID, GroupID: req.GroupID, ExpectedVersion: *req.ExpectedVersion, ServerEpoch: req.ServerEpoch})
	if err != nil {
		return err
	}
	send(w, 200, map[string]any{"operation": operation, "state": s.stateView(state)})
	return nil
}
