package control

import (
	"strings"
	"unicode/utf8"
)

// ResolveVoice only interprets one complete, explicit command. It never executes a switch.
// Names remain data: no model output, substring, or fuzzy matching is trusted.
func (s *Service) ResolveVoice(id, transcript string) (Group, error) {
	if !utf8.ValidString(transcript) || utf8.RuneCountInString(transcript) > 256 {
		return Group{}, fail(400, "invalid_voice_command", "one explicit group command required")
	}
	st, err := s.State(id)
	if err != nil {
		return Group{}, err
	}
	matches := map[string]Group{}
	names := map[string]bool{}
	command := false
	// ASR sentence punctuation may also be literal group-name data. Consider the
	// unchanged command and every removable punctuation suffix; never prefer a
	// normalized spelling over another joined group with a literal suffix.
	for text := strings.TrimSpace(transcript); text != ""; {
		for _, prefix := range []string{"切换到", "切到", "进入"} {
			if strings.HasPrefix(text, prefix) {
				name := strings.TrimSpace(strings.TrimPrefix(text, prefix))
				if name != "" {
					command = true
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
	if !command {
		return Group{}, fail(400, "invalid_voice_command", "say an explicit switch command")
	}
	if len(matches) == 0 {
		return Group{}, fail(422, "voice_target_unknown", "no joined group exactly matches command")
	}
	if len(matches) != 1 {
		return Group{}, fail(422, "voice_target_ambiguous", "command matches multiple joined groups")
	}
	for _, group := range matches {
		return group, nil
	}
	panic("unreachable")
}
