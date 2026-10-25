package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/httpapi"
	"github.com/iewiad/miku-intercom/server/internal/recommendmodel"
)

const runLimit = 40 * time.Minute
const minimumStartWindow = 35 * time.Second // model 20s, cleanup 7s, final Close 7s, setup margin.
const startInterval = 5 * time.Second

type reportingModel interface {
	RecommendWithReport(context.Context, control.RecommendationInput) (string, recommendmodel.Report, error)
}
type suggestion struct {
	Key    string `json:"key"`
	Reason string `json:"reason"` // API explanation after the declared reason policy; not provider prose.
}
type caseReport struct {
	CaseID             string                `json:"case_id"`
	Category           string                `json:"category"`
	Protocol           string                `json:"protocol_status"`
	Semantic           string                `json:"semantic_status"`
	Transport          string                `json:"transport_status"`
	Cleanup            string                `json:"cleanup_status"`
	ReasonCode         string                `json:"reason_code"`
	HTTPStatus         int                   `json:"http_status"`
	ModelCalls         int                   `json:"model_calls"`
	CandidateOrder     []string              `json:"candidate_order"`
	Suggestions        []suggestion          `json:"suggestions"`
	AuthorizationOK    bool                  `json:"candidate_authorization_ok"`
	Unchanged          bool                  `json:"unconfirmed_group_unchanged"`
	NoOperations       bool                  `json:"no_control_operations"`
	ElapsedMS          int64                 `json:"elapsed_ms"`
	Observation        recommendmodel.Report `json:"report"`
	UnknownReportTypes int                   `json:"unknown_report_event_types"`
}
type metrics struct {
	Cases              int `json:"cases"`
	ProtocolValid      int `json:"protocol_valid"`
	ProtocolErrors     int `json:"protocol_errors"`
	TransportErrors    int `json:"transport_errors"`
	TransportTimeouts  int `json:"transport_timeouts"`
	TransportCancelled int `json:"transport_cancelled"`
	CleanupUnconfirmed int `json:"cleanup_unconfirmed"`
	SemanticScored     int `json:"semantic_scored"`
	SemanticPassed     int `json:"semantic_passed"`
	SemanticMissed     int `json:"semantic_missed"`
}
type evaluation struct {
	Version           int          `json:"version"`
	ReasonPolicy      string       `json:"reason_policy"`
	DatasetVersion    int          `json:"dataset_version"`
	Selected          int          `json:"cases_selected"`
	StopReason        string       `json:"stop_reason"`
	PendingAfterClose bool         `json:"cleanup_pending_after_close"`
	ElapsedMS         int64        `json:"elapsed_ms"`
	Metrics           metrics      `json:"metrics"`
	Cases             []caseReport `json:"cases"`
}

var reportStages = map[string]bool{"prepare": true, "cleanup_pending": true, "create_failed": true, "timeout": true, "offer_failed": true, "answer_failed": true, "response": true, "text_completed": true, "cleanup_unconfirmed": true}
var reportEvents = map[string]bool{"session.created": true, "session.updated": true, "response.created": true, "response.text.delta": true, "response.output_text.delta": true, "response.text.done": true, "response.output_text.done": true, "response.done": true, "response.audio.delta": true, "response.output_audio.delta": true, "error": true, "rtid.error": true, "session.closed": true}

func bounded(value, maximum int) int {
	if value < 0 {
		return 0
	}
	if value > maximum {
		return maximum
	}
	return value
}
func safeReport(input recommendmodel.Report) (recommendmodel.Report, int) {
	result := input
	if !reportStages[result.Stage] {
		result.Stage = "unknown"
	}
	result.TextBytes = bounded(result.TextBytes, control.MaxRecommendationOutputBytes+1)
	result.AudioEvents = bounded(result.AudioEvents, 257)
	result.MicrophonePaths = bounded(result.MicrophonePaths, 257)
	result.PlaybackPaths = bounded(result.PlaybackPaths, 257)
	for _, value := range []*int64{&result.CreateMS, &result.OfferMS, &result.ResponseMS, &result.CleanupMS, &result.TotalMS} {
		*value = int64(bounded(int(*value), int(runLimit.Milliseconds())))
	}
	result.Events = nil
	unknown := 0
	for _, event := range input.Events {
		if !reportEvents[event] {
			unknown++
		} else if len(result.Events) < 30 {
			result.Events = append(result.Events, event)
		}
	}
	return result, unknown
}

