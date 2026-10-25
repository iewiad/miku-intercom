package control

import (
	"strings"
	"testing"
	"time"
	"unicode/utf8"
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

func TestVoiceResolutionDiagnosticOutcomes(t *testing.T) {
	cases := []struct {
		name, text, prefix, outcome, code string
		invalidIdentity, ambiguous        bool
		candidates, matches               int
	}{
		{name: "long prefix", text: " 切换到 后勤。 ", prefix: "switch_to", outcome: "success", code: "none", candidates: 2, matches: 1},
		{name: "short prefix", text: "切到后勤", prefix: "switch_short", outcome: "success", code: "none", candidates: 2, matches: 1},
		{name: "enter prefix", text: "进入后勤！", prefix: "enter", outcome: "success", code: "none", candidates: 2, matches: 1},
		{name: "bare target", text: "后勤", prefix: "none", outcome: "invalid_command", code: "invalid_voice_command", candidates: 2},
		{name: "empty target", text: "切到", prefix: "none", outcome: "invalid_command", code: "invalid_voice_command", candidates: 2},
		{name: "negated command", text: "不要切到后勤", prefix: "none", outcome: "invalid_command", code: "invalid_voice_command", candidates: 2},
		{name: "literal unknown", text: "切到后勤然后进入协作一组", prefix: "switch_short", outcome: "unknown_target", code: "voice_target_unknown", candidates: 2},
		{name: "punctuation ambiguity", text: "进入后勤！！", prefix: "enter", outcome: "ambiguous_target", code: "voice_target_ambiguous", candidates: 3, matches: 2, ambiguous: true},
		{name: "invalid UTF8", text: string([]byte{0xff}), prefix: "none", outcome: "invalid_command", code: "invalid_voice_command"},
		{name: "rune limit", text: strings.Repeat("切", 257), prefix: "none", outcome: "invalid_command", code: "invalid_voice_command"},
		{name: "service failure", text: "切到后勤", prefix: "none", outcome: "service_error", code: "unauthorized", invalidIdentity: true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			f := setup(t)
			if tc.ambiguous {
				_, _, _, err := f.s.CreateGroup(f.a.ID, "后勤！")
				must(t, err)
			}
			id := f.a.ID
			if tc.invalidIdentity {
				id = "unrecognized-private-identity"
			}
			group, diagnostic, err := f.s.ResolveVoiceWithDiagnostics(id, tc.text)
			if tc.code == "none" {
				must(t, err)
				if group.ID != f.g2.ID {
					t.Fatal("diagnostic resolver selected wrong group")
				}
			} else {
				wantCode(t, err, tc.code)
				if group.ID != "" {
					t.Fatal("failed resolution returned a target")
				}
			}
			want := VoiceResolutionDiagnostics{
				InputEvaluated: true, UTF8Valid: utf8.ValidString(tc.text), InputRunes: utf8.RuneCountInString(tc.text), InputBytes: len(tc.text),
				AcceptedPrefix: tc.prefix, CandidateCount: tc.candidates, ExactMatchCount: tc.matches, Outcome: tc.outcome, ErrorCode: tc.code,
			}
			if diagnostic != want {
				t.Fatalf("diagnostics = %+v, want %+v", diagnostic, want)
			}
		})
	}
}

func TestVoiceDiagnosticsUseOneAuthorizedSnapshot(t *testing.T) {
	f := setup(t)
	snapshots := 0
	f.s.opts.Now = func() time.Time {
		snapshots++
		return f.clock.Now()
	}
	// Another device's joined groups and global groups must not be counted.
	_, diagnostic, err := f.s.ResolveVoiceWithDiagnostics(f.b.ID, "切到后勤")
	wantCode(t, err, "voice_target_unknown")
	if snapshots != 1 || diagnostic.CandidateCount != 1 || diagnostic.ExactMatchCount != 0 {
		t.Fatalf("resolution did not use one authorized snapshot: calls=%d diagnostic=%+v", snapshots, diagnostic)
	}
	snapshots = 0
	_, err = f.s.ResolveVoice(f.a.ID, "切到后勤")
	must(t, err)
	if snapshots != 1 {
		t.Fatalf("compatibility resolver took %d snapshots", snapshots)
	}
	snapshots = 0
	_, _, err = f.s.ResolveVoiceWithDiagnostics(f.a.ID, string([]byte{0xff}))
	wantCode(t, err, "invalid_voice_command")
	if snapshots != 0 {
		t.Fatal("invalid input accessed service state")
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
