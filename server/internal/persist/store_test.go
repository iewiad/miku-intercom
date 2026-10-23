package persist

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestRoundTripPermissionsAndExclusiveLock(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "private")
	path := filepath.Join(dir, "state.json")
	f, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = Open(path); err == nil {
		t.Fatal("second writer acquired lock")
	}
	b, err := f.Load()
	if err != nil || b != nil {
		t.Fatal("new snapshot", err)
	}
	for _, data := range [][]byte{[]byte(`{"first":1}`), []byte(`{"second":2}`)} {
		if err = f.Save(data); err != nil {
			t.Fatal(err)
		}
		b, err = f.Load()
		if err != nil || !bytes.Equal(data, b) {
			t.Fatal("round trip", err)
		}
	}
	for p, want := range map[string]os.FileMode{dir: 0700, path: 0600} {
		info, err := os.Stat(p)
		if err != nil || info.Mode().Perm() != want {
			t.Fatalf("%s mode %v: %v", p, info, err)
		}
	}
	if err = f.Close(); err != nil {
		t.Fatal(err)
	}
	f, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if err = f.Close(); err != nil {
		t.Fatal(err)
	}
	if err = f.Save(nil); err == nil {
		t.Fatal("closed store accepted write")
	}
}
func TestRejectsUnsafeExistingParentWithoutChmod(t *testing.T) {
	dir := t.TempDir()
	if err := os.Chmod(dir, 0755); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(filepath.Join(dir, "state.json")); err == nil {
		t.Fatal("accepted public directory")
	}
	info, _ := os.Stat(dir)
	if info.Mode().Perm() != 0755 {
		t.Fatal("modified existing parent permissions")
	}
}
func TestStaleLockIsNotStolen(t *testing.T) {
	dir := t.TempDir()
	if err := os.Chmod(dir, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "state.json")
	if err := os.Mkdir(path+".lock", 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(path+".lock", "owner"), []byte("pid=99999999\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(path); err == nil {
		t.Fatal("stale lock silently removed")
	}
	if _, err := os.Stat(path + ".lock"); err != nil {
		t.Fatal("lock removed")
	}
}
func TestFailedAtomicReplacePreservesCommittedSnapshot(t *testing.T) {
	dir := t.TempDir()
	if err := os.Chmod(dir, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "state.json")
	f, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	old := []byte("committed")
	if err = f.Save(old); err != nil {
		t.Fatal(err)
	}
	// Moving the containing directory makes every write through the old path fail.
	moved := dir + "-moved"
	if err = os.Rename(dir, moved); err != nil {
		t.Fatal(err)
	}
	defer os.Rename(moved, dir)
	if err = f.Save([]byte("uncommitted")); err == nil {
		t.Fatal("write unexpectedly succeeded")
	}
	got, err := os.ReadFile(filepath.Join(moved, "state.json"))
	if err != nil || !bytes.Equal(got, old) {
		t.Fatal("failed write lost committed snapshot", err)
	}
}
func TestRefusesSymlinkSnapshot(t *testing.T) {
	dir := t.TempDir()
	if err := os.Chmod(dir, 0700); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(dir, "real")
	if err := os.WriteFile(target, []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(dir, "state.json")
	if err := os.Symlink(target, link); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(link); err == nil {
		t.Fatal("accepted symlink snapshot")
	}
}
