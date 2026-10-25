package recommendmodel

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

// The real adapter/DataChannel turn is exercised against a fake remote peer.
// This verifies prompt routing and cleanup, not the model's semantic choices.
func TestVoiceTargetProductionPromptWireAndCleanup(t *testing.T) {
	observation := &fakeTurnObservation{prompts: make(chan string, 1)}
	proxy, creates := fakeRTC(t, true, observation)
	model := New(proxy)
	input := control.RecommendationInput{Intent: control.VoiceTargetIntent, Need: "帮我转到合成后秦组",
		Candidates: []control.RecommendationCandidate{{CandidateID: "c00", Name: "合成后勤组", Purpose: "不能据此选组"}}}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	output, report, err := model.RecommendWithReport(ctx, input)
	if err != nil || output != `{"suggestions":[]}` || creates.Load() != 1 || !report.StopConfirmed || report.MicrophonePaths != 0 || report.PlaybackPaths != 0 {
		t.Fatalf("voice adapter turn failed report=%+v err=%v", report, err)
	}
	var prompt string
	select {
	case prompt = <-observation.prompts:
	default:
		t.Fatal("actual provider input not observed")
	}
	policy, data, ok := strings.Cut(prompt, recommendationDataMarker)
	if !ok {
		t.Fatal("reference data boundary missing")
	}
	sum := sha256.Sum256([]byte(policy + recommendationDataMarker))
	if hex.EncodeToString(sum[:]) != VoiceTargetPromptSHA256() || VoiceTargetPromptSHA256() == PromptSHA256() || VoiceTargetPromptVersion == PromptVersion {
		t.Fatal("voice policy routing/fingerprint was not independent of S5")
	}
	var decoded control.RecommendationInput
	if json.Unmarshal([]byte(data), &decoded) != nil || decoded.Intent != input.Intent || decoded.Need != input.Need || len(decoded.Candidates) != 1 || decoded.Candidates[0] != input.Candidates[0] {
		t.Fatal("typed voice reference data changed on the wire")
	}
	var fields map[string]json.RawMessage
	if json.Unmarshal([]byte(data), &fields) != nil || len(fields) != 3 || fields["intent"] == nil || fields["need"] == nil || fields["candidates"] == nil {
		t.Fatal("voice model received unexpected reference fields")
	}
	if len(policy)+len(recommendationDataMarker)+24576 > 32768 {
		t.Fatal("voice policy narrows existing adapter input bound")
	}
}

func TestVoiceTargetUnknownModeDoesNotCreateProviderSession(t *testing.T) {
	proxy, creates := fakeRTC(t, true)
	_, report, err := New(proxy).RecommendWithReport(context.Background(), control.RecommendationInput{Intent: "untrusted_override", Need: "合成输入"})
	if err == nil || creates.Load() != 0 || report.StopConfirmed || report.Stage != "prepare" {
		t.Fatal("unknown intent reached provider")
	}
}
