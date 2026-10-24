package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/httpapi"
	"github.com/iewiad/miku-intercom/server/internal/persist"
	"github.com/iewiad/miku-intercom/server/internal/recommendmodel"
	"github.com/iewiad/miku-intercom/server/internal/rta"
)

type controlReport struct {
	Stage                string `json:"stage"`
	ModelConfigured      bool   `json:"original_model_configured"`
	HTTPStatus           int    `json:"recommendation_http_status"`
	Suggestions          int    `json:"suggestions"`
	ExactFixtureTarget   bool   `json:"exact_fixture_target"`
	UnconfirmedUnchanged bool   `json:"unconfirmed_current_group_unchanged"`
	ExplicitConfirmation bool   `json:"explicit_synthetic_confirmation"`
	Applied              bool   `json:"confirmed_operation_applied"`
	ReplayStable         bool   `json:"replay_did_not_switch_again"`
	RestartReplayStable  bool   `json:"restart_replay_returned_saved_terminal"`
	RemoteStopConfirmed  bool   `json:"known_rta_session_stop_confirmed"`
	MicrophonePaths      int    `json:"microphone_paths"`
	PlaybackPaths        int    `json:"playback_paths"`
}

func call(client *http.Client, base, method, path, token string, input, output any) int {
	var body io.Reader
	if input != nil {
		b, e := json.Marshal(input)
		if e != nil {
			return 0
		}
		body = bytes.NewReader(b)
	}
	req, e := http.NewRequest(method, base+"/api/v1"+path, body)
	if e != nil {
		return 0
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+token)
	response, e := client.Do(req)
	if e != nil {
		return 0
	}
	defer response.Body.Close()
	data, e := io.ReadAll(io.LimitReader(response.Body, 65537))
	if e != nil || len(data) > 65536 {
		return 0
	}
	if output != nil && response.StatusCode >= 200 && response.StatusCode < 300 && json.Unmarshal(data, output) != nil {
		return 0
	}
	return response.StatusCode
}

func probeControl(proxy *rta.Proxy, statePath string, confirm bool) (report controlReport) {
	report.Stage = "prepare"
	store, e := persist.Open(statePath)
	if e != nil {
		report.Stage = "private_state_unavailable"
		return
	}
	defer store.Close()
	snapshot, e := store.Load()
	if e != nil || len(snapshot) != 0 {
		report.Stage = "state_must_be_new_and_empty"
		return
	}
	service, e := control.New(nil, store, control.Options{Recommender: recommendmodel.New(proxy)})
	if e != nil {
		return
	}
	report.ModelConfigured = service.RecommendationAvailable()
	device, token, e := service.CreateDevice("合成模型验收终端")
	if e != nil {
		return
	}
	first, _, before, e := service.CreateGroupWithPurpose(device.ID, "合成后勤组", "物资、交通和用餐协调")
	if e != nil {
		return
	}
	target, _, _, e := service.CreateGroupWithPurpose(device.ID, "合成维修组", "音响、灯光和设备故障排查维修")
	if e != nil {
		return
	}
	outsider, _, e := service.CreateDevice("合成未授权终端")
	if e != nil {
		return
	}
	if _, _, _, e = service.CreateGroupWithPurpose(outsider.ID, "合成未授权群", "未加入的群，不得成为候选"); e != nil {
		return
	}
	server := httptest.NewServer(httpapi.New(service))
	defer server.Close()
	client := &http.Client{Timeout: 35 * time.Second}
	var recommended struct {
		Recommendation control.Recommendation `json:"recommendation"`
	}
	req := control.RecommendationRequest{RequestID: "probe_need_0001", Need: "音响突然没有声音，需要能排查设备故障的小组", ExpectedVersion: before.CurrentVersion, ServerEpoch: before.ServerEpoch}
	report.HTTPStatus = call(client, server.URL, http.MethodPost, "/recommendations", token, req, &recommended)
	report.RemoteStopConfirmed = proxy.Status("recommendation_model").CommandID == ""
	after, e := service.State(device.ID)
	if e != nil {
		return
	}
	report.UnconfirmedUnchanged = after.CurrentGroupID == first.ID && after.CurrentVersion == before.CurrentVersion
	report.Suggestions = len(recommended.Recommendation.Suggestions)
	report.ExactFixtureTarget = report.HTTPStatus == 200 && report.Suggestions == 1 && recommended.Recommendation.Suggestions[0].GroupID == target.ID
	if !report.ExactFixtureTarget {
		report.Stage = "recommendation_not_verified"
		return
	}
	report.Stage = "suggestion_verified"
	if !confirm {
		return
	}
	report.ExplicitConfirmation = true
	confirmation := control.RecommendationConfirmation{RecommendationID: recommended.Recommendation.RecommendationID, OperationID: "probe_confirm_0001", GroupID: target.ID, ExpectedVersion: before.CurrentVersion, ServerEpoch: before.ServerEpoch}
	var confirmed struct {
		Operation control.Operation `json:"operation"`
		State     control.State     `json:"state"`
	}
	if call(client, server.URL, http.MethodPut, "/recommendations/confirm", token, confirmation, &confirmed) != 200 {
		return
	}
	report.Applied = confirmed.Operation.Status == "applied" && confirmed.Operation.RecommendationID == confirmation.RecommendationID && confirmed.State.CurrentGroupID == target.ID && confirmed.State.CurrentVersion == before.CurrentVersion+1
	var replayed struct {
		Operation control.Operation `json:"operation"`
		State     control.State     `json:"state"`
	}
	report.ReplayStable = call(client, server.URL, http.MethodPut, "/recommendations/confirm", token, confirmation, &replayed) == 200 && replayed.Operation == confirmed.Operation && replayed.State.CurrentVersion == confirmed.State.CurrentVersion
	server.Close()
	snapshot, e = store.Load()
	if e != nil {
		return
	}
	if store.Close() != nil {
		return
	}
	store, e = persist.Open(statePath)
	if e != nil {
		return
	}
	defer store.Close()
	restarted, e := control.New(snapshot, store, control.Options{})
	if e != nil {
		return
	}
	recovered := httptest.NewServer(httpapi.New(restarted))
	defer recovered.Close()
	report.RestartReplayStable = call(client, recovered.URL, http.MethodPut, "/recommendations/confirm", token, confirmation, &replayed) == 200 && replayed.Operation == confirmed.Operation && replayed.State.CurrentVersion == confirmed.State.CurrentVersion && replayed.State.ServerEpoch != before.ServerEpoch
	if report.Applied && report.ReplayStable && report.RestartReplayStable {
		report.Stage = "confirmed_and_replay_verified"
	}
	return
}
