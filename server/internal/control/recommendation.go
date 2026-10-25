package control

import (
	"context"
	"encoding/json"
	"io"
	"sort"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

const (
	RecommendationTTL               = 60 * time.Second
	RecommendationModelTimeout      = 20 * time.Second
	MaxRecommendationCandidates     = 32
	MaxRecommendationOutputBytes    = 4096
	MaxRecommendationCandidateBytes = 16 * 1024
	MaxRecommendationRecords        = 256
)

// RecommendationReasonPolicy identifies the server-owned explanation policy,
// independently of the model prompt that selects candidates.
const RecommendationReasonPolicy = "server-neutral-v1"

// RecommendationReasonNote does not assert facts about the need or group
// capabilities. Model-generated reasons are validated but never published.
const RecommendationReasonNote = "模型建议联系该组，请核对群职责后确认。"

// RecommendationModel returns untrusted text only. Implementations must never
// execute a switch, access another identity, collect group audio, or play audio.
type RecommendationModel interface {
	Recommend(context.Context, RecommendationInput) (string, error)
}
type RecommendationCandidate struct {
	CandidateID string `json:"candidate_id"`
	Name        string `json:"name"`
	Purpose     string `json:"purpose,omitempty"`
}
type RecommendationInput struct {
	Need       string                    `json:"need"`
	Candidates []RecommendationCandidate `json:"candidates"`
}
type RecommendationRequest struct {
	RequestID       string `json:"request_id"`
	Need            string `json:"need"`
	ExpectedVersion uint64 `json:"expected_version"`
	ServerEpoch     string `json:"server_epoch"`
}
type RecommendationSuggestion struct {
	GroupID string `json:"group_id"`
	Name    string `json:"name"`
	Purpose string `json:"purpose,omitempty"`
	Reason  string `json:"reason"`
}
type Recommendation struct {
	RecommendationID string                     `json:"recommendation_id"`
	RequestID        string                     `json:"request_id"`
	ServerEpoch      string                     `json:"server_epoch"`
	CurrentVersion   uint64                     `json:"current_version"`
	CandidateDigest  string                     `json:"candidate_digest"`
	ExpiresAt        time.Time                  `json:"expires_at"`
	Suggestions      []RecommendationSuggestion `json:"suggestions"`
}
type RecommendationConfirmation struct {
	RecommendationID string `json:"recommendation_id"`
	OperationID      string `json:"operation_id"`
	GroupID          string `json:"group_id"`
	ExpectedVersion  uint64 `json:"expected_version"`
	ServerEpoch      string `json:"server_epoch"`
}
type recommendationRecord struct {
	requestDigest  string
	recommendation Recommendation
	candidates     []recommendationGroup
	ready          bool
	err            error
}
type recommendationGroup struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Purpose string `json:"purpose,omitempty"`
}

func boundedRecommendationText(s string, max int) bool {
	if !utf8.ValidString(s) || strings.TrimSpace(s) != s || s == "" || utf8.RuneCountInString(s) > max {
		return false
	}
	for _, r := range s {
		if unicode.IsControl(r) {
			return false
		}
	}
	return true
}
func (s *Service) RecommendationAvailable() bool { return s.opts.Recommender != nil }

func (s *Service) recommendationCandidates(d *deviceRecord) ([]recommendationGroup, string) {
	groups := make([]recommendationGroup, 0, len(d.Memberships))
	for gid := range d.Memberships {
		if g := s.disk.Groups[gid]; g != nil {
			groups = append(groups, recommendationGroup{ID: g.ID, Name: g.Name, Purpose: g.Purpose})
		}
	}
	sort.Slice(groups, func(i, j int) bool { return groups[i].ID < groups[j].ID })
	b, _ := json.Marshal(groups)
	return groups, digest(string(b))
}
func (s *Service) pruneRecommendations(now time.Time) int {
	total := 0
	for id, records := range s.recommendations {
		for requestID, record := range records {
			if record.ready && !now.Before(record.recommendation.ExpiresAt) {
				delete(records, requestID)
			}
		}
		if len(records) == 0 {
			delete(s.recommendations, id)
		}
		total += len(records)
	}
	// Expired rate entries cannot accumulate across arbitrarily many identities.
	for id, last := range s.recommendationLast {
		if now.Sub(last) >= RecommendationTTL {
			delete(s.recommendationLast, id)
		}
	}
	return total
}

