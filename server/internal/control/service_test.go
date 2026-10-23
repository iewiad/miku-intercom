package control

import (
	"bytes"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"
)

type memorySaver struct {
	data  []byte
	fail  bool
	saves int
}

func (m *memorySaver) Save(b []byte) error {
	if m.fail {
		return errors.New("disk unavailable")
	}
	m.data = append([]byte(nil), b...)
	m.saves++
	return nil
}

type testClock struct {
	mu  sync.Mutex
	now time.Time
}

func (c *testClock) Now() time.Time          { c.mu.Lock(); defer c.mu.Unlock(); return c.now }
func (c *testClock) Advance(d time.Duration) { c.mu.Lock(); defer c.mu.Unlock(); c.now = c.now.Add(d) }

type fixture struct {
	s              *Service
	save           *memorySaver
	clock          *testClock
	a, b           Device
	tokenA, tokenB string
	g1, g2         Group
	code1, code2   string
}

func setup(t *testing.T) *fixture {
	t.Helper()
	f := &fixture{save: &memorySaver{}, clock: &testClock{now: time.Date(2026, 10, 23, 1, 2, 3, 0, time.UTC)}}
	var err error
	f.s, err = New(nil, f.save, Options{Now: f.clock.Now})
	must(t, err)
	f.a, f.tokenA, err = f.s.CreateDevice("甲")
	must(t, err)
	f.b, f.tokenB, err = f.s.CreateDevice("乙")
	must(t, err)
	f.g1, f.code1, _, err = f.s.CreateGroup(f.a.ID, "协作一组")
	must(t, err)
	f.g2, f.code2, _, err = f.s.CreateGroup(f.a.ID, "后勤")
	must(t, err)
	_, err = f.s.Join(f.b.ID, f.code1)
	must(t, err)
	return f
}
func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}
func wantCode(t *testing.T, err error, code string) {
	t.Helper()
	var e *Error
	if !errors.As(err, &e) || e.Code != code {
		t.Fatalf("error=%v; want %s", err, code)
	}
}
func (f *fixture) req(id, request string) AcquireRequest {
	st, _ := f.s.State(id)
	return AcquireRequest{FloorRequest: FloorRequest{GroupID: st.CurrentGroupID, CurrentVersion: st.CurrentVersion, RequestID: request, ServerEpoch: st.ServerEpoch}, ValidUntil: f.clock.Now().Add(5 * time.Second)}
}
func state(t *testing.T, s *Service, id string) State {
	t.Helper()
	out, err := s.State(id)
	must(t, err)
	return out
}
func TestIdentityNamesAndSecrets(t *testing.T) {
	f := setup(t)
	if got, err := f.s.Authenticate(f.tokenA); err != nil || got != f.a.ID {
		t.Fatalf("auth=%s %v", got, err)
	}
	_, err := f.s.Authenticate("forged")
	wantCode(t, err, "unauthorized")
	for _, name := range []string{"", strings.Repeat("中", 33), "a\nb", string([]byte{255})} {
		_, _, err = f.s.CreateDevice(name)
		wantCode(t, err, "invalid_name")
	}
	_, _, err = f.s.CreateDevice(strings.Repeat("中", 32))
	must(t, err)
	if bytes.Contains(f.save.data, []byte(f.tokenA)) || bytes.Contains(f.save.data, []byte(f.code1)) {
		t.Fatal("plaintext credential persisted")
	}
	if len(f.tokenA) != 43 {
		t.Fatalf("token length %d", len(f.tokenA))
	}
}
func TestMembershipNormalizationAndNoInterruption(t *testing.T) {
	f := setup(t)
	before := state(t, f.s, f.a.ID)
	after, err := f.s.Join(f.a.ID, "  "+strings.ToLower(f.code2)+" \n")
	must(t, err)
	if after.CurrentGroupID != f.g1.ID || after.CurrentVersion != before.CurrentVersion || len(after.Groups) != 2 {
		t.Fatal("second/duplicate join interrupted current group")
	}
	after, err = f.s.Join(f.b.ID, strings.ToLower(f.code2))
	must(t, err)
	if after.CurrentGroupID != f.g1.ID || after.CurrentVersion != 1 || len(after.Groups) != 2 {
		t.Fatal("joining second group changed selection")
	}
	for _, g := range after.Groups {
		if g.MemberCount != 2 {
			t.Fatalf("member_count=%d", g.MemberCount)
		}
	}
	_, _, _, err = f.s.CreateGroup(f.b.ID, f.g1.Name)
	wantCode(t, err, "group_name_exists")
	_, err = f.s.Acquire(f.b.ID, AcquireRequest{FloorRequest: FloorRequest{GroupID: f.g2.ID, CurrentVersion: 1, RequestID: "crossgrp1", ServerEpoch: before.ServerEpoch}, ValidUntil: f.clock.Now().Add(time.Second)})
	wantCode(t, err, "version_conflict")
}
func TestBadCodeRateLimit(t *testing.T) {
	f := setup(t)
	for i := 0; i < 5; i++ {
		if i == 4 {
			f.clock.Advance(30 * time.Second)
		}
		_, err := f.s.Join(f.b.ID, "WRONG000")
		wantCode(t, err, "invalid_code")
	}
	_, err := f.s.Join(f.b.ID, f.code2)
	wantCode(t, err, "join_rate_limited")
	f.clock.Advance(31 * time.Second)
	_, err = f.s.Join(f.b.ID, "WRONG001")
	wantCode(t, err, "join_rate_limited")
	f.clock.Advance(29 * time.Second)
	_, err = f.s.Join(f.b.ID, f.code2)
	must(t, err)
}
func TestSwitchTerminalReplayAndVersion(t *testing.T) {
	f := setup(t)
	req := SwitchRequest{OperationID: "switch001", GroupID: f.g2.ID, ExpectedVersion: 1}
	op, st, err := f.s.Switch(f.a.ID, req)
	must(t, err)
	if op.Status != "applied" || st.CurrentVersion != 2 || st.CurrentGroupID != f.g2.ID {
		t.Fatal(op, st)
	}
	_, _, err = f.s.Switch(f.a.ID, SwitchRequest{OperationID: req.OperationID, GroupID: f.g1.ID, ExpectedVersion: 1})
	wantCode(t, err, "operation_conflict")
	_, _, err = f.s.Switch(f.a.ID, SwitchRequest{OperationID: "switch002", GroupID: f.g1.ID, ExpectedVersion: 2})
	must(t, err)
	replay, fresh, err := f.s.Switch(f.a.ID, req)
	must(t, err)
	if replay != op || fresh.CurrentVersion != 3 || fresh.CurrentGroupID != f.g1.ID {
		t.Fatal("immutable result/fresh state violated")
	}
	op, _, err = f.s.Switch(f.a.ID, SwitchRequest{OperationID: "switch003", GroupID: f.g2.ID, ExpectedVersion: 1})
	must(t, err)
	if op.Status != "rejected" || op.ErrorCode != "version_conflict" {
		t.Fatal(op)
	}
	op, st, err = f.s.Switch(f.a.ID, SwitchRequest{OperationID: "switch004", GroupID: f.g1.ID, ExpectedVersion: 3})
	must(t, err)
	if op.Status != "applied" || st.CurrentVersion != 3 {
		t.Fatal("same group not no-op")
	}
	op, _, err = f.s.Switch(f.b.ID, SwitchRequest{OperationID: "switch005", GroupID: f.g2.ID, ExpectedVersion: 1})
	must(t, err)
	if op.ErrorCode != "not_member" || state(t, f.s, f.b.ID).CurrentGroupID != f.g1.ID {
		t.Fatal(op)
	}
	_, err = f.s.Operation(f.b.ID, "switch001")
	wantCode(t, err, "operation_not_found")
}
func TestPersistenceFailureKeepsSelectionLeaseAndOperationUnknown(t *testing.T) {
	f := setup(t)
	req := f.req(f.a.ID, "press0001")
	floor, err := f.s.Acquire(f.a.ID, req)
	must(t, err)
	before := append([]byte(nil), f.save.data...)
	f.save.fail = true
	switchReq := SwitchRequest{OperationID: "switch001", GroupID: f.g2.ID, ExpectedVersion: 1}
	_, _, err = f.s.Switch(f.a.ID, switchReq)
	wantCode(t, err, "persistence_failed")
	got := state(t, f.s, f.a.ID)
	if got.CurrentGroupID != f.g1.ID || got.CurrentVersion != 1 {
		t.Fatal("uncommitted selection published")
	}
	var held *Floor
	for _, g := range got.Groups {
		if g.ID == f.g1.ID {
			held = g.Floor
		}
	}
	if held == nil || held.LeaseID != floor.LeaseID {
		t.Fatal("failed save released floor")
	}
	if !bytes.Equal(before, f.save.data) {
		t.Fatal("failed save changed durable state")
	}
	_, err = f.s.Operation(f.a.ID, switchReq.OperationID)
	wantCode(t, err, "operation_not_found")
	_, _, _, err = f.s.CreateGroup(f.a.ID, "failed group")
	wantCode(t, err, "persistence_failed")
	if len(state(t, f.s, f.a.ID).Groups) != 2 {
		t.Fatal("failed create published")
	}
	_, _, err = f.s.CreateDevice("failed identity")
	wantCode(t, err, "persistence_failed")
	f.save.fail = false
	_, _, err = f.s.Switch(f.a.ID, switchReq)
	must(t, err)
	if f.s.floors[f.g1.ID] != nil {
		t.Fatal("successful switch retained old floor")
	}
}
func TestRestartKeepsOperationsAndInvalidatesFloors(t *testing.T) {
	f := setup(t)
	req := f.req(f.a.ID, "press0001")
	_, err := f.s.Acquire(f.a.ID, req)
	must(t, err)
	op, _, err := f.s.Switch(f.a.ID, SwitchRequest{OperationID: "noop0001", GroupID: f.g1.ID, ExpectedVersion: 1})
	must(t, err)
	restart, err := New(f.save.data, f.save, Options{Now: f.clock.Now})
	must(t, err)
	got := state(t, restart, f.a.ID)
	if got.CurrentVersion != 1 || got.ServerEpoch == req.ServerEpoch {
		t.Fatal("restart fencing missing")
	}
	for _, g := range got.Groups {
		if g.Floor != nil {
			t.Fatal("floor survived restart")
		}
	}
	gotOp, err := restart.Operation(f.a.ID, "noop0001")
	must(t, err)
	if op != gotOp {
		t.Fatal("operation changed after restart")
	}
	_, err = restart.Acquire(f.a.ID, req)
	wantCode(t, err, "epoch_conflict")
	err = restart.Release(f.a.ID, req.FloorRequest)
	wantCode(t, err, "epoch_conflict")
	_, err = restart.Renew(f.a.ID, RenewRequest{FloorRequest: req.FloorRequest, LeaseID: "old"})
	wantCode(t, err, "epoch_conflict")
	_, err = restart.Authenticate(f.tokenA)
	must(t, err)
}
func TestConcurrentAcquireExactlyOne(t *testing.T) {
	f := setup(t)
	start := make(chan struct{})
	results := make(chan error, 2)
	for i, id := range []string{f.a.ID, f.b.ID} {
		req := f.req(id, fmt.Sprintf("press%04d", i))
		go func(id string, req AcquireRequest) { <-start; _, err := f.s.Acquire(id, req); results <- err }(id, req)
	}
	close(start)
	won := 0
	for i := 0; i < 2; i++ {
		if err := <-results; err == nil {
			won++
		} else {
			wantCode(t, err, "floor_busy")
		}
	}
	if won != 1 {
		t.Fatalf("winners=%d", won)
	}
}
func TestReleaseBeforeAcquireAndOldRelease(t *testing.T) {
	f := setup(t)
	req := f.req(f.a.ID, "press0001")
	must(t, f.s.Release(f.a.ID, req.FloorRequest))
	_, err := f.s.Acquire(f.a.ID, req)
	wantCode(t, err, "request_ended")
	next := f.req(f.a.ID, "press0002")
	floor, err := f.s.Acquire(f.a.ID, next)
	must(t, err)
	must(t, f.s.Release(f.a.ID, req.FloorRequest))
	again, err := f.s.Acquire(f.a.ID, next)
	must(t, err)
	if again != floor {
		t.Fatal("old release changed new lease")
	}
	_, err = f.s.Renew(f.a.ID, RenewRequest{FloorRequest: req.FloorRequest, LeaseID: floor.LeaseID})
	wantCode(t, err, "lease_not_held")
}
func TestBusyCannotReviveAndAcquireNeverRenews(t *testing.T) {
	f := setup(t)
	first := f.req(f.a.ID, "press0001")
	granted, err := f.s.Acquire(f.a.ID, first)
	must(t, err)
	busy := f.req(f.b.ID, "press0002")
	_, err = f.s.Acquire(f.b.ID, busy)
	wantCode(t, err, "floor_busy")
	f.clock.Advance(time.Second)
	duplicate, err := f.s.Acquire(f.a.ID, first)
	must(t, err)
	if duplicate.ExpiresAt != granted.ExpiresAt {
		t.Fatal("acquire renewed lease")
	}
	must(t, f.s.Release(f.a.ID, first.FloorRequest))
	_, err = f.s.Acquire(f.b.ID, busy)
	wantCode(t, err, "request_ended")
}
func TestRenewExpiryAndDeadlines(t *testing.T) {
	f := setup(t)
	req := f.req(f.a.ID, "press0001")
	floor, err := f.s.Acquire(f.a.ID, req)
	must(t, err)
	f.clock.Advance(2 * time.Second)
	renewed, err := f.s.Renew(f.a.ID, RenewRequest{FloorRequest: req.FloorRequest, LeaseID: floor.LeaseID})
	must(t, err)
	if !renewed.ExpiresAt.Equal(f.clock.Now().Add(3 * time.Second)) {
		t.Fatal("bad renewal")
	}
	changed := req
	changed.ValidUntil = changed.ValidUntil.Add(time.Second)
	_, err = f.s.Acquire(f.a.ID, changed)
	wantCode(t, err, "request_conflict")
	f.clock.Advance(3 * time.Second)
	_, err = f.s.Renew(f.a.ID, RenewRequest{FloorRequest: req.FloorRequest, LeaseID: floor.LeaseID})
	wantCode(t, err, "lease_not_held")
	for _, g := range state(t, f.s, f.a.ID).Groups {
		if g.Floor != nil {
			t.Fatal("expired lease visible")
		}
	}
	expired := f.req(f.a.ID, "expired01")
	expired.ValidUntil = f.clock.Now()
	_, err = f.s.Acquire(f.a.ID, expired)
	wantCode(t, err, "request_expired")
	expired.ValidUntil = f.clock.Now().Add(time.Second)
	_, err = f.s.Acquire(f.a.ID, expired)
	wantCode(t, err, "request_conflict")
	future := f.req(f.a.ID, "future001")
	future.ValidUntil = f.clock.Now().Add(10*time.Second + time.Nanosecond)
	_, err = f.s.Acquire(f.a.ID, future)
	wantCode(t, err, "invalid_deadline")
	zero := f.req(f.a.ID, "zero00001")
	zero.ValidUntil = time.Time{}
	_, err = f.s.Acquire(f.a.ID, zero)
	wantCode(t, err, "invalid_deadline")
}
func TestOldRequestsAfterProtectionRemainExpired(t *testing.T) {
	f := setup(t)
	req := f.req(f.a.ID, "press0001")
	must(t, f.s.Release(f.a.ID, req.FloorRequest))
	f.clock.Advance(61 * time.Second)
	_, err := f.s.Acquire(f.a.ID, req)
	wantCode(t, err, "request_expired")
}
func TestRequestCapacityDoesNotEvictProtectedRecords(t *testing.T) {
	f := setup(t)
	for i := 0; i < MaxRequests; i++ {
		req := f.req(f.a.ID, fmt.Sprintf("press%04d", i))
		must(t, f.s.Release(f.a.ID, req.FloorRequest))
	}
	req := f.req(f.a.ID, "overflow1")
	_, err := f.s.Acquire(f.a.ID, req)
	wantCode(t, err, "request_capacity")
	canceled := f.req(f.a.ID, "press0000")
	_, err = f.s.Acquire(f.a.ID, canceled)
	wantCode(t, err, "request_ended")
	f.clock.Advance(61 * time.Second)
	req = f.req(f.a.ID, "overflow1")
	_, err = f.s.Acquire(f.a.ID, req)
	must(t, err)
}
func TestOperationCapacityPreservesReplay(t *testing.T) {
	f := setup(t)
	original, _, err := f.s.Switch(f.a.ID, SwitchRequest{OperationID: "original1", GroupID: f.g1.ID, ExpectedVersion: 1})
	must(t, err)
	// Populate the boundary without repeatedly serializing an ever-growing fixture.
	for i := 1; i < MaxOperations; i++ {
		id := fmt.Sprintf("filled%04d", i)
		f.s.disk.Devices[f.a.ID].Operations[id] = Operation{OperationID: id, GroupID: f.g1.ID, ExpectedVersion: 1, Status: "applied", CurrentGroupID: f.g1.ID, CurrentVersion: 1}
	}
	_, _, err = f.s.Switch(f.a.ID, SwitchRequest{OperationID: "overflow1", GroupID: f.g2.ID, ExpectedVersion: 1})
	wantCode(t, err, "operation_capacity")
	replay, _, err := f.s.Switch(f.a.ID, SwitchRequest{OperationID: "original1", GroupID: f.g1.ID, ExpectedVersion: 1})
	must(t, err)
	if replay != original {
		t.Fatal("capacity evicted replay")
	}
}
func TestCorruptSnapshotsAndRandomFailure(t *testing.T) {
	for _, raw := range []string{"not json", "{}", `{"format_version":2,"devices":{},"groups":{}}`, `{"format_version":1,"devices":{"x":null},"groups":{}}`} {
		if _, err := New([]byte(raw), nil, Options{}); err == nil {
			t.Fatalf("accepted snapshot %s", raw)
		}
	}
	if _, err := New(nil, nil, Options{Random: strings.NewReader("")}); err == nil {
		t.Fatal("ignored random failure")
	}
}

