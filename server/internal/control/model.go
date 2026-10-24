package control

import (
	"crypto/rand"
	"io"
	"time"
)

type Device struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}
type Floor struct {
	PeerID     string    `json:"peer_id,omitempty"`
	DeviceID   string    `json:"device_id"`
	DeviceName string    `json:"device_name"`
	RequestID  string    `json:"request_id"`
	LeaseID    string    `json:"lease_id"`
	ExpiresAt  time.Time `json:"expires_at"`
}
type Group struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Purpose     string `json:"purpose,omitempty"`
	MemberCount int    `json:"member_count"`
	Floor       *Floor `json:"floor"`
}
type State struct {
	Device         Device    `json:"device"`
	ServerEpoch    string    `json:"server_epoch"`
	ServerTime     time.Time `json:"server_time"`
	CurrentGroupID string    `json:"current_group_id"`
	CurrentVersion uint64    `json:"current_version"`
	Groups         []Group   `json:"groups"`
	MediaReady     bool      `json:"media_ready"`
}
type Operation struct {
	OperationID     string `json:"operation_id"`
	GroupID         string `json:"group_id"`
	ExpectedVersion uint64 `json:"expected_version"`
	Status          string `json:"status"`
	ErrorCode       string `json:"error_code"`
	CurrentGroupID  string `json:"current_group_id"`
	CurrentVersion  uint64 `json:"current_version"`
}
type SwitchRequest struct {
	OperationID     string `json:"operation_id"`
	GroupID         string `json:"group_id"`
	ExpectedVersion uint64 `json:"expected_version"`
}
type FloorRequest struct {
	PeerID         string `json:"peer_id,omitempty"`
	GroupID        string `json:"group_id"`
	CurrentVersion uint64 `json:"current_version"`
	RequestID      string `json:"request_id"`
	ServerEpoch    string `json:"server_epoch"`
}
type AcquireRequest struct {
	FloorRequest
	ValidUntil time.Time `json:"valid_until"`
}
type RenewRequest struct {
	FloorRequest
	LeaseID string `json:"lease_id"`
}
type Saver interface{ Save([]byte) error }
type Options struct {
	Now    func() time.Time
	Random io.Reader
}

func (o *Options) defaults() {
	if o.Now == nil {
		o.Now = time.Now
	}
	if o.Random == nil {
		o.Random = rand.Reader
	}
}

type Error struct {
	Code, Message string
	Status        int
}

func (e *Error) Error() string                { return e.Code + ": " + e.Message }
func fail(status int, code, msg string) error { return &Error{code, msg, status} }

type deviceRecord struct {
	Device
	TokenHash      string               `json:"token_hash"`
	Memberships    map[string]bool      `json:"memberships"`
	CurrentGroupID string               `json:"current_group_id"`
	CurrentVersion uint64               `json:"current_version"`
	Operations     map[string]Operation `json:"operations"`
}
type groupRecord struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Purpose  string `json:"purpose,omitempty"`
	CodeHash string `json:"code_hash"`
}
type diskState struct {
	FormatVersion int                      `json:"format_version"`
	Devices       map[string]*deviceRecord `json:"devices"`
	Groups        map[string]*groupRecord  `json:"groups"`
}
type lease struct {
	Floor
	Version uint64
}
type requestRecord struct {
	PeerID         string
	Status         string
	ValidUntil     time.Time
	ProtectedUntil time.Time
	LeaseID        string
}
