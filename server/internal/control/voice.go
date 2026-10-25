package control

import (
	"errors"
	"strings"
	"unicode/utf8"
)

// VoiceResolutionDiagnostics contains only counts and fixed enum values. It must
// never contain command text, group data, or identity/credential material.
// UTF8Valid and input counts are meaningful only when InputEvaluated is true.
// CandidateCount is the number of joined groups in the resolution's State snapshot;
// ExactMatchCount counts distinct group IDs matched in that same snapshot.
type VoiceResolutionDiagnostics struct {
	InputEvaluated  bool
	UTF8Valid       bool
	InputRunes      int
	InputBytes      int
	AcceptedPrefix  string
	CandidateCount  int
	ExactMatchCount int
	Outcome         string
	ErrorCode       string
}

// ResolveVoice only interprets one complete, explicit command. It never executes a switch.
// Names remain data: no model output, substring, or fuzzy matching is trusted.
func (s *Service) ResolveVoice(id, transcript string) (Group, error) {
	group, _, err := s.ResolveVoiceWithDiagnostics(id, transcript)
	return group, err
}

// ResolveVoiceWithDiagnostics uses exactly the same snapshot and matching rules as
// ResolveVoice. AcceptedPrefix is none unless a supported prefix has a nonempty
// literal target; its other values are switch_to, switch_short, and enter.
func (s *Service) ResolveVoiceWithDiagnostics(id, transcript string) (Group, VoiceResolutionDiagnostics, error) {
	diagnostic := VoiceResolutionDiagnostics{
		InputEvaluated: true,
		UTF8Valid:      utf8.ValidString(transcript),
		InputRunes:     utf8.RuneCountInString(transcript),
		InputBytes:     len(transcript),
		AcceptedPrefix: "none",
		Outcome:        "invalid_command",
		ErrorCode:      "invalid_voice_command",
	}
	if !diagnostic.UTF8Valid || diagnostic.InputRunes > 256 {
		return Group{}, diagnostic, fail(400, "invalid_voice_command", "one explicit group command required")
	}
	st, err := s.State(id)
	if err != nil {
		diagnostic.Outcome, diagnostic.ErrorCode = "service_error", "service_error"
		var controlError *Error
		if errors.As(err, &controlError) && controlError.Code == "unauthorized" {
			diagnostic.ErrorCode = "unauthorized"
		}
		return Group{}, diagnostic, err
	}
	diagnostic.CandidateCount = len(st.Groups)
	matches := map[string]Group{}
	names := map[string]bool{}
	command := false
	// ASR sentence punctuation may also be literal group-name data. Consider the
	// unchanged command and every removable punctuation suffix; never prefer a
	// normalized spelling over another joined group with a literal suffix.
	for text := strings.TrimSpace(transcript); text != ""; {
		for _, prefix := range []struct{ text, code string }{{"切换到", "switch_to"}, {"切到", "switch_short"}, {"进入", "enter"}} {
			if strings.HasPrefix(text, prefix.text) {
				name := strings.TrimSpace(strings.TrimPrefix(text, prefix.text))
				if name != "" {
					command = true
					diagnostic.AcceptedPrefix = prefix.code
					names[name] = true
				}
			}
		}
		last, width := utf8.DecodeLastRuneInString(text)
		if !strings.ContainsRune("。.!！", last) {
			break
		}
		text = strings.TrimSpace(text[:len(text)-width])
	}
	for _, group := range st.Groups {
		if names[group.Name] {
			matches[group.ID] = group
		}
	}
	diagnostic.ExactMatchCount = len(matches)
	if !command {
		return Group{}, diagnostic, fail(400, "invalid_voice_command", "say an explicit switch command")
	}
	if len(matches) == 0 {
		diagnostic.Outcome, diagnostic.ErrorCode = "unknown_target", "voice_target_unknown"
		return Group{}, diagnostic, fail(422, "voice_target_unknown", "no joined group exactly matches command")
	}
	if len(matches) != 1 {
		diagnostic.Outcome, diagnostic.ErrorCode = "ambiguous_target", "voice_target_ambiguous"
		return Group{}, diagnostic, fail(422, "voice_target_ambiguous", "command matches multiple joined groups")
	}
	diagnostic.Outcome, diagnostic.ErrorCode = "success", "none"
	for _, group := range matches {
		return group, diagnostic, nil
	}
	panic("unreachable")
}
