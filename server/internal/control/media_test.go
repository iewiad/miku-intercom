package control

import "testing"

func bind(t *testing.T, f *fixture, id, peer string, ready bool) {
	t.Helper()
	st := state(t, f.s, id)
	_, err := f.s.BindMediaPeer(id, peer, "", st.CurrentGroupID, st.CurrentVersion, st.ServerEpoch)
	must(t, err)
	if ready && !f.s.SetMediaReady(id, peer) {
		t.Fatal("cannot mark current Peer ready")
	}
}
func TestMediaBindingReadyLeaseAndReplacement(t *testing.T) {
	f := setup(t)
	bind(t, f, f.a.ID, "peer_a001", false)
	bind(t, f, f.b.ID, "peer_b001", true)
	req := f.req(f.a.ID, "press0001")
	req.PeerID = "peer_a001"
	_, err := f.s.Acquire(f.a.ID, req)
	wantCode(t, err, "peer_not_ready")
	if !f.s.SetMediaReady(f.a.ID, "peer_a001") {
		t.Fatal("cannot mark ready")
	}
	legacy := f.req(f.a.ID, "legacy001")
	_, err = f.s.Acquire(f.a.ID, legacy)
	must(t, err)
	if _, ok := f.s.AuthorizeMedia(f.a.ID, "peer_a001", f.b.ID, "peer_b001"); ok {
		t.Fatal("legacy control lease authorized audio")
	}
	must(t, f.s.Release(f.a.ID, legacy.FloorRequest))
	_, err = f.s.Acquire(f.a.ID, req)
	must(t, err)
	permit, ok := f.s.AuthorizeMedia(f.a.ID, "peer_a001", f.b.ID, "peer_b001")
	if !ok || !f.s.ValidateMedia(permit) {
		t.Fatal("authorized media denied")
	}
	st := state(t, f.s, f.a.ID)
	old, err := f.s.BindMediaPeer(f.a.ID, "peer_a002", "peer_a001", st.CurrentGroupID, st.CurrentVersion, st.ServerEpoch)
	must(t, err)
	if old != "peer_a001" || f.s.ValidateMedia(permit) {
		t.Fatal("replacement retained old permit")
	}
	if f.s.SetMediaReady(f.a.ID, "peer_a001") {
		t.Fatal("old callback made replacement ready")
	}
	if !f.s.SetMediaReady(f.a.ID, "peer_a002") {
		t.Fatal("replacement not ready")
	}
	next := f.req(f.a.ID, "press0002")
	next.PeerID = "peer_a002"
	_, err = f.s.Acquire(f.a.ID, next)
	must(t, err)
	f.s.UnbindMediaPeer(f.a.ID, "peer_a001")
	if _, ok = f.s.AuthorizeMedia(f.a.ID, "peer_a002", f.b.ID, "peer_b001"); !ok {
		t.Fatal("old close callback revoked new lease")
	}
}
func TestFailedSwitchKeepsMediaBindingThenSuccessInvalidates(t *testing.T) {
	f := setup(t)
	bind(t, f, f.a.ID, "peer_a001", true)
	bind(t, f, f.b.ID, "peer_b001", true)
	req := f.req(f.a.ID, "press0001")
	req.PeerID = "peer_a001"
	_, err := f.s.Acquire(f.a.ID, req)
	must(t, err)
	permit, ok := f.s.AuthorizeMedia(f.a.ID, "peer_a001", f.b.ID, "peer_b001")
	if !ok {
		t.Fatal("permit missing")
	}
	f.save.fail = true
	op := SwitchRequest{OperationID: "switch001", GroupID: f.g2.ID, ExpectedVersion: 1}
	_, _, err = f.s.Switch(f.a.ID, op)
	wantCode(t, err, "persistence_failed")
	if !f.s.MediaPeerValid(f.a.ID, "peer_a001") || !f.s.ValidateMedia(permit) {
		t.Fatal("failed save invalidated media")
	}
	f.save.fail = false
	_, _, err = f.s.Switch(f.a.ID, op)
	must(t, err)
	if f.s.MediaPeerValid(f.a.ID, "peer_a001") || f.s.ValidateMedia(permit) {
		t.Fatal("successful switch retained old media")
	}
}
func TestMediaBindingAuthorizationAndVersion(t *testing.T) {
	f := setup(t)
	st := state(t, f.s, f.b.ID)
	_, err := f.s.BindMediaPeer(f.b.ID, "peer_b001", "", f.g2.ID, 1, st.ServerEpoch)
	wantCode(t, err, "not_member")
	_, err = f.s.BindMediaPeer(f.b.ID, "peer_b001", "", f.g1.ID, 0, st.ServerEpoch)
	wantCode(t, err, "version_conflict")
	_, err = f.s.BindMediaPeer(f.b.ID, "peer_b001", "", f.g1.ID, 1, "old_epoch")
	wantCode(t, err, "epoch_conflict")
	bind(t, f, f.b.ID, "peer_b001", true)
	_, err = f.s.BindMediaPeer(f.b.ID, "peer_b002", "stale001", f.g1.ID, 1, st.ServerEpoch)
	wantCode(t, err, "replace_conflict")
}
