package control

import (
	"strconv"
	"time"
)

func requestKey(req FloorRequest) string {
	return req.GroupID + "/" + strconv.FormatUint(req.CurrentVersion, 10) + "/" + req.RequestID
}
func (s *Service) expire(now time.Time) {
	for gid, l := range s.floors {
		if !now.Before(l.ExpiresAt) {
			s.revokeOwned(l.DeviceID, gid, now)
		}
	}
	for id, records := range s.requests {
		for key, r := range records {
			if r.Status != "granted" && !now.Before(r.ProtectedUntil) {
				delete(records, key)
			}
		}
		if len(records) == 0 {
			delete(s.requests, id)
		}
	}
}
func (s *Service) revokeOwned(id, gid string, now time.Time) {
	l := s.floors[gid]
	if l == nil || l.DeviceID != id {
		return
	}
	key := requestKey(FloorRequest{GroupID: gid, CurrentVersion: l.Version, RequestID: l.RequestID})
	if r := s.requests[id][key]; r != nil {
		r.Status = "ended"
		r.ProtectedUntil = now.Add(RequestProtection)
	}
	delete(s.floors, gid)
}
func (s *Service) validateFloor(id string, req FloorRequest) (*deviceRecord, error) {
	d, err := s.identity(id)
	if err != nil {
		return nil, err
	}
	if !validID(req.RequestID) || req.GroupID == "" || req.ServerEpoch == "" {
		return nil, fail(400, "invalid_request", "group_id, request_id and server_epoch required")
	}
	if req.ServerEpoch != s.epoch {
		return nil, fail(409, "epoch_conflict", "server restarted; refresh state before another press")
	}
	if !d.Memberships[req.GroupID] {
		return nil, fail(403, "not_member", "group membership required")
	}
	if d.CurrentGroupID != req.GroupID || d.CurrentVersion != req.CurrentVersion {
		return nil, fail(409, "version_conflict", "current group changed; refresh state")
	}
	return d, nil
}
func (s *Service) requestSlot(id, key string) (*requestRecord, error) {
	records := s.requests[id]
	if records == nil {
		records = map[string]*requestRecord{}
		s.requests[id] = records
	}
	if r := records[key]; r != nil {
		return r, nil
	}
	if len(records) >= MaxRequests {
		return nil, fail(429, "request_capacity", "too many protected requests; wait before pressing again")
	}
	r := &requestRecord{}
	records[key] = r
	return r, nil
}
func (s *Service) Acquire(id string, req AcquireRequest) (Floor, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	d, err := s.validateFloor(id, req.FloorRequest)
	if err != nil {
		return Floor{}, err
	}
	if req.ValidUntil.IsZero() || req.ValidUntil.After(now.Add(10*time.Second)) {
		return Floor{}, fail(400, "invalid_deadline", "valid_until must be within ten seconds of server time")
	}
	key := requestKey(req.FloorRequest)
	r, err := s.requestSlot(id, key)
	if err != nil {
		return Floor{}, err
	}
	if !r.ValidUntil.IsZero() && !r.ValidUntil.Equal(req.ValidUntil) {
		return Floor{}, fail(409, "request_conflict", "request_id was used with a different deadline")
	}
	if !now.Before(req.ValidUntil) {
		if r.Status == "" {
			r.ValidUntil = req.ValidUntil
			r.Status = "expired"
			r.ProtectedUntil = now.Add(RequestProtection)
		}
		return Floor{}, fail(409, "request_expired", "press deadline expired; release and press again")
	}
	if r.Status != "" {
		if r.Status == "granted" {
			if l := s.floors[req.GroupID]; l != nil && l.DeviceID == id && l.RequestID == req.RequestID && l.LeaseID == r.LeaseID {
				return l.Floor, nil
			}
		}
		return Floor{}, fail(409, "request_ended", "request was canceled, refused or expired; release and press again")
	}
	r.ValidUntil = req.ValidUntil
	r.ProtectedUntil = now.Add(RequestProtection)
	if s.floors[req.GroupID] != nil {
		r.Status = "busy"
		return Floor{}, fail(409, "floor_busy", "group floor is already held")
	}
	leaseID, err := s.randomID("lease_", 16)
	if err != nil {
		r.Status = "failed"
		return Floor{}, err
	}
	f := Floor{DeviceID: id, DeviceName: d.Name, RequestID: req.RequestID, LeaseID: leaseID, ExpiresAt: now.Add(LeaseDuration)}
	s.floors[req.GroupID] = &lease{Floor: f, Version: req.CurrentVersion}
	r.Status = "granted"
	r.LeaseID = leaseID
	return f, nil
}
func (s *Service) Renew(id string, req RenewRequest) (Floor, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	if _, err := s.validateFloor(id, req.FloorRequest); err != nil {
		return Floor{}, err
	}
	l := s.floors[req.GroupID]
	if req.LeaseID == "" || l == nil || l.DeviceID != id || l.Version != req.CurrentVersion || l.RequestID != req.RequestID || l.LeaseID != req.LeaseID {
		return Floor{}, fail(409, "lease_not_held", "lease is no longer held; release and press again")
	}
	l.ExpiresAt = now.Add(LeaseDuration)
	if r := s.requests[id][requestKey(req.FloorRequest)]; r != nil {
		r.ProtectedUntil = now.Add(RequestProtection)
	}
	return l.Floor, nil
}
func (s *Service) Release(id string, req FloorRequest) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	if _, err := s.validateFloor(id, req); err != nil {
		return err
	}
	r, err := s.requestSlot(id, requestKey(req))
	if err != nil {
		return err
	}
	r.Status = "canceled"
	r.ProtectedUntil = now.Add(RequestProtection)
	if l := s.floors[req.GroupID]; l != nil && l.DeviceID == id && l.Version == req.CurrentVersion && l.RequestID == req.RequestID {
		delete(s.floors, req.GroupID)
	}
	return nil
}
