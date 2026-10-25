package control

import (
	"context"
	"fmt"
	"strings"
	"testing"
	"time"
)

func voiceProposal(t *testing.T, f *fixture, id, requestID, transcript string) VoiceProposalRequest {
	t.Helper()
	st := state(t, f.s, id)
	return VoiceProposalRequest{RequestID: requestID, Transcript: transcript,
		ExpectedVersion: st.CurrentVersion, ServerEpoch: st.ServerEpoch}
}

func TestVoiceProposalExactNeedsConfirmationWithoutModel(t *testing.T) {
	f := setup(t)
	before, saves := state(t, f.s, f.a.ID), f.save.saves
	req := voiceProposal(t, f, f.a.ID, "voice_exact_001", " 进入后勤！ ")
	rec, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, req)
	must(t, err)
	if rec.Source != "exact" || len(rec.Suggestions) != 1 || rec.Suggestions[0].GroupID != f.g2.ID ||
		rec.Suggestions[0].Reason != VoiceTargetReasonNote || rec.ExpiresAt != f.clock.Now().Add(RecommendationTTL) {
		t.Fatalf("wrong exact proposal: %+v", rec)
	}
	after := state(t, f.s, f.a.ID)
	if after.CurrentVersion != before.CurrentVersion || after.CurrentGroupID != before.CurrentGroupID || saves != f.save.saves {
		t.Fatal("proposal executed or persisted a switch")
	}
	replay, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, req)
	must(t, err)
	if replay.RecommendationID != rec.RecommendationID || replay.Source != "exact" {
		t.Fatal("request replay minted another token")
	}
	confirm := RecommendationConfirmation{RecommendationID: rec.RecommendationID, OperationID: "voice_confirm_001",
		GroupID: f.g2.ID, ExpectedVersion: rec.CurrentVersion, ServerEpoch: rec.ServerEpoch}
	f.save.fail = true
	_, _, err = f.s.ConfirmRecommendation(f.a.ID, confirm)
	wantCode(t, err, "persistence_failed")
	if state(t, f.s, f.a.ID).CurrentVersion != before.CurrentVersion {
		t.Fatal("failed persistence changed current group")
	}
	f.save.fail = false
	op, current, err := f.s.ConfirmRecommendation(f.a.ID, confirm)
	must(t, err)
	if op.Status != "applied" || op.RecommendationID != rec.RecommendationID || current.CurrentGroupID != f.g2.ID {
		t.Fatal("explicit confirmation did not use shared switch transaction")
	}
	saves = f.save.saves
	f.clock.Advance(RecommendationTTL + time.Second)
	opAgain, _, err := f.s.ConfirmRecommendation(f.a.ID, confirm)
	must(t, err)
	if opAgain != op || f.save.saves != saves {
		t.Fatal("expired terminal replay repeated mutation")
	}
}

func TestVoiceProposalModeFingerprintAndLegacyS5Isolation(t *testing.T) {
	f := setup(t)
	calls := 0
	f.s.opts.Recommender = recommendationModelFunc(func(_ context.Context, input RecommendationInput) (string, error) {
		calls++
		if input.Intent != "" {
			t.Fatal("S5 acquired voice-target intent")
		}
		return `{"suggestions":[]}`, nil
	})
	req := voiceProposal(t, f, f.a.ID, "voice_shared_001", "切到后勤")
	_, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, req)
	must(t, err)
	need := RecommendationRequest{RequestID: req.RequestID, Need: req.Transcript, ExpectedVersion: req.ExpectedVersion, ServerEpoch: req.ServerEpoch}
	_, err = f.s.Recommend(context.Background(), f.a.ID, need)
	wantCode(t, err, "recommendation_request_conflict")
	if calls != 0 {
		t.Fatal("exact proposal or cross-mode replay called model")
	}
	f.clock.Advance(6 * time.Second)
	need.RequestID = "need_shared_001"
	rec, err := f.s.Recommend(context.Background(), f.a.ID, need)
	must(t, err)
	if rec.Source != "" || calls != 1 {
		t.Fatal("legacy S5 response or model admission changed")
	}
	req.RequestID = need.RequestID
	_, err = f.s.ProposeVoiceTarget(context.Background(), f.a.ID, req)
	wantCode(t, err, "recommendation_request_conflict")
}