// Recommend captures authorized candidates under the control lock, runs the
// bounded text model without that lock, then revalidates the complete snapshot.
func (s *Service) Recommend(ctx context.Context, id string, req RecommendationRequest) (Recommendation, error) {
	req.Need = strings.TrimSpace(req.Need)
	if !validID(req.RequestID) || !boundedRecommendationText(req.Need, 512) || !validID(req.ServerEpoch) {
		return Recommendation{}, fail(400, "invalid_recommendation_request", "a request id, epoch and need of 1 to 512 characters are required")
	}
	encoded, _ := json.Marshal(req)
	fingerprint := digest(string(encoded))
	s.mu.Lock()
	now := s.opts.Now().UTC()
	d, err := s.identity(id)
	if err != nil {
		s.mu.Unlock()
		return Recommendation{}, err
	}
	if s.opts.Recommender == nil {
		s.mu.Unlock()
		return Recommendation{}, fail(503, "recommendation_unavailable", "the configured RTA text capability has not been verified")
	}
	total := s.pruneRecommendations(now)
	records := s.recommendations[id]
	if record := records[req.RequestID]; record != nil {
		requestDigest, ready, result, recordErr := record.requestDigest, record.ready, record.recommendation, record.err
		s.mu.Unlock()
		if requestDigest != fingerprint {
			return Recommendation{}, fail(409, "recommendation_request_conflict", "request id was used with a different payload")
		}
		if !ready {
			return Recommendation{}, fail(409, "recommendation_pending", "recommendation is still running; retry the same request")
		}
		return copyRecommendation(result), recordErr
	}
	if req.ServerEpoch != s.epoch {
		s.mu.Unlock()
		return Recommendation{}, fail(409, "server_restarted", "refresh state before requesting a recommendation")
	}
	if req.ExpectedVersion != d.CurrentVersion {
		s.mu.Unlock()
		return Recommendation{}, fail(409, "version_conflict", "refresh the current group before requesting a recommendation")
	}
	if total >= MaxRecommendationRecords || len(records) >= 8 {
		s.mu.Unlock()
		return Recommendation{}, fail(429, "recommendation_capacity", "recommendation capacity is full; retry later")
	}
	for _, record := range records {
		if !record.ready {
			s.mu.Unlock()
			return Recommendation{}, fail(409, "recommendation_pending", "one recommendation is already running")
		}
	}
	if last, ok := s.recommendationLast[id]; ok && now.Sub(last) < 5*time.Second {
		s.mu.Unlock()
		return Recommendation{}, fail(429, "recommendation_rate_limited", "wait five seconds before another need")
	}
	candidates, candidateDigest := s.recommendationCandidates(d)
	if len(candidates) == 0 {
		s.mu.Unlock()
		return Recommendation{}, fail(422, "recommendation_no_candidates", "join a group before requesting a recommendation")
	}
	candidateBytes, _ := json.Marshal(candidates)
	if len(candidates) > MaxRecommendationCandidates || len(candidateBytes) > MaxRecommendationCandidateBytes {
		s.mu.Unlock()
		return Recommendation{}, fail(422, "recommendation_candidates_too_large", "joined group directory exceeds the bounded model candidate limit")
	}
	recID, err := s.randomID("rec_", 24)
	if err != nil {
		s.mu.Unlock()
		return Recommendation{}, err
	}
	recommendation := Recommendation{RecommendationID: recID, RequestID: req.RequestID, ServerEpoch: s.epoch, CurrentVersion: d.CurrentVersion, CandidateDigest: candidateDigest, ExpiresAt: now.Add(RecommendationTTL), Suggestions: []RecommendationSuggestion{}}
	record := &recommendationRecord{requestDigest: fingerprint, recommendation: recommendation, candidates: candidates}
	if records == nil {
		records = map[string]*recommendationRecord{}
		s.recommendations[id] = records
	}
	records[req.RequestID] = record
	s.recommendationLast[id] = now
	input := RecommendationInput{Need: req.Need, Candidates: make([]RecommendationCandidate, len(candidates))}
	for i, g := range candidates {
		input.Candidates[i] = RecommendationCandidate{CandidateID: candidateKey(i), Name: g.Name, Purpose: g.Purpose}
	}
	s.mu.Unlock()
	modelCtx, cancel := context.WithTimeout(ctx, RecommendationModelTimeout)
	raw, modelErr := s.opts.Recommender.Recommend(modelCtx, input)
	modelContextErr := modelCtx.Err()
	cancel()
	suggestions, parseErr := parseRecommendation(raw, candidates)
	s.mu.Lock()
	defer s.mu.Unlock()
	record.ready = true
	switch {
	case modelErr != nil || modelContextErr != nil || ctx.Err() != nil:
		record.err = fail(503, "recommendation_model_failed", "the model did not return a verified text result")
	case parseErr != nil:
		record.err = parseErr
	default:
		current, currentErr := s.identity(id)
		if currentErr != nil {
			record.err = currentErr
			break
		}
		_, currentDigest := s.recommendationCandidates(current)
		switch {
		case !s.opts.Now().UTC().Before(recommendation.ExpiresAt):
			record.err = fail(409, "recommendation_expired", "recommendation expired before it was ready")
		case current.CurrentVersion != recommendation.CurrentVersion:
			record.err = fail(409, "version_conflict", "current group changed while the model was running")
		case currentDigest != recommendation.CandidateDigest:
			record.err = fail(409, "recommendation_candidates_changed", "joined groups changed while the model was running")
		default:
			record.recommendation.Suggestions = suggestions
		}
	}
	return copyRecommendation(record.recommendation), record.err
}

