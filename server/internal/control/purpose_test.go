package control

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
)

func TestCreateGroupPurposeValidation(t *testing.T) {
	for _, tc := range []struct {
		name, input, want string
		invalid           bool
	}{
		{name: "omitted-compatible"},
		{name: "whitespace", input: " \t\n\u2003 "},
		{name: "trimmed", input: "\n 设备排障与维修\u2003", want: "设备排障与维修"},
		{name: "ascii-limit", input: strings.Repeat("a", 256), want: strings.Repeat("a", 256)},
		{name: "multibyte-limit", input: strings.Repeat("修", 256), want: strings.Repeat("修", 256)},
		{name: "ascii-over-limit", input: strings.Repeat("a", 257), invalid: true},
		{name: "multibyte-over-limit", input: strings.Repeat("修", 257), invalid: true},
		{name: "invalid-utf8", input: string([]byte{0xff}), invalid: true},
		{name: "internal-newline", input: "设备\n维修", invalid: true},
		{name: "internal-tab", input: "设备\t维修", invalid: true},
		{name: "nul", input: "设备\x00维修", invalid: true},
		{name: "delete", input: "设备\x7f维修", invalid: true},
		{name: "unicode-control", input: "设备\u0085维修", invalid: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := setup(t)
			before := append([]byte(nil), f.save.data...)
			g, _, st, err := f.s.CreateGroupWithPurpose(f.a.ID, "维修组", tc.input)
			if tc.invalid {
				wantCode(t, err, "invalid_purpose")
				if !bytes.Equal(before, f.save.data) || len(state(t, f.s, f.a.ID).Groups) != 2 {
					t.Fatal("invalid purpose changed saved or published groups")
				}
				return
			}
			must(t, err)
			if g.Purpose != tc.want || purposeGroup(t, st, g.ID).Purpose != tc.want {
				t.Fatalf("purpose=%q state=%q want=%q", g.Purpose, purposeGroup(t, st, g.ID).Purpose, tc.want)
			}
		})
	}
}

func purposeGroup(t *testing.T, st State, id string) Group {
	t.Helper()
	for _, g := range st.Groups {
		if g.ID == id {
			return g
		}
	}
	t.Fatal("group missing from authorized state")
	return Group{}
}

func TestGroupPurposeOldAndNewSnapshots(t *testing.T) {
	f := setup(t)
	legacy := append([]byte(nil), f.save.data...)
	if bytes.Contains(legacy, []byte(`"purpose"`)) {
		t.Fatal("legacy two-argument creation must omit empty purpose")
	}
	restarted, err := New(legacy, f.save, Options{Now: f.clock.Now})
	must(t, err)
	if g := purposeGroup(t, state(t, restarted, f.a.ID), f.g1.ID); g.Purpose != "" {
		t.Fatal("legacy snapshot gained a purpose")
	}
	g, _, _, err := restarted.CreateGroupWithPurpose(f.a.ID, "维修组", "设备维修")
	must(t, err)
	if !bytes.Contains(f.save.data, []byte(`"purpose":"设备维修"`)) {
		t.Fatal("purpose missing from saved snapshot")
	}
	saved := append([]byte(nil), f.save.data...)
	saves := f.save.saves
	restarted, err = New(saved, f.save, Options{Now: f.clock.Now})
	must(t, err)
	st := state(t, restarted, f.a.ID)
	if purposeGroup(t, st, g.ID).Purpose != "设备维修" || purposeGroup(t, st, f.g1.ID).Purpose != "" {
		t.Fatal("new and legacy purposes did not survive restart")
	}
	if f.save.saves != saves || !bytes.Equal(f.save.data, saved) {
		t.Fatal("loading rewrote the snapshot")
	}
}

func TestGroupPurposeRejectsBadSnapshotsWithoutRewriting(t *testing.T) {
	f := setup(t)
	for _, bad := range []string{" padded ", strings.Repeat("修", 257), "a\nb", "a\x00b", "a\u0085b"} {
		var disk diskState
		must(t, json.Unmarshal(f.save.data, &disk))
		disk.Groups[f.g1.ID].Purpose = bad
		snapshot, err := json.Marshal(disk)
		must(t, err)
		t.Run(bad, func(t *testing.T) {
			saves := f.save.saves
			before := append([]byte(nil), f.save.data...)
			if _, err := New(snapshot, f.save, Options{}); err == nil {
				t.Fatal("invalid snapshot purpose accepted")
			}
			if f.save.saves != saves || !bytes.Equal(before, f.save.data) {
				t.Fatal("invalid snapshot was rewritten")
			}
		})
	}
	// encoding/json otherwise repairs raw invalid UTF-8 before field validation.
	var disk diskState
	must(t, json.Unmarshal(f.save.data, &disk))
	disk.Groups[f.g1.ID].Purpose = "marker"
	snapshot, err := json.Marshal(disk)
	must(t, err)
	snapshot = bytes.Replace(snapshot, []byte(`"purpose":"marker"`), []byte{'"', 'p', 'u', 'r', 'p', 'o', 's', 'e', '"', ':', '"', 0xff, '"'}, 1)
	if _, err := New(snapshot, f.save, Options{}); err == nil {
		t.Fatal("raw invalid UTF-8 snapshot was silently repaired")
	}
}

func TestGroupPurposeCreationAtomicityAndMembershipScope(t *testing.T) {
	f := setup(t)
	floor, err := f.s.Acquire(f.a.ID, f.req(f.a.ID, "purpose_floor"))
	must(t, err)
	beforeState := state(t, f.s, f.a.ID)
	beforeDisk := append([]byte(nil), f.save.data...)
	f.save.fail = true
	_, _, _, err = f.s.CreateGroupWithPurpose(f.a.ID, "维修组", "设备维修")
	wantCode(t, err, "persistence_failed")
	failedState := state(t, f.s, f.a.ID)
	if !bytes.Equal(beforeDisk, f.save.data) || len(failedState.Groups) != len(beforeState.Groups) {
		t.Fatal("failed save published the group")
	}
	f.save.fail = false
	g, code, afterState, err := f.s.CreateGroupWithPurpose(f.a.ID, "维修组", "设备维修")
	must(t, err)
	for _, st := range []State{failedState, afterState} {
		current := purposeGroup(t, st, f.g1.ID)
		if st.CurrentGroupID != beforeState.CurrentGroupID || st.CurrentVersion != beforeState.CurrentVersion || current.Floor == nil || current.Floor.LeaseID != floor.LeaseID {
			t.Fatal("creating another group changed current group, version or floor")
		}
	}
	for _, other := range state(t, f.s, f.b.ID).Groups {
		if other.ID == g.ID || other.Purpose == "设备维修" {
			t.Fatal("purpose leaked to a non-member")
		}
	}
	joined, err := f.s.Join(f.b.ID, code)
	must(t, err)
	if purposeGroup(t, joined, g.ID).Purpose != "设备维修" || joined.CurrentGroupID != f.g1.ID || joined.CurrentVersion != 1 {
		t.Fatal("joining failed to expose purpose or changed existing current group")
	}
}
