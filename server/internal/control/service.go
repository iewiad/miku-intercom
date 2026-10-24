package control

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"
)

const MaxOperations = 1024
const MaxRequests = 256
const MaxPurposeRunes = 256
const LeaseDuration = 3 * time.Second
const RequestProtection = 60 * time.Second

type Service struct {
	mediaPeers         map[string]MediaBinding
	mu                 sync.Mutex
	disk               diskState
	saver              Saver
	opts               Options
	epoch              string
	tokens             map[string]string
	codes              map[string]string
	floors             map[string]*lease
	requests           map[string]map[string]*requestRecord
	badCodes           map[string][]time.Time
	recommendations    map[string]map[string]*recommendationRecord
	recommendationLast map[string]time.Time
}

func New(snapshot []byte, saver Saver, opts Options) (*Service, error) {
	opts.defaults()
	s := &Service{mediaPeers: map[string]MediaBinding{}, saver: saver, opts: opts, floors: map[string]*lease{}, requests: map[string]map[string]*requestRecord{}, badCodes: map[string][]time.Time{}, recommendations: map[string]map[string]*recommendationRecord{}, recommendationLast: map[string]time.Time{}}
	s.disk = diskState{FormatVersion: 1, Devices: map[string]*deviceRecord{}, Groups: map[string]*groupRecord{}}
	if len(snapshot) > 0 {
		if !utf8.Valid(snapshot) {
			return nil, errors.New("invalid UTF-8 snapshot")
		}
		s.disk = diskState{}
		dec := json.NewDecoder(strings.NewReader(string(snapshot)))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&s.disk); err != nil {
			return nil, fmt.Errorf("invalid snapshot: %w", err)
		}
		if err := dec.Decode(new(any)); err != io.EOF {
			return nil, errors.New("invalid trailing snapshot data")
		}
	}
	if err := s.validateDisk(); err != nil {
		return nil, err
	}
	var err error
	s.epoch, err = s.randomID("epoch_", 16)
	if err != nil {
		return nil, err
	}
	s.reindex()
	return s, nil
}
func (s *Service) validateDisk() error {
	if s.disk.FormatVersion != 1 || s.disk.Devices == nil || s.disk.Groups == nil {
		return errors.New("unsupported or incomplete snapshot")
	}
	names := map[string]bool{}
	tokens := map[string]bool{}
	codes := map[string]bool{}
	for id, g := range s.disk.Groups {
		if g == nil || id != g.ID || !validName(g.Name) || !validPurpose(g.Purpose) || len(g.CodeHash) != 64 || names[g.Name] || codes[g.CodeHash] {
			return errors.New("invalid group snapshot")
		}
		names[g.Name] = true
		codes[g.CodeHash] = true
	}
	for id, d := range s.disk.Devices {
		if d == nil || id != d.ID || !validName(d.Name) || len(d.TokenHash) != 64 || tokens[d.TokenHash] || d.Memberships == nil || d.Operations == nil || len(d.Operations) > MaxOperations {
			return errors.New("invalid identity snapshot")
		}
		tokens[d.TokenHash] = true
		if d.CurrentGroupID != "" && !d.Memberships[d.CurrentGroupID] {
			return errors.New("invalid current group in snapshot")
		}
		for gid, member := range d.Memberships {
			if !member || s.disk.Groups[gid] == nil {
				return errors.New("invalid membership in snapshot")
			}
		}
		for opid, op := range d.Operations {
			if (op.RecommendationID == "") != (op.RecommendationEpoch == "") || (op.RecommendationID != "" && (!validID(op.RecommendationID) || !validID(op.RecommendationEpoch))) {
				return errors.New("invalid recommendation operation snapshot")
			}
			if !validID(opid) || opid != op.OperationID || (op.Status != "applied" && op.Status != "rejected") {
				return errors.New("invalid operation snapshot")
			}
		}
	}
	return nil
}
func (s *Service) randomID(prefix string, n int) (string, error) {
	b := make([]byte, n)
	if _, err := io.ReadFull(s.opts.Random, b); err != nil {
		return "", fail(500, "random_failed", "cannot generate identifier")
	}
	return prefix + base64.RawURLEncoding.EncodeToString(b), nil
}
func digest(value string) string {
	sum := sha256.Sum256([]byte(value))
	return hex.EncodeToString(sum[:])
}
func validName(name string) bool {
	if !utf8.ValidString(name) || utf8.RuneCountInString(name) < 1 || utf8.RuneCountInString(name) > 32 || strings.TrimSpace(name) != name {
		return false
	}
	for _, r := range name {
		if unicode.IsControl(r) {
			return false
		}
	}
	return true
}
func validPurpose(purpose string) bool {
	if !utf8.ValidString(purpose) || utf8.RuneCountInString(purpose) > MaxPurposeRunes || strings.TrimSpace(purpose) != purpose {
		return false
	}
	for _, r := range purpose {
		if unicode.IsControl(r) {
			return false
		}
	}
	return true
}
func validID(id string) bool {
	if len(id) < 8 || len(id) > 80 {
		return false
	}
	for _, c := range id {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' || c == '_') {
			return false
		}
	}
	return true
}
func (s *Service) reindex() {
	s.tokens = map[string]string{}
	s.codes = map[string]string{}
	for id, d := range s.disk.Devices {
		s.tokens[d.TokenHash] = id
	}
	for id, g := range s.disk.Groups {
		s.codes[g.CodeHash] = id
	}
}
func (s *Service) clone() diskState {
	b, _ := json.Marshal(s.disk)
	var next diskState
	_ = json.Unmarshal(b, &next)
	return next
}
func (s *Service) commit(next diskState) error {
	b, err := json.Marshal(next)
	if err != nil {
		return fail(500, "persistence_failed", "cannot encode snapshot")
	}
	if s.saver != nil {
		if err = s.saver.Save(b); err != nil {
			return fail(503, "persistence_failed", "state was not saved; retry the same request")
		}
	}
	s.disk = next
	s.reindex()
	return nil
}
func (s *Service) identity(id string) (*deviceRecord, error) {
	d := s.disk.Devices[id]
	if d == nil {
		return nil, fail(401, "unauthorized", "valid identity required")
	}
	return d, nil
}
func (s *Service) Authenticate(token string) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if id := s.tokens[digest(token)]; token != "" && id != "" {
		return id, nil
	}
	return "", fail(401, "unauthorized", "valid Bearer token required")
}
func (s *Service) CreateDevice(name string) (Device, string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	name = strings.TrimSpace(name)
	if !validName(name) {
		return Device{}, "", fail(400, "invalid_name", "name must contain 1 to 32 Unicode characters")
	}
	id, err := s.randomID("dev_", 16)
	if err != nil {
		return Device{}, "", err
	}
	token, err := s.randomID("", 32)
	if err != nil {
		return Device{}, "", err
	}
	if s.disk.Devices[id] != nil || s.tokens[digest(token)] != "" {
		return Device{}, "", fail(500, "random_failed", "identifier collision")
	}
	d := Device{ID: id, Name: name}
	next := s.clone()
	next.Devices[id] = &deviceRecord{Device: d, TokenHash: digest(token), Memberships: map[string]bool{}, Operations: map[string]Operation{}}
	if err = s.commit(next); err != nil {
		return Device{}, "", err
	}
	return d, token, nil
}
func (s *Service) publicGroup(gid string) Group {
	g := s.disk.Groups[gid]
	out := Group{ID: g.ID, Name: g.Name, Purpose: g.Purpose}
	for _, d := range s.disk.Devices {
		if d.Memberships[gid] {
			out.MemberCount++
		}
	}
	if l := s.floors[gid]; l != nil {
		f := l.Floor
		out.Floor = &f
	}
	return out
}
func (s *Service) state(id string, now time.Time) State {
	d := s.disk.Devices[id]
	out := State{Device: d.Device, ServerEpoch: s.epoch, ServerTime: now, CurrentGroupID: d.CurrentGroupID, CurrentVersion: d.CurrentVersion, Groups: []Group{}}
	for gid := range d.Memberships {
		out.Groups = append(out.Groups, s.publicGroup(gid))
	}
	sort.Slice(out.Groups, func(i, j int) bool { return out.Groups[i].ID < out.Groups[j].ID })
	return out
}
func (s *Service) State(id string) (State, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	if _, err := s.identity(id); err != nil {
		return State{}, err
	}
	return s.state(id, now), nil
}
func (s *Service) CreateGroup(id, name string) (Group, string, State, error) {
	return s.CreateGroupWithPurpose(id, name, "")
}
func (s *Service) CreateGroupWithPurpose(id, name, purpose string) (Group, string, State, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	if _, err := s.identity(id); err != nil {
		return Group{}, "", State{}, err
	}
	name = strings.TrimSpace(name)
	if !validName(name) {
		return Group{}, "", State{}, fail(400, "invalid_name", "group name must contain 1 to 32 Unicode characters")
	}
	purpose = strings.TrimSpace(purpose)
	if !validPurpose(purpose) {
		return Group{}, "", State{}, fail(400, "invalid_purpose", "purpose must contain at most 256 Unicode characters without control characters")
	}
	for _, g := range s.disk.Groups {
		if g.Name == name {
			return Group{}, "", State{}, fail(409, "group_name_exists", "group name already exists")
		}
	}
	gid, err := s.randomID("grp_", 16)
	if err != nil {
		return Group{}, "", State{}, err
	}
	alphabet := "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
	var code string
	for attempt := 0; attempt < 10; attempt++ {
		b := make([]byte, 8)
		if _, err = io.ReadFull(s.opts.Random, b); err != nil {
			return Group{}, "", State{}, fail(500, "random_failed", "cannot generate group code")
		}
		for i := range b {
			b[i] = alphabet[int(b[i])&31]
		}
		code = string(b)
		if s.codes[digest(code)] == "" {
			break
		}
		code = ""
	}
	if code == "" || s.disk.Groups[gid] != nil {
		return Group{}, "", State{}, fail(500, "random_failed", "identifier collision")
	}
	next := s.clone()
	next.Groups[gid] = &groupRecord{ID: gid, Name: name, Purpose: purpose, CodeHash: digest(code)}
	d := next.Devices[id]
	d.Memberships[gid] = true
	if d.CurrentGroupID == "" {
		d.CurrentGroupID = gid
		d.CurrentVersion++
	}
	if err = s.commit(next); err != nil {
		return Group{}, "", State{}, err
	}
	return s.publicGroup(gid), code, s.state(id, now), nil
}
func (s *Service) Join(id, code string) (State, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	d, err := s.identity(id)
	if err != nil {
		return State{}, err
	}
	previous := s.badCodes[id]
	if len(previous) >= 5 && now.Sub(previous[len(previous)-1]) < time.Minute {
		return State{}, fail(429, "join_rate_limited", "too many incorrect group codes; wait one minute")
	}
	var recent []time.Time
	for _, t := range s.badCodes[id] {
		if now.Sub(t) < time.Minute {
			recent = append(recent, t)
		}
	}
	s.badCodes[id] = recent
	if len(recent) >= 5 {
		return State{}, fail(429, "join_rate_limited", "too many incorrect group codes; wait one minute")
	}
	code = strings.ToUpper(strings.TrimSpace(code))
	gid := s.codes[digest(code)]
	if gid == "" {
		s.badCodes[id] = append(recent, now)
		return State{}, fail(404, "invalid_code", "group code is invalid")
	}
	if d.Memberships[gid] {
		return s.state(id, now), nil
	}
	next := s.clone()
	nd := next.Devices[id]
	nd.Memberships[gid] = true
	if nd.CurrentGroupID == "" {
		nd.CurrentGroupID = gid
		nd.CurrentVersion++
	}
	if err = s.commit(next); err != nil {
		return State{}, err
	}
	return s.state(id, now), nil
}
func (s *Service) Switch(id string, req SwitchRequest) (Operation, State, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	return s.switchLocked(id, req, now, "", "", "")
}