func TestVoiceProposalInputBoundsBeforeAnyModel(t *testing.T) {
	for name, text := range map[string]string{
		"empty": " \n\t ", "runes": strings.Repeat("群", 257), "bytes": strings.Repeat("😀", 257),
		"utf8": string([]byte{0xff}), "NUL": "切到后\x00勤", "control": "切到后\n勤", "tab": "切到\t后勤",
	} {
		t.Run(name, func(t *testing.T) {
			f := setup(t)
			f.s.opts.Recommender = recommendationModelFunc(func(context.Context, RecommendationInput) (string, error) {
				t.Fatal("invalid input reached model")
				return "", nil
			})
			_, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, voiceProposal(t, f, f.a.ID, "voice_invalid_001", text))
			wantCode(t, err, "invalid_recommendation_request")
			if len(f.s.recommendations) != 0 {
				t.Fatal("invalid input allocated a review record")
			}
		})
	}
	f := setup(t)
	f.s.opts.Recommender = recommendationModelFunc(func(_ context.Context, input RecommendationInput) (string, error) {
		if len(input.Need) != 1024 || input.Intent != VoiceTargetIntent {
			t.Fatal("256-rune/1024-byte boundary or voice intent changed")
		}
		return `{"suggestions":[]}`, nil
	})
	_, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, voiceProposal(t, f, f.a.ID, "voice_boundary_001", strings.Repeat("😀", 256)))
	must(t, err)
}

func TestVoiceProposalModelSnapshotReasonAndNoExecution(t *testing.T) {
	f := setup(t)
	foreign, _, _, err := f.s.CreateGroup(f.b.ID, "不可见目标")
	must(t, err)
	calls := 0
	f.s.opts.Recommender = recommendationModelFunc(func(ctx context.Context, input RecommendationInput) (string, error) {
		calls++
		if input.Intent != VoiceTargetIntent || input.Need != "麻烦转到后秦吧" || len(input.Candidates) != 2 {
			t.Fatal("voice model did not receive bounded target intent and authorized snapshot")
		}
		deadline, ok := ctx.Deadline()
		if !ok || time.Until(deadline) > RecommendationModelTimeout {
			t.Fatal("voice model has no bounded deadline")
		}
		for _, c := range input.Candidates {
			if c.Name == foreign.Name || c.CandidateID == foreign.ID || c.CandidateID == f.g2.ID {
				t.Fatal("model received an unauthorized candidate or real group id")
			}
		}
		return selectingGroup(f.g2.Name)(ctx, input)
	})
	before, saves := state(t, f.s, f.a.ID), f.save.saves
	req := voiceProposal(t, f, f.a.ID, "voice_model_001", "麻烦转到后秦吧")
	rec, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, req)
	must(t, err)
	if calls != 1 || rec.Source != "model" || len(rec.Suggestions) != 1 || rec.Suggestions[0].Reason != VoiceTargetReasonNote {
		t.Fatal("voice result did not retain model provenance and fixed server note")
	}
	if current := state(t, f.s, f.a.ID); current.CurrentVersion != before.CurrentVersion || current.CurrentGroupID != before.CurrentGroupID || saves != f.save.saves {
		t.Fatal("model proposal executed a switch")
	}
	f.clock.Advance(RecommendationTTL + time.Second)
	op, _, err := f.s.ConfirmRecommendation(f.a.ID, RecommendationConfirmation{RecommendationID: rec.RecommendationID,
		OperationID: "voice_expired_001", GroupID: f.g2.ID, ExpectedVersion: rec.CurrentVersion, ServerEpoch: rec.ServerEpoch})
	must(t, err)
	if op.Status != "rejected" || op.ErrorCode != "recommendation_expired" {
		t.Fatal("voice review bypassed shared expiry guard")
	}
}

