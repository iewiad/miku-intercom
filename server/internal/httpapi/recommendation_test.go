package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"testing"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

type apiRecommendationModel struct {
	calls int
	input control.RecommendationInput
}

func (m *apiRecommendationModel) Recommend(ctx context.Context, input control.RecommendationInput) (string, error) {
	m.calls++
	m.input = input
	return fmt.Sprintf(`{"suggestions":[{"candidate_id":%q,"reason":"职责与本次主动需求相符"}]}`, input.Candidates[len(input.Candidates)-1].CandidateID), nil
}
func TestHTTPRecommendationUnavailableAuthAndBoundary(t *testing.T) {
	f := fixture(t)
	out := response(t, request(f.h, "GET", "/api/v1/recommendations", "", f.tokenA, nil), 200)
	if string(out["available"]) != "false" {
		t.Fatal("unverified model advertised")
	}
	errorCode(t, request(f.h, "GET", "/api/v1/recommendations", "", "", nil), 401, "unauthorized")
	errorCode(t, request(f.h, "DELETE", "/api/v1/recommendations", "", f.tokenA, nil), 405, "method_not_allowed")
	errorCode(t, request(f.h, "POST", "/api/v1/recommendations", `{"request_id":"request_001","need":"需求","server_epoch":"epoch001"}`, f.tokenA, nil), 400, "invalid_request")
	errorCode(t, request(f.h, "POST", "/api/v1/recommendations", `{"request_id":"request_001","need":"需求","expected_version":0,"server_epoch":"epoch001","candidates":[]}`, f.tokenA, nil), 400, "invalid_json")
	errorCode(t, request(f.h, "POST", "/api/v1/recommendations", `{"request_id":"request_001","need":"需求","expected_version":0,"server_epoch":"epoch001"}`, f.tokenA, nil), 503, "recommendation_unavailable")
	errorCode(t, request(f.h, "PUT", "/api/v1/recommendations/confirm", `{"recommendation_id":"recommend001","operation_id":"operation001","group_id":"group001","server_epoch":"epoch001"}`, f.tokenA, nil), 400, "invalid_request")
	errorCode(t, request(f.h, "GET", "/api/v1/recommendations", "", f.tokenA, map[string]string{"Origin": "https://hostile.example"}), 403, "origin_forbidden")
}
func TestHTTPRecommendationRealControllerTransaction(t *testing.T) {
	model := &apiRecommendationModel{}
	service, err := control.New(nil, nil, control.Options{Recommender: model})
	if err != nil {
		t.Fatal(err)
	}
	device, token, err := service.CreateDevice("甲")
	if err != nil {
		t.Fatal(err)
	}
	_, _, _, err = service.CreateGroupWithPurpose(device.ID, "协作组", "设备排查")
	if err != nil {
		t.Fatal(err)
	}
	outsider, _, err := service.CreateDevice("乙")
	if err != nil {
		t.Fatal(err)
	}
	_, _, _, err = service.CreateGroupWithPurpose(outsider.ID, "未授权群", "secret purpose")
	if err != nil {
		t.Fatal(err)
	}
	handler := New(service)
	st, err := service.State(device.ID)
	if err != nil {
		t.Fatal(err)
	}
	req := control.RecommendationRequest{RequestID: "need_request001", Need: "设备没有声音", ExpectedVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch}
	body, _ := json.Marshal(req)
	out := response(t, request(handler, "POST", "/api/v1/recommendations", string(body), token, nil), 200)
	var recommendation control.Recommendation
	if err = json.Unmarshal(out["recommendation"], &recommendation); err != nil {
		t.Fatal(err)
	}
	if len(model.input.Candidates) != 1 || len(recommendation.Suggestions) != 1 || model.calls != 1 {
		t.Fatal("authorization or candidate mapping failed")
	}
	response(t, request(handler, "POST", "/api/v1/recommendations", string(body), token, nil), 200)
	if model.calls != 1 {
		t.Fatal("retried recommendation called model again")
	}
	confirm := control.RecommendationConfirmation{RecommendationID: recommendation.RecommendationID, OperationID: "confirm_operation001", GroupID: recommendation.Suggestions[0].GroupID, ExpectedVersion: recommendation.CurrentVersion, ServerEpoch: recommendation.ServerEpoch}
	body, _ = json.Marshal(confirm)
	result := response(t, request(handler, "PUT", "/api/v1/recommendations/confirm", string(body), token, nil), 200)
	var operation control.Operation
	if err = json.Unmarshal(result["operation"], &operation); err != nil {
		t.Fatal(err)
	}
	if operation.Status != "applied" || operation.RecommendationID != recommendation.RecommendationID {
		t.Fatalf("unexpected terminal=%+v", operation)
	}
	response(t, request(handler, "GET", "/api/v1/operations/"+operation.OperationID, "", token, nil), 200)
	response(t, request(handler, "PUT", "/api/v1/recommendations/confirm", string(body), token, nil), 200)
	response(t, request(handler, http.MethodGet, "/api/v1/recommendations", "", token, nil), 200)
}
