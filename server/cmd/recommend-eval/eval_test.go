package main

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
	"github.com/iewiad/miku-intercom/server/internal/recommendmodel"
)

type fakeModel struct {
	calls int
	input control.RecommendationInput
	turn  func(context.Context, control.RecommendationInput) (string, recommendmodel.Report, error)
}

func (m *fakeModel) RecommendWithReport(ctx context.Context, input control.RecommendationInput) (string, recommendmodel.Report, error) {
	m.calls++
	m.input = input
	if m.turn != nil {
		return m.turn(ctx, input)
	}
	return modelText(input, "维修组"), completedReport(), nil
}
func completedReport() recommendmodel.Report {
	return recommendmodel.Report{Stage: "text_completed", DataChannelOpen: true, SessionUpdated: true, ResponseDone: true, StopConfirmed: true,
		Events: []string{"session.updated", "response.created", "response.done"}, CreateMS: 1, OfferMS: 2, ResponseMS: 3, CleanupMS: 4, TotalMS: 10}
}
func modelText(input control.RecommendationInput, names ...string) string {
	items := []map[string]string{}
	for _, name := range names {
		for _, candidate := range input.Candidates {
			if candidate.Name == name {
				items = append(items, map[string]string{"candidate_id": candidate.CandidateID, "reason": "职责涵盖本次合成需求"})
			}
		}
	}
	data, _ := json.Marshal(map[string]any{"suggestions": items})
	return string(data)
}
func fixtureDataset() dataset {
	return dataset{Version: 1, Profiles: map[string]profile{"synthetic_profile": {Groups: []group{
		{Key: "repair", Name: "维修组", Purpose: "设备排障", Joined: true},
		{Key: "logistics", Name: "后勤组", Purpose: "物资协调", Joined: true},
		{Key: "foreign", Name: "未授权急救组", Purpose: "不能发给模型", Joined: false},
	}}}, Cases: []evalCase{{ID: "C01", Category: "clear_role", Profile: "synthetic_profile", Need: "设备需要维修", AcceptableKeys: []string{"repair"}, Expected: "recommend", Notes: "PRIVATE_LABEL_MUST_NOT_REACH_MODEL"}}}
}
func runFixture(c evalCase, model reportingModel) caseReport {
	data := fixtureDataset()
	return runCase(context.Background(), c, data.Profiles[c.Profile], model, time.Now, func() bool { return false })
}

func TestRealHTTPControlPathMapsAuthorizedCandidatesWithoutConfirmation(t *testing.T) {
	data, model := fixtureDataset(), &fakeModel{}
	result := runFixture(data.Cases[0], model)
	if result.Semantic != "pass" || result.Protocol != "valid" || result.Transport != "ok" || result.Cleanup != "confirmed" || !result.Unchanged || !result.NoOperations || !result.AuthorizationOK || result.HTTPStatus != 200 || model.calls != 1 {
		t.Fatalf("strict control path failed: %+v", result)
	}
	if len(result.Suggestions) != 1 || result.Suggestions[0].Key != "repair" || len(result.CandidateOrder) != 2 {
		t.Fatal("authoritative group mapping missing")
	}
	for index, candidate := range model.input.Candidates {
		want := map[string]string{"维修组": "repair", "后勤组": "logistics"}[candidate.Name]
		if result.CandidateOrder[index] != want {
			t.Fatal("recorded synthetic key order differs from actual model candidate order")
		}
	}
	encoded, _ := json.Marshal(model.input)
	for _, private := range []string{"PRIVATE_LABEL", "clear_role", "synthetic_profile", "acceptable_keys", "require_all", "未授权", "不能发给模型", "joined", "repair", "logistics", "C01"} {
		if strings.Contains(string(encoded), private) {
			t.Fatalf("local labels or unauthorized directory entered model input: %s", private)
		}
	}
	output, _ := json.Marshal(result)
	for _, prefix := range []string{"grp_", "dev_", "rec_", "epoch_", "evaluation_request_"} {
		if strings.Contains(string(output), prefix) {
			t.Fatal("report contains generated persistent identities")
		}
	}
}