type observedModel struct {
	inner         reportingModel
	groups        []group
	calls         int
	invoked       bool
	order         []string
	authorized    bool
	timeout       bool
	cancelled     bool
	failed        bool
	report        recommendmodel.Report
	unknownEvents int
}

func (m *observedModel) Recommend(ctx context.Context, input control.RecommendationInput) (string, error) {
	m.invoked = true
	joined := map[string]group{}
	for _, g := range m.groups {
		if g.Joined {
			joined[g.Name] = g
		}
	}
	m.authorized = len(joined) == len(input.Candidates)
	seen := map[string]bool{}
	for index, candidate := range input.Candidates {
		g, exists := joined[candidate.Name]
		m.authorized = m.authorized && exists && candidate.Purpose == g.Purpose && !seen[candidate.Name] && candidate.CandidateID == fmt.Sprintf("c%02x", index)
		seen[candidate.Name] = true
		if exists {
			m.order = append(m.order, g.Key)
		}
	}
	if !m.authorized {
		m.failed = true
		return "", errors.New("candidate_authorization_failed")
	}
	m.calls++
	text, report, err := m.inner.RecommendWithReport(ctx, input)
	m.timeout = errors.Is(ctx.Err(), context.DeadlineExceeded) || errors.Is(err, context.DeadlineExceeded) || report.Stage == "timeout"
	m.cancelled = errors.Is(ctx.Err(), context.Canceled) || errors.Is(err, context.Canceled)
	m.failed = err != nil
	m.report, m.unknownEvents = safeReport(report)
	return text, err
}

type memorySnapshot struct{ data []byte }

func (m *memorySnapshot) Save(data []byte) error { m.data = append(m.data[:0], data...); return nil }
func (m *memorySnapshot) noOperations() bool {
	var state struct {
		Devices map[string]struct {
			Operations map[string]json.RawMessage `json:"operations"`
		} `json:"devices"`
	}
	if json.Unmarshal(m.data, &state) != nil {
		return false
	}
	for _, device := range state.Devices {
		if len(device.Operations) != 0 {
			return false
		}
	}
	return true
}

func semanticPass(c evalCase, suggestions []suggestion) bool {
	if c.Expected == "abstain" {
		return len(suggestions) == 0
	}
	if len(suggestions) == 0 {
		return false
	}
	acceptable := map[string]bool{}
	for _, key := range c.AcceptableKeys {
		acceptable[key] = true
	}
	for _, item := range suggestions {
		if !acceptable[item.Key] {
			return false
		}
		delete(acceptable, item.Key)
	}
	return !c.RequireAll || len(acceptable) == 0
}