func TestVoiceProposalAmbiguityAndAbstentionDoNotAutoSelect(t *testing.T) {
	f := setup(t)
	_, _, _, err := f.s.CreateGroup(f.a.ID, "后勤！")
	must(t, err)
	f.s.opts.Recommender = recommendationModelFunc(func(_ context.Context, input RecommendationInput) (string, error) {
		var suggestions []string
		for _, c := range input.Candidates {
			if strings.HasPrefix(c.Name, "后勤") {
				suggestions = append(suggestions, fmt.Sprintf(`{"candidate_id":%q,"reason":"需核对两个候选"}`, c.CandidateID))
			}
		}
		return `{"suggestions":[` + strings.Join(suggestions, ",") + `]}`, nil
	})
	before := state(t, f.s, f.a.ID)
	rec, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, voiceProposal(t, f, f.a.ID, "voice_ambiguous_001", "切到后勤！！"))
	must(t, err)
	if rec.Source != "model" || len(rec.Suggestions) != 2 || state(t, f.s, f.a.ID).CurrentVersion != before.CurrentVersion {
		t.Fatal("ambiguous exact spellings silently chose first target")
	}
	// The scripted empty result tests protocol abstention, not model semantics.
	f.clock.Advance(6 * time.Second)
	f.s.opts.Recommender = recommendationModelFunc(func(context.Context, RecommendationInput) (string, error) { return `{"suggestions":[]}`, nil })
	rec, err = f.s.ProposeVoiceTarget(context.Background(), f.a.ID, voiceProposal(t, f, f.a.ID, "voice_abstain_001", "今天聊点别的"))
	must(t, err)
	op, _, err := f.s.ConfirmRecommendation(f.a.ID, RecommendationConfirmation{RecommendationID: rec.RecommendationID,
		OperationID: "voice_empty_001", GroupID: f.g2.ID, ExpectedVersion: rec.CurrentVersion, ServerEpoch: rec.ServerEpoch})
	must(t, err)
	if op.Status != "rejected" || op.ErrorCode != "recommendation_target_invalid" {
		t.Fatal("empty proposal accepted an arbitrary target")
	}
}

func TestVoiceProposalRevalidationAndCandidateOutputGuards(t *testing.T) {
	for _, fault := range []string{"outside", "duplicate", "stale_version", "changed_candidates", "expired", "cancelled"} {
		t.Run(fault, func(t *testing.T) {
			f := setup(t)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			f.s.opts.Recommender = recommendationModelFunc(func(ctx context.Context, input RecommendationInput) (string, error) {
				if fault == "outside" {
					return `{"suggestions":[{"candidate_id":"c99","reason":"越界"}]}`, nil
				}
				if fault == "duplicate" {
					return fmt.Sprintf(`{"suggestions":[{"candidate_id":%q,"reason":"一"},{"candidate_id":%q,"reason":"二"}]}`, input.Candidates[0].CandidateID, input.Candidates[0].CandidateID), nil
				}
				if fault == "stale_version" {
					st := state(t, f.s, f.a.ID)
					_, _, e := f.s.Switch(f.a.ID, SwitchRequest{OperationID: "other_switch_001", GroupID: f.g2.ID, ExpectedVersion: st.CurrentVersion})
					must(t, e)
				}
				if fault == "changed_candidates" {
					_, _, _, e := f.s.CreateGroup(f.a.ID, "新候选")
					must(t, e)
				}
				if fault == "expired" {
					f.clock.Advance(RecommendationTTL)
				}
				if fault == "cancelled" {
					cancel()
				}
				return selectingGroup(f.g2.Name)(ctx, input)
			})
			_, err := f.s.ProposeVoiceTarget(ctx, f.a.ID, voiceProposal(t, f, f.a.ID, "voice_fault_001", "帮我转到后秦"))
			code := map[string]string{"outside": "recommendation_invalid_output", "duplicate": "recommendation_invalid_output", "stale_version": "version_conflict", "changed_candidates": "recommendation_candidates_changed", "expired": "recommendation_expired", "cancelled": "recommendation_model_failed"}[fault]
			wantCode(t, err, code)
		})
	}
}
