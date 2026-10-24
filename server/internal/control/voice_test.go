package control

import (
	"strings"
	"testing"
)

func TestVoiceResolveIsExactAuthorizedAndReadOnly(t *testing.T) {
	f := setup(t)
	before := state(t, f.s, f.a.ID)
	saves := f.save.saves
	for _, text := range []string{"切到后勤", " 切换到 后勤。 ", "进入后勤！"} {
		g, e := f.s.ResolveVoice(f.a.ID, text)
		must(t, e)
		if g.ID != f.g2.ID {
			t.Fatal("wrong target")
		}
	}
	for _, text := range []string{"后勤", "我想切到后勤", "不要切到后勤", "切到", "", "切到后勤然后进入协作一组", "切到后勤？", strings.Repeat("切", 257), string([]byte{0xff})} {
		if _, e := f.s.ResolveVoice(f.a.ID, text); e == nil {
			t.Fatalf("accepted %q", text)
		}
	}
	_, e := f.s.ResolveVoice(f.b.ID, "切到后勤")
	wantCode(t, e, "voice_target_unknown")
	after := state(t, f.s, f.a.ID)
	if before.CurrentGroupID != after.CurrentGroupID || before.CurrentVersion != after.CurrentVersion || f.save.saves != saves {
		t.Fatal("resolve changed state")
	}
}

func TestVoicePunctuationAndPrefixCandidatesMustRemainUnique(t *testing.T) {
	cases := []struct {
		name                string
		groups              []string
		command, want, code string
	}{
		{"literal and sentence punctuation", []string{"后勤", "后勤！"}, "切到后勤！", "", "voice_target_ambiguous"},
		{"intermediate punctuation suffix", []string{"后勤！", "后勤！！"}, "切换到后勤！！！", "", "voice_target_ambiguous"},
		{"mixed punctuation suffix", []string{"后勤！", "后勤！。"}, "进入后勤！。。", "", "voice_target_ambiguous"},
		{"unique literal punctuation", []string{"后勤！！"}, "切到后勤！！", "后勤！！", ""},
		{"sentence punctuation after literal", []string{"后勤！"}, "进入后勤！！。", "后勤！", ""},
		{"full group name not prefix", []string{"后勤", "后勤一组"}, "切换到后勤一组。", "后勤一组", ""},
		{"command prefix is literal name data", []string{"后勤", "切到后勤"}, "进入切到后勤！", "切到后勤", ""},
		{"prefix cannot authorize bare group name", []string{"进入后勤"}, "进入后勤", "", "voice_target_unknown"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			s, e := New(nil, nil, Options{})
			must(t, e)
			device, _, e := s.CreateDevice("测试")
			must(t, e)
			for _, name := range tc.groups {
				_, _, _, e = s.CreateGroup(device.ID, name)
				must(t, e)
			}
			before := state(t, s, device.ID)
			group, e := s.ResolveVoice(device.ID, tc.command)
			if tc.code != "" {
				wantCode(t, e, tc.code)
			} else {
				must(t, e)
				if group.Name != tc.want {
					t.Fatalf("selected %q want %q", group.Name, tc.want)
				}
			}
			after := state(t, s, device.ID)
			if before.CurrentGroupID != after.CurrentGroupID || before.CurrentVersion != after.CurrentVersion {
				t.Fatal("resolver changed group")
			}
		})
	}
}