func candidateKey(index int) string {
	// Fixed-width, opaque identifiers cannot be confused with names or instructions.
	const alphabet = "0123456789abcdef"
	return "c" + string([]byte{alphabet[index/16], alphabet[index%16]})
}
func parseRecommendation(raw string, candidates []recommendationGroup) ([]RecommendationSuggestion, error) {
	invalid := func() ([]RecommendationSuggestion, error) {
		return nil, fail(502, "recommendation_invalid_output", "model output did not exactly match authorized candidates")
	}
	if len(raw) > MaxRecommendationOutputBytes || !utf8.ValidString(raw) || invalidRecommendationJSONKeys(raw) {
		return invalid()
	}
	var output struct {
		Suggestions []struct {
			CandidateID string `json:"candidate_id"`
			Reason      string `json:"reason"`
		} `json:"suggestions"`
	}
	decoder := json.NewDecoder(strings.NewReader(raw))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&output) != nil || decoder.Decode(new(any)) != io.EOF || output.Suggestions == nil || len(output.Suggestions) > 3 {
		return invalid()
	}
	results := make([]RecommendationSuggestion, 0, len(output.Suggestions))
	seen := map[string]bool{}
	for _, suggestion := range output.Suggestions {
		if seen[suggestion.CandidateID] || !boundedRecommendationText(suggestion.Reason, 160) {
			return invalid()
		}
		matched := false
		for i, group := range candidates {
			if candidateKey(i) == suggestion.CandidateID {
				results = append(results, RecommendationSuggestion{GroupID: group.ID, Name: group.Name, Purpose: group.Purpose, Reason: RecommendationReasonNote})
				matched = true
				break
			}
		}
		if !matched {
			return invalid()
		}
		seen[suggestion.CandidateID] = true
	}
	return results, nil
}

func (s *Service) ConfirmRecommendation(id string, req RecommendationConfirmation) (Operation, State, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	d, err := s.identity(id)
	if err != nil {
		return Operation{}, State{}, err
	}
	if !validID(req.RecommendationID) || !validID(req.OperationID) || !validID(req.GroupID) || !validID(req.ServerEpoch) {
		return Operation{}, State{}, fail(400, "invalid_request", "valid recommendation, operation, group and epoch identifiers required")
	}
	switchReq := SwitchRequest{OperationID: req.OperationID, GroupID: req.GroupID, ExpectedVersion: req.ExpectedVersion}
	// A saved terminal is authoritative even after expiry, restart or membership
	// changes. Payload conflicts are checked by the shared switch implementation.
	if _, ok := d.Operations[req.OperationID]; ok {
		return s.switchLocked(id, switchReq, now, req.RecommendationID, req.ServerEpoch, "")
	}
	rejection := ""
	for _, op := range d.Operations {
		if op.RecommendationID == req.RecommendationID {
			rejection = "recommendation_consumed"
			break
		}
	}
	var record *recommendationRecord
	for _, candidate := range s.recommendations[id] {
		if candidate.recommendation.RecommendationID == req.RecommendationID {
			record = candidate
			break
		}
	}
	if rejection == "" {
		switch {
		case req.ServerEpoch != s.epoch:
			rejection = "server_restarted"
		case record == nil || !record.ready || record.err != nil:
			rejection = "recommendation_unknown"
		case !now.Before(record.recommendation.ExpiresAt):
			rejection = "recommendation_expired"
		case req.ExpectedVersion != record.recommendation.CurrentVersion:
			rejection = "version_conflict"
		default:
			_, currentDigest := s.recommendationCandidates(d)
			selected := false
			for _, suggestion := range record.recommendation.Suggestions {
				if suggestion.GroupID == req.GroupID {
					selected = true
					break
				}
			}
			switch {
			case !d.Memberships[req.GroupID]:
				rejection = "not_member"
			case !selected:
				rejection = "recommendation_target_invalid"
			case currentDigest != record.recommendation.CandidateDigest:
				rejection = "recommendation_candidates_changed"
			}
		}
	}
	return s.switchLocked(id, switchReq, now, req.RecommendationID, req.ServerEpoch, rejection)
}

// Decoder.DisallowUnknownFields accepts duplicate keys and matches property names
// without regard to case. Require canonical spellings before decoding untrusted
// model text, so aliases cannot bypass the duplicate-key check.
func invalidRecommendationJSONKeys(raw string) bool {
	d := json.NewDecoder(strings.NewReader(raw))
	var value func() bool
	value = func() bool {
		token, err := d.Token()
		if err != nil {
			return true
		}
		delim, ok := token.(json.Delim)
		if !ok {
			return false
		}
		if delim == '{' {
			seen := map[string]bool{}
			for d.More() {
				token, err = d.Token()
				if err != nil {
					return true
				}
				key, ok := token.(string)
				if !ok || seen[key] || (key != "suggestions" && key != "candidate_id" && key != "reason") {
					return true
				}
				seen[key] = true
				if value() {
					return true
				}
			}
		} else if delim == '[' {
			for d.More() {
				if value() {
					return true
				}
			}
		} else {
			return true
		}
		_, err = d.Token()
		return err != nil
	}
	return value()
}

func copyRecommendation(rec Recommendation) Recommendation {
	rec.Suggestions = append([]RecommendationSuggestion{}, rec.Suggestions...)
	return rec
}