func TestScoringSeparatesValidSemanticMissAndAbstention(t *testing.T) {
	data := fixtureDataset()
	for _, tc := range []struct {
		name, expected, want string
		accepted, names      []string
		all                  bool
	}{
		{"correct", "recommend", "pass", []string{"repair"}, []string{"维修组"}, false},
		{"wrong role", "recommend", "miss", []string{"repair"}, []string{"后勤组"}, false},
		{"missed recommendation", "recommend", "miss", []string{"repair"}, nil, false},
		{"correct abstention", "abstain", "pass", nil, nil, false},
		{"false recommendation", "abstain", "miss", nil, []string{"维修组"}, false},
		{"any acceptable subset", "recommend", "pass", []string{"repair", "logistics"}, []string{"维修组"}, false},
		{"require all missed one", "recommend", "miss", []string{"repair", "logistics"}, []string{"维修组"}, true},
		{"require all met", "recommend", "pass", []string{"repair", "logistics"}, []string{"维修组", "后勤组"}, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			c := data.Cases[0]
			c.Expected, c.AcceptableKeys, c.RequireAll = tc.expected, tc.accepted, tc.all
			model := &fakeModel{turn: func(_ context.Context, input control.RecommendationInput) (string, recommendmodel.Report, error) {
				return modelText(input, tc.names...), completedReport(), nil
			}}
			result := runFixture(c, model)
			if result.Semantic != tc.want || result.Protocol != "valid" || !result.Unchanged || !result.NoOperations {
				t.Fatalf("wrong semantic score: %+v", result)
			}
		})
	}
}

func TestProtocolTransportAndCleanupAreNotSemanticAbstention(t *testing.T) {
	c := fixtureDataset().Cases[0]
	c.Expected, c.AcceptableKeys = "abstain", nil
	for _, tc := range []struct {
		name, raw, stage, protocol, transport, cleanup string
		err                                            error
	}{
		{"invalid JSON", "not JSON", "text_completed", "invalid", "ok", "confirmed", nil},
		{"foreign candidate", `{"suggestions":[{"candidate_id":"c99","reason":"合成理由"}]}`, "text_completed", "invalid", "ok", "confirmed", nil},
		{"timeout", "", "timeout", "not_evaluated", "timeout", "confirmed", context.DeadlineExceeded},
		{"transport failure", "", "offer_failed", "not_evaluated", "error", "confirmed", errors.New("PRIVATE_PROVIDER_ERROR")},
		{"cleanup failure", "", "cleanup_unconfirmed", "not_evaluated", "ok", "unconfirmed", errors.New("PRIVATE_PROVIDER_ERROR")},
	} {
		t.Run(tc.name, func(t *testing.T) {
			model := &fakeModel{turn: func(context.Context, control.RecommendationInput) (string, recommendmodel.Report, error) {
				report := completedReport()
				report.Stage, report.StopConfirmed = tc.stage, tc.cleanup == "confirmed"
				return tc.raw, report, tc.err
			}}
			result := runFixture(c, model)
			if result.Semantic != "not_scored" || result.Protocol != tc.protocol || result.Transport != tc.transport || result.Cleanup != tc.cleanup || len(result.Suggestions) != 0 || !result.Unchanged || !result.NoOperations {
				t.Fatalf("failure conflated with abstention: %+v", result)
			}
			data, _ := json.Marshal(result)
			if strings.Contains(string(data), "PRIVATE_PROVIDER_ERROR") {
				t.Fatal("raw provider error leaked")
			}
		})
	}
}

func TestModelObserverNeverForwardsUnauthorizedOrMismatchedCandidates(t *testing.T) {
	for _, candidate := range []control.RecommendationCandidate{
		{CandidateID: "c00", Name: "未授权急救组", Purpose: "不能发给模型"},
		{CandidateID: "c00", Name: "维修组", Purpose: "伪造职责"},
	} {
		inner := &fakeModel{}
		trace := &observedModel{inner: inner, groups: fixtureDataset().Profiles["synthetic_profile"].Groups}
		if _, err := trace.Recommend(context.Background(), control.RecommendationInput{Need: "合成", Candidates: []control.RecommendationCandidate{candidate}}); err == nil || inner.calls != 0 || trace.authorized {
			t.Fatal("unauthorized input crossed actual model boundary")
		}
	}
}

func TestReportMetadataRejectsUnknownProviderStringsAndBoundsNumbers(t *testing.T) {
	model := &fakeModel{turn: func(context.Context, control.RecommendationInput) (string, recommendmodel.Report, error) {
		return "PRIVATE_RAW_MODEL_TEXT", recommendmodel.Report{Stage: "PRIVATE_STAGE", Events: []string{"PRIVATE_SESSION_ID", "response.done"}, TextBytes: 99999999, AudioEvents: -1, TotalMS: -1}, errors.New("PRIVATE_SECRET_ERROR")
	}}
	result := runFixture(fixtureDataset().Cases[0], model)
	encoded, _ := json.Marshal(result)
	if strings.Contains(string(encoded), "PRIVATE") || result.Observation.Stage != "unknown" || result.UnknownReportTypes != 1 || result.Observation.TextBytes != control.MaxRecommendationOutputBytes+1 || result.Observation.TotalMS != 0 {
		t.Fatal("untrusted report or error strings crossed report boundary")
	}
}

