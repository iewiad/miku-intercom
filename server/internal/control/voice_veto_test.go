package control

import (
	"context"
	"testing"
)

// These call the public service path and observe model admission, review
// records, persistence and current group, rather than testing regex internals.
func TestVoiceVetoPureDenialAndFinalGlobalCancel(t *testing.T) {
	for _, text := range []string{
		"不要切到后勤", "别进入后勤", "不用转到后勤", "请不要切到后勤，谢谢",
		"不要后勤，别进协作一组", "别切群，先保持现在这样", "取消切群", "先不切换", "请勿进入后勤",
		"切到后勤，不对，先别切了", "切到后勤，取消切群", "切到后勤，取消切群，谢谢",
	} {
		t.Run(text, func(t *testing.T) {
			f := setup(t)
			calls := 0
			f.s.opts.Recommender = recommendationModelFunc(func(context.Context, RecommendationInput) (string, error) {
				calls++
				return `{"suggestions":[]}`, nil
			})
			before, saves := state(t, f.s, f.a.ID), f.save.saves
			operations := len(f.s.disk.Devices[f.a.ID].Operations)
			rec, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, voiceProposal(t, f, f.a.ID, "voice_veto_001", text))
			wantCode(t, err, "voice_intent_negated")
			if calls != 0 || rec.RecommendationID != "" || rec.Source != "" || len(f.s.recommendations) != 0 || len(f.s.recommendationLast) != 0 {
				t.Fatal("veto called model, minted a token or admitted a recommendation record")
			}
			after := state(t, f.s, f.a.ID)
			if after.CurrentGroupID != before.CurrentGroupID || after.CurrentVersion != before.CurrentVersion || f.save.saves != saves || len(f.s.disk.Devices[f.a.ID].Operations) != operations {
				t.Fatal("veto changed group, persisted an operation or consumed a review")
			}
		})
	}
}

func TestVoiceVetoDefersMixedQuestionAndCorrection(t *testing.T) {
	for _, text := range []string{
		"不要后勤，切到协作一组", "切到后勤，不要进协作一组", "不要后勤，而是协作一组",
		"不要后勤然后切到协作一组", "不要切到后勤又转到协作一组", "不是不要后勤，帮我转过去",
		"不要切到后勤吗", "不要切到后勤？", "他说不要切到后勤", "不要切到后勤的话",
		"不要切到后勤我想协作一组", "不要切到后勤希望协作一组", "不要切到后勤请选协作一组",
		"不要切到后勤，不对，协作一组", "不要切到后勤，然后再说", "取消切群，切到后勤",
		"不要切到后秦", "不要切到未知组", "不要切到后勤如何",
	} {
		t.Run(text, func(t *testing.T) {
			f := setup(t)
			calls := 0
			f.s.opts.Recommender = recommendationModelFunc(func(_ context.Context, input RecommendationInput) (string, error) {
				calls++
				if input.Intent != VoiceTargetIntent || input.Need != text {
					t.Fatal("mixed intent was rewritten")
				}
				return `{"suggestions":[]}`, nil
			})
			rec, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, voiceProposal(t, f, f.a.ID, "voice_defer_001", text))
			must(t, err)
			if calls != 1 || rec.Source != "model" || len(rec.Suggestions) != 0 {
				t.Fatal("narrow veto swallowed a mixed/unclear intent")
			}
		})
	}
}

func TestVoiceVetoPreservesLiteralExactNamesAndLegacyResolver(t *testing.T) {
	for _, name := range []string{"不要打扰组", "后勤，取消切群", "别切了", "协作一组，不要进后勤"} {
		t.Run(name, func(t *testing.T) {
			f := setup(t)
			group, _, _, err := f.s.CreateGroup(f.a.ID, name)
			must(t, err)
			f.s.opts.Recommender = recommendationModelFunc(func(context.Context, RecommendationInput) (string, error) {
				t.Fatal("literal exact target reached model")
				return "", nil
			})
			text := "切到" + name
			rec, err := f.s.ProposeVoiceTarget(context.Background(), f.a.ID, voiceProposal(t, f, f.a.ID, "voice_literal_001", text))
			must(t, err)
			if rec.Source != "exact" || len(rec.Suggestions) != 1 || rec.Suggestions[0].GroupID != group.ID {
				t.Fatal("negative words inside a literal name triggered veto")
			}
			legacy, err := f.s.ResolveVoice(f.a.ID, text)
			must(t, err)
			if legacy.ID != group.ID {
				t.Fatal("legacy literal matching changed")
			}
		})
	}
}

func TestVoiceVetoWithoutModelAndAfterAuthorityGates(t *testing.T) {
	f := setup(t)
	req := voiceProposal(t, f, f.a.ID, "voice_gate_veto_001", "不要切到后勤")
	_, err := f.s.ProposeVoiceTarget(context.Background(), "unknown_identity", req)
	wantCode(t, err, "unauthorized")
	stale := req
	stale.ServerEpoch = "different_epoch"
	_, err = f.s.ProposeVoiceTarget(context.Background(), f.a.ID, stale)
	wantCode(t, err, "server_restarted")
	stale = req
	stale.ExpectedVersion++
	_, err = f.s.ProposeVoiceTarget(context.Background(), f.a.ID, stale)
	wantCode(t, err, "version_conflict")
	_, err = f.s.ProposeVoiceTarget(context.Background(), f.a.ID, req)
	wantCode(t, err, "voice_intent_negated")
	_, err = f.s.ResolveVoice(f.a.ID, req.Transcript)
	wantCode(t, err, "invalid_voice_command")
	// The old S5 route still delegates this text to its original model policy.
	calls := 0
	f.s.opts.Recommender = recommendationModelFunc(func(_ context.Context, input RecommendationInput) (string, error) {
		calls++
		if input.Intent != "" {
			t.Fatal("S5 intent changed")
		}
		return `{"suggestions":[]}`, nil
	})
	_, err = f.s.Recommend(context.Background(), f.a.ID, RecommendationRequest{RequestID: "need_not_veto_001", Need: req.Transcript, ExpectedVersion: req.ExpectedVersion, ServerEpoch: req.ServerEpoch})
	must(t, err)
	if calls != 1 {
		t.Fatal("voice veto leaked into S5")
	}
}
