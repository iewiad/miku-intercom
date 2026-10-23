// Package persist provides an exclusive, atomic snapshot store for one process.
package persist

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sync"
)

type File struct {
	mu         sync.Mutex
	path, lock string
	closed     bool
}

func Open(path string) (*File, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	dir := filepath.Dir(abs)
	if err = os.MkdirAll(dir, 0700); err != nil {
		return nil, fmt.Errorf("create data directory: %w", err)
	}
	info, err := os.Lstat(dir)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() || info.Mode().Perm()&0077 != 0 {
		return nil, fmt.Errorf("data directory must be a private directory (0700): %s", dir)
	}
	if info, err = os.Lstat(abs); err == nil {
		if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
			return nil, errors.New("snapshot must be a private regular file (0600)")
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	lock := abs + ".lock"
	if err = os.Mkdir(lock, 0700); err != nil {
		return nil, fmt.Errorf("cannot lock snapshot %s; another process or an unclean shutdown may own %s (verify manually before removing): %w", abs, lock, err)
	}
	if err = os.WriteFile(filepath.Join(lock, "owner"), []byte(fmt.Sprintf("pid=%d\n", os.Getpid())), 0600); err != nil {
		_ = os.Remove(lock)
		return nil, err
	}
	return &File{path: abs, lock: lock}, nil
}
func (f *File) Load() ([]byte, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return nil, errors.New("snapshot store is closed")
	}
	b, err := os.ReadFile(f.path)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	return b, err
}
func (f *File) Save(data []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return errors.New("snapshot store is closed")
	}
	file, err := os.CreateTemp(filepath.Dir(f.path), ".state-*")
	if err != nil {
		return err
	}
	tmp := file.Name()
	defer os.Remove(tmp)
	if err = file.Chmod(0600); err == nil {
		_, err = file.Write(data)
	}
	if err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	// Rename is the commit point. Never report failure after replacing the snapshot.
	return os.Rename(tmp, f.path)
}
func (f *File) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return nil
	}
	f.closed = true
	if err := os.Remove(filepath.Join(f.lock, "owner")); err != nil {
		return err
	}
	return os.Remove(f.lock)
}