func TestSequentialRunStopsImmediatelyOnUnconfirmedCleanup(t *testing.T) {
	data := fixtureDataset()
	data.Cases = append(data.Cases, data.Cases[0])
	data.Cases[1].ID = "C02"
	pending := false
	model := &fakeModel{turn: func(context.Context, control.RecommendationInput) (string, recommendmodel.Report, error) {
		pending = true
		return "", recommendmodel.Report{Stage: "cleanup_unconfirmed", ResponseDone: true}, errors.New("unavailable")
	}}
	result := runEvaluation(context.Background(), data, data.Cases, model, runOptions{now: time.Now, wait: waitContext, cleanupPending: func() bool { return pending }, checkpoint: func(evaluation) error { return nil }})
	if result.Selected != 2 || len(result.Cases) != 1 || model.calls != 1 || result.StopReason != "cleanup_unconfirmed" || result.Metrics.CleanupUnconfirmed != 1 || result.Metrics.SemanticScored != 0 {
		t.Fatalf("cleanup failure admitted another case: %+v", result)
	}
}

func TestGlobalStartSpacingAndBudgetKeepPlannedDenominator(t *testing.T) {
	data := fixtureDataset()
	data.Cases = append(data.Cases, data.Cases[0])
	data.Cases[1].ID = "C02"
	now := time.Date(2026, 10, 25, 1, 0, 0, 0, time.UTC)
	waits := []time.Duration{}
	options := runOptions{now: func() time.Time { return now }, wait: func(_ context.Context, delay time.Duration) error {
		waits = append(waits, delay)
		now = now.Add(delay)
		return nil
	}, cleanupPending: func() bool { return false }, checkpoint: func(evaluation) error { return nil }}
	result := runEvaluation(context.Background(), data, data.Cases, &fakeModel{}, options)
	if len(waits) != 1 || waits[0] != startInterval || result.Metrics.SemanticPassed != 2 || result.Selected != 2 {
		t.Fatal("sequential five-second start gate missing")
	}
	model := &fakeModel{turn: func(_ context.Context, input control.RecommendationInput) (string, recommendmodel.Report, error) {
		now = now.Add(runLimit - minimumStartWindow + time.Second)
		return modelText(input, "维修组"), completedReport(), nil
	}}
	result = runEvaluation(context.Background(), data, data.Cases, model, options)
	if result.Selected != 2 || result.Metrics.Cases != 1 || result.StopReason != "run_deadline" || model.calls != 1 {
		t.Fatal("budget dropped unrun cases from planned denominator or admitted another call")
	}
}

func TestCancelledRunRecordsCaseAndRetainsCleanupObservation(t *testing.T) {
	data := fixtureDataset()
	ctx, cancel := context.WithCancel(context.Background())
	model := &fakeModel{turn: func(context.Context, control.RecommendationInput) (string, recommendmodel.Report, error) {
		cancel()
		return "", recommendmodel.Report{Stage: "response", StopConfirmed: true}, context.Canceled
	}}
	result := runEvaluation(ctx, data, data.Cases, model, runOptions{now: time.Now, wait: waitContext, cleanupPending: func() bool { return false }, checkpoint: func(evaluation) error { return nil }})
	if result.StopReason != "run_cancelled" || len(result.Cases) != 1 || result.Cases[0].Semantic != "not_scored" || result.Cases[0].Cleanup != "confirmed" {
		t.Fatal("interrupt lost terminal cleanup observation or became a semantic abstention")
	}
}

func TestCancellationBeforeCasesOrDuringSpacingIsNotADeadline(t *testing.T) {
	data := fixtureDataset()
	data.Cases = append(data.Cases, data.Cases[0])
	data.Cases[1].ID = "C02"
	for _, before := range []bool{true, false} {
		ctx, cancel := context.WithCancel(context.Background())
		model := &fakeModel{}
		if before {
			cancel()
		}
		result := runEvaluation(ctx, data, data.Cases, model, runOptions{now: time.Now,
			wait:           func(context.Context, time.Duration) error { cancel(); return context.Canceled },
			cleanupPending: func() bool { return false }, checkpoint: func(evaluation) error { return nil }})
		cancel()
		wantCalls := 1
		if before {
			wantCalls = 0
		}
		if result.StopReason != "run_cancelled" || result.Selected != 2 || model.calls != wantCalls {
			t.Fatal("user cancellation became a deadline or started another case")
		}
	}
}