func TestFailedJoinDoesNotPublishMembership(t *testing.T) {
	f := setup(t)
	f.save.fail = true
	_, err := f.s.Join(f.b.ID, f.code2)
	wantCode(t, err, "persistence_failed")
	st := state(t, f.s, f.b.ID)
	if len(st.Groups) != 1 || st.CurrentVersion != 1 {
		t.Fatal("failed join published membership")
	}
	f.save.fail = false
	_, err = f.s.Join(f.b.ID, f.code2)
	must(t, err)
}
func TestSwitchFencesOldGroupRequests(t *testing.T) {
	f := setup(t)
	old := f.req(f.a.ID, "oldpress1")
	_, err := f.s.Acquire(f.a.ID, old)
	must(t, err)
	_, _, err = f.s.Switch(f.a.ID, SwitchRequest{OperationID: "switch001", GroupID: f.g2.ID, ExpectedVersion: 1})
	must(t, err)
	current := f.req(f.a.ID, "newpress1")
	floor, err := f.s.Acquire(f.a.ID, current)
	must(t, err)
	wantCode(t, f.s.Release(f.a.ID, old.FloorRequest), "version_conflict")
	_, err = f.s.Acquire(f.a.ID, old)
	wantCode(t, err, "version_conflict")
	if got := f.s.floors[f.g2.ID]; got == nil || got.LeaseID != floor.LeaseID {
		t.Fatal("old group request disturbed current lease")
	}
	restarted, err := New(f.save.data, f.save, Options{Now: f.clock.Now})
	must(t, err)
	st := state(t, restarted, f.a.ID)
	if st.CurrentGroupID != f.g2.ID || st.CurrentVersion != 2 {
		t.Fatal("selection/version lost at restart")
	}
}