func runCase(ctx context.Context, c evalCase, p profile, model reportingModel, now func() time.Time, cleanupPending func() bool) (result caseReport) {
	started := now()
	result = caseReport{CaseID: c.ID, Category: c.Category, Protocol: "not_evaluated", Semantic: "not_scored", Transport: "not_called", Cleanup: "not_started", ReasonCode: "synthetic_setup_failed", Suggestions: []suggestion{}, CandidateOrder: []string{}}
	defer func() { result.ElapsedMS = now().Sub(started).Milliseconds() }()
	trace := &observedModel{inner: model, groups: p.Groups}
	store := &memorySnapshot{}
	service, err := control.New(nil, store, control.Options{Recommender: trace, Now: now})
	if err != nil {
		return
	}
	device, token, err := service.CreateDevice("合成需求评测终端")
	if err != nil {
		return
	}
	owner, _, err := service.CreateDevice("合成目录所有者")
	if err != nil {
		return
	}
	keys := map[string]group{}
	for _, g := range p.Groups {
		created, code, _, createErr := service.CreateGroupWithPurpose(owner.ID, g.Name, g.Purpose)
		if createErr != nil {
			return
		}
		if g.Joined {
			if _, err = service.Join(device.ID, code); err != nil {
				return
			}
			keys[created.ID] = g
		}
	}
	before, err := service.State(device.ID)
	if err != nil {
		return
	}
	requestBytes, _ := json.Marshal(control.RecommendationRequest{RequestID: "evaluation_request_0001", Need: c.Need, ExpectedVersion: before.CurrentVersion, ServerEpoch: before.ServerEpoch})
	request := httptest.NewRequest(http.MethodPost, "http://127.0.0.1:18792/api/v1/recommendations", bytes.NewReader(requestBytes)).WithContext(ctx)
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+token)
	response := httptest.NewRecorder()
	httpapi.New(service).ServeHTTP(response, request)
	result.HTTPStatus, result.ModelCalls = response.Code, trace.calls
	result.CandidateOrder = append(result.CandidateOrder, trace.order...)
	result.Observation, result.UnknownReportTypes = trace.report, trace.unknownEvents
	result.AuthorizationOK = !trace.invoked || trace.authorized
	after, stateErr := service.State(device.ID)
	result.Unchanged = stateErr == nil && after.CurrentGroupID == before.CurrentGroupID && after.CurrentVersion == before.CurrentVersion
	result.NoOperations = store.noOperations()
	result.Transport = "ok"
	if trace.calls == 0 {
		result.Transport = "not_called"
	} else if trace.timeout {
		result.Transport = "timeout"
	} else if trace.cancelled {
		result.Transport = "cancelled"
	} else if trace.failed && trace.report.Stage != "response" && trace.report.Stage != "text_completed" && trace.report.Stage != "cleanup_unconfirmed" {
		result.Transport = "error"
	}
	if trace.report.StopConfirmed {
		result.Cleanup = "confirmed"
	} else if cleanupPending() || trace.report.Stage == "cleanup_pending" || trace.report.Stage == "cleanup_unconfirmed" || trace.calls > 0 && trace.report.Stage != "prepare" && trace.report.Stage != "create_failed" {
		result.Cleanup = "unconfirmed"
	}
	if cleanupPending() {
		result.Cleanup = "unconfirmed"
	}
	if !result.Unchanged || !result.NoOperations || !result.AuthorizationOK {
		result.Protocol, result.ReasonCode = "invalid", "control_invariant_failed"
		return
	}
	if result.Cleanup == "unconfirmed" {
		result.ReasonCode = "cleanup_unconfirmed"
		return
	}
	if result.Transport != "ok" {
		result.ReasonCode = "transport_error"
		if result.Transport == "not_called" {
			result.ReasonCode = "control_rejected_before_model"
		}
		if result.Transport == "timeout" {
			result.ReasonCode = "transport_timeout"
		} else if result.Transport == "cancelled" {
			result.ReasonCode = "run_cancelled"
		}
		return
	}
	if result.Observation.AudioEvents != 0 || result.Observation.MicrophonePaths != 0 || result.Observation.PlaybackPaths != 0 {
		result.Protocol, result.ReasonCode = "invalid", "unexpected_audio_or_device_path"
		return
	}
	var body struct {
		Recommendation control.Recommendation `json:"recommendation"`
	}
	if response.Code != http.StatusOK || json.Unmarshal(response.Body.Bytes(), &body) != nil {
		result.Protocol, result.ReasonCode = "invalid", "control_or_output_rejected"
		return
	}
	for _, item := range body.Recommendation.Suggestions {
		g, ok := keys[item.GroupID]
		if !ok || item.Name != g.Name || item.Purpose != g.Purpose || !validText(item.Reason, 160, false) {
			result.Protocol, result.ReasonCode, result.AuthorizationOK = "invalid", "unauthorized_or_invalid_suggestion", false
			result.Suggestions = nil
			return
		}
		result.Suggestions = append(result.Suggestions, suggestion{Key: g.Key, Reason: item.Reason})
	}
	result.Protocol, result.ReasonCode = "valid", "semantic_miss"
	if result.Cleanup == "confirmed" {
		result.Semantic = "miss"
		if semanticPass(c, result.Suggestions) {
			result.Semantic, result.ReasonCode = "pass", "semantic_match"
		}
	}
	return
}