// switchLocked commits both the original switch and a recommendation consumption
// as one durable terminal. The caller holds the sole control lock.
func (s *Service) switchLocked(id string, req SwitchRequest, now time.Time, recommendationID, recommendationEpoch, rejection string) (Operation, State, error) {
	d, err := s.identity(id)
	if err != nil {
		return Operation{}, State{}, err
	}
	if !validID(req.OperationID) || req.GroupID == "" {
		return Operation{}, State{}, fail(400, "invalid_request", "valid operation_id and group_id required")
	}
	if op, ok := d.Operations[req.OperationID]; ok {
		if op.GroupID != req.GroupID || op.ExpectedVersion != req.ExpectedVersion || op.RecommendationID != recommendationID || op.RecommendationEpoch != recommendationEpoch {
			return Operation{}, State{}, fail(409, "operation_conflict", "operation_id was used with a different payload")
		}
		return op, s.state(id, now), nil
	}
	if len(d.Operations) >= MaxOperations {
		return Operation{}, State{}, fail(429, "operation_capacity", "operation history is full")
	}
	next := s.clone()
	nd := next.Devices[id]
	op := Operation{OperationID: req.OperationID, GroupID: req.GroupID, ExpectedVersion: req.ExpectedVersion, Status: "applied", RecommendationID: recommendationID, RecommendationEpoch: recommendationEpoch}
	switch {
	case rejection != "":
		op.Status = "rejected"
		op.ErrorCode = rejection
	case req.ExpectedVersion != d.CurrentVersion:
		op.Status = "rejected"
		op.ErrorCode = "version_conflict"
	case !d.Memberships[req.GroupID]:
		op.Status = "rejected"
		op.ErrorCode = "not_member"
	case req.GroupID != d.CurrentGroupID:
		if nd.CurrentVersion == ^uint64(0) {
			return Operation{}, State{}, fail(409, "version_exhausted", "current group version exhausted")
		}
		nd.CurrentGroupID = req.GroupID
		nd.CurrentVersion++
	}
	op.CurrentGroupID = nd.CurrentGroupID
	op.CurrentVersion = nd.CurrentVersion
	nd.Operations[req.OperationID] = op
	if err = s.commit(next); err != nil {
		return Operation{}, State{}, err
	}
	if op.Status == "applied" && d.CurrentGroupID != nd.CurrentGroupID {
		s.revokeOwned(id, d.CurrentGroupID, now)
		delete(s.mediaPeers, id)
	}
	return op, s.state(id, now), nil
}
func (s *Service) Operation(id, opid string) (Operation, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	d, err := s.identity(id)
	if err != nil {
		return Operation{}, err
	}
	if !validID(opid) {
		return Operation{}, fail(400, "invalid_request", "invalid operation_id")
	}
	if op, ok := d.Operations[opid]; ok {
		return op, nil
	}
	return Operation{}, fail(404, "operation_not_found", "operation is unknown; retry the original request")
}
