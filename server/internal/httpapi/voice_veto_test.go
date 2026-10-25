package httpapi

import (
	"encoding/json"
	"testing"

	"github.com/iewiad/miku-intercom/server/internal/control"
)

func TestVoiceVetoHTTPNativeAndLocalHaveNoReviewOrModel(t *testing.T) {
	for _, native := range []bool{false, true} {
		for _, configured := range []bool{false, true} {
			t.Run(map[bool]string{false: "local", true: "native"}[native]+map[bool]string{false: "_without_model", true: "_with_model"}[configured], func(t *testing.T) {
				model := &apiRecommendationModel{}
				options := control.Options{}
				if configured {
					options.Recommender = model
				}
				s, err := control.New(nil, nil, options)
				if err != nil {
					t.Fatal(err)
				}
				device, token, err := s.CreateDevice("合成否定门禁终端")
				if err != nil {
					t.Fatal(err)
				}
				_, _, _, err = s.CreateGroup(device.ID, "合成后勤组")
				if err != nil {
					t.Fatal(err)
				}
				before, _ := s.State(device.ID)
				body, _ := json.Marshal(control.VoiceProposalRequest{RequestID: "voice_http_veto_001", Transcript: "请不要切到合成后勤组，谢谢。", ExpectedVersion: before.CurrentVersion, ServerEpoch: before.ServerEpoch})
				h := New(s)
				if native {
					h, err = NewDevice(s, nil, deviceHost)
					if err != nil {
						t.Fatal(err)
					}
				}
				if native {
					errorCode(t, nativeRequest(h, "POST", "/api/v1/voice/proposals", string(body), token, nil), 422, "voice_intent_negated")
				} else {
					errorCode(t, request(h, "POST", "/api/v1/voice/proposals", string(body), token, nil), 422, "voice_intent_negated")
				}
				after, _ := s.State(device.ID)
				if model.calls != 0 || after.CurrentGroupID != before.CurrentGroupID || after.CurrentVersion != before.CurrentVersion {
					t.Fatal("veto reached provider or changed group")
				}
			})
		}
	}
}