type runOptions struct {
	now            func() time.Time
	wait           func(context.Context, time.Duration) error
	cleanupPending func() bool
	checkpoint     func(evaluation) error
}

func contextStopReason(err error) string {
	if errors.Is(err, context.Canceled) {
		return "run_cancelled"
	}
	return "run_deadline"
}

func runEvaluation(ctx context.Context, data dataset, cases []evalCase, model reportingModel, options runOptions) evaluation {
	started := options.now()
	deadline := started.Add(runLimit)
	result := evaluation{Version: 1, ReasonPolicy: control.RecommendationReasonPolicy, DatasetVersion: data.Version, Selected: len(cases), StopReason: "completed", Cases: []caseReport{}}
	var lastStart time.Time
	for _, c := range cases {
		if ctx.Err() != nil {
			result.StopReason = contextStopReason(ctx.Err())
			break
		}
		if options.now().Add(minimumStartWindow).After(deadline) {
			result.StopReason = "run_deadline"
			break
		}
		if options.cleanupPending() {
			result.StopReason = "cleanup_unconfirmed"
			break
		}
		if !lastStart.IsZero() {
			if delay := lastStart.Add(startInterval).Sub(options.now()); delay > 0 {
				if err := options.wait(ctx, delay); err != nil {
					result.StopReason = contextStopReason(err)
					break
				}
			}
		}
		if ctx.Err() != nil {
			result.StopReason = contextStopReason(ctx.Err())
			break
		}
		if options.now().Add(minimumStartWindow).After(deadline) {
			result.StopReason = "run_deadline"
			break
		}
		lastStart = options.now()
		current := runCase(ctx, c, data.Profiles[c.Profile], model, options.now, options.cleanupPending)
		result.Cases = append(result.Cases, current)
		result.Metrics.Cases++
		switch current.Protocol {
		case "valid":
			result.Metrics.ProtocolValid++
		case "invalid":
			result.Metrics.ProtocolErrors++
		}
		switch current.Transport {
		case "timeout":
			result.Metrics.TransportTimeouts++
		case "error":
			result.Metrics.TransportErrors++
		case "cancelled":
			result.Metrics.TransportCancelled++
		}
		if current.Cleanup == "unconfirmed" {
			result.Metrics.CleanupUnconfirmed++
			result.StopReason = "cleanup_unconfirmed"
		} else if !current.AuthorizationOK || !current.Unchanged || !current.NoOperations {
			result.StopReason = "control_invariant_failed"
		} else if ctx.Err() != nil {
			result.StopReason = contextStopReason(ctx.Err())
		}
		if current.Semantic != "not_scored" {
			result.Metrics.SemanticScored++
			if current.Semantic == "pass" {
				result.Metrics.SemanticPassed++
			} else {
				result.Metrics.SemanticMissed++
			}
		}
		result.ElapsedMS = options.now().Sub(started).Milliseconds()
		if options.checkpoint(result) != nil {
			result.StopReason = "output_unavailable"
			break
		}
		if result.StopReason != "completed" {
			break
		}
	}
	result.ElapsedMS = options.now().Sub(started).Milliseconds()
	return result
}
