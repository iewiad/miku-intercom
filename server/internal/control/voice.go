package control

import (
	"context"
	"errors"
	"strings"
	"unicode/utf8"
)

const VoiceTargetIntent = "voice_target"
const VoiceTargetReasonNote = "请核对识别出的目标群，确认后才会切换。"

type VoiceProposalRequest struct {
	RequestID       string `json:"request_id"`
	Transcript      string `json:"transcript"`
	ExpectedVersion uint64 `json:"expected_version"`
	ServerEpoch     string `json:"server_epoch"`
}

// ProposeVoiceTarget never executes a switch. Exact names avoid a model turn,
// but receive the same bounded confirmation record as model-assisted targets.
func (s *Service) ProposeVoiceTarget(ctx context.Context, id string, req VoiceProposalRequest) (Recommendation, error) {
	return s.recommend(ctx, id, RecommendationRequest{RequestID: req.RequestID, Need: req.Transcript,
		ExpectedVersion: req.ExpectedVersion, ServerEpoch: req.ServerEpoch}, VoiceTargetIntent)
}

// voiceTargetNames is shared with the legacy resolver, including its ambiguity
// protection for punctuation which can also be literal group-name data.
func voiceTargetNames(transcript string) (map[string]bool, string) {
	names := map[string]bool{}
	acceptedPrefix := "none"
	for text := strings.TrimSpace(transcript); text != ""; {
		for _, prefix := range []struct{ text, code string }{{"切换到", "switch_to"}, {"切到", "switch_short"}, {"进入", "enter"}} {
			if strings.HasPrefix(text, prefix.text) {
				name := strings.TrimSpace(strings.TrimPrefix(text, prefix.text))
				if name != "" {
					acceptedPrefix = prefix.code
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
	return names, acceptedPrefix
}

// voiceIntentNegated is a deliberately narrow veto, not a second intent model.
// The caller first admits a unique literal target, so names remain data even
// when they contain cancellation words or punctuation. Mixed or unclear clauses
// fall through to model review; a veto never selects an alternative target.
func voiceIntentNegated(transcript string, candidates []recommendationGroup) bool {
	names := make(map[string]bool, len(candidates))
	for _, candidate := range candidates {
		names[candidate.Name] = true
	}
	clauses := strings.FieldsFunc(transcript, func(r rune) bool {
		return strings.ContainsRune(",，;；。.!！", r)
	})
	for len(clauses) > 0 && voiceCourtesyClause(clauses[len(clauses)-1]) {
		clauses = clauses[:len(clauses)-1]
	}
	if len(clauses) == 0 {
		return false
	}
	// An independently stated final global cancellation withdraws earlier
	// actions. A negated target ("不要进某组") is not a global cancellation.
	if len(clauses) > 1 && voiceGlobalCancellation(clauses[len(clauses)-1]) {
		return true
	}
	denied := false
	for _, clause := range clauses {
		if voiceCourtesyClause(clause) {
			continue
		}
		if !voiceGlobalCancellation(clause) && !voiceSimpleNegation(clause, names) {
			return false
		}
		denied = true
	}
	return denied
}

func voiceCourtesyClause(clause string) bool {
	switch strings.TrimSpace(clause) {
	case "请", "请你", "麻烦了", "谢谢", "谢谢你", "谢谢了", "好的", "好", "嗯":
		return true
	}
	return false
}

func voiceWithoutCourtesy(clause string) string {
	text := strings.TrimSpace(clause)
	for {
		previous := text
		for _, prefix := range []string{"请帮我", "麻烦你", "麻烦", "请你", "请", "帮我"} {
			if strings.HasPrefix(text, prefix) {
				text = strings.TrimSpace(strings.TrimPrefix(text, prefix))
				break
			}
		}
		if text == previous {
			return text
		}
	}
}

func voiceGlobalCancellation(clause string) bool {
	switch voiceWithoutCourtesy(clause) {
	case "取消", "取消切群", "取消切换", "取消换群", "取消本次切换", "取消本次操作",
		"别切了", "不要切了", "先别切了", "先不要切了", "先不切了", "不切了",
		"不要切群", "别切群", "不用切群", "先别切群", "先不要切群", "先不切群",
		"不要换群", "别换群", "不用换群", "不切换", "先不切换", "不用切换", "不需要切换",
		"保持当前群", "保持现在的群", "保持原群", "保持当前状态", "保持现在这样", "保持不变",
		"先保持当前群", "先保持原群", "先保持现在这样", "算了", "先算了":
		return true
	}
	return false
}

func voiceSimpleNegation(clause string, names map[string]bool) bool {
	text := voiceWithoutCourtesy(clause)
	negative := false
	for _, prefix := range []string{"先不要", "暂时不要", "暂时别", "不要", "先别", "别", "不用", "不必", "无需", "勿"} {
		if strings.HasPrefix(text, prefix) {
			text = strings.TrimSpace(strings.TrimPrefix(text, prefix))
			negative = true
			break
		}
	}
	if !negative {
		return false
	}
	for _, action := range []string{"切换到", "切换至", "切到", "换到", "转到", "转去", "转入", "进入", "进", "去"} {
		if strings.HasPrefix(text, action) {
			text = strings.TrimSpace(strings.TrimPrefix(text, action))
			break
		}
	}
	// Only a complete authorized name, with optional closed courtesy endings,
	// proves where this target ends. Unknown spellings or any remaining clause
	// are model input, not something a broad negation regex may discard.
	for text != "" {
		if names[text] {
			return true
		}
		previous := text
		for _, suffix := range []string{"谢谢你", "谢谢", "就好", "吧", "了"} {
			if strings.HasSuffix(text, suffix) {
				text = strings.TrimSpace(strings.TrimSuffix(text, suffix))
				break
			}
		}
		if text == previous {
			break
		}
	}
	return false
}

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
	names, prefix := voiceTargetNames(transcript)
	diagnostic.AcceptedPrefix = prefix
	// ASR sentence punctuation may also be literal group-name data. Consider the
	// unchanged command and every removable punctuation suffix; never prefer a
	// normalized spelling over another joined group with a literal suffix.
	for _, group := range st.Groups {
		if names[group.Name] {
			matches[group.ID] = group
		}
	}
	diagnostic.ExactMatchCount = len(matches)
	if prefix == "none" {
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
