package control

// MediaBinding is an ephemeral, immutable group/version binding. Ready is set
// only after the transport is connected; replacing a binding revokes its lease.
type MediaBinding struct {
	DeviceID       string
	PeerID         string
	ServerEpoch    string
	GroupID        string
	CurrentVersion uint64
	Ready          bool
}

type MediaPermit struct {
	Source      MediaBinding
	Destination MediaBinding
	RequestID   string
	LeaseID     string
}

func (s *Service) mediaMatches(id, peer string, ready bool) bool {
	b, ok := s.mediaPeers[id]
	if !ok || b.PeerID != peer || ready && !b.Ready {
		return false
	}
	d := s.disk.Devices[id]
	return d != nil && d.Memberships[b.GroupID] && d.CurrentGroupID == b.GroupID && d.CurrentVersion == b.CurrentVersion && b.ServerEpoch == s.epoch
}

func (s *Service) MediaBinding(id string) (MediaBinding, string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	if _, err := s.identity(id); err != nil {
		return MediaBinding{}, "", err
	}
	b := s.mediaPeers[id]
	if !s.mediaMatches(id, b.PeerID, false) {
		return MediaBinding{}, s.epoch, nil
	}
	return b, s.epoch, nil
}

// BindMediaPeer compares and replaces the active Peer atomically with control
// authorization. The caller closes the returned old Peer outside this lock.
func (s *Service) BindMediaPeer(id, peer, replace, group string, version uint64, epoch string) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Now().UTC()
	s.expire(now)
	d, err := s.identity(id)
	if err != nil {
		return "", err
	}
	if !validID(peer) || replace != "" && !validID(replace) || group == "" {
		return "", fail(400, "invalid_request", "valid Peer identifiers and group required")
	}
	if epoch != s.epoch {
		return "", fail(409, "epoch_conflict", "server restarted; refresh state")
	}
	if !d.Memberships[group] {
		return "", fail(403, "not_member", "group membership required")
	}
	if d.CurrentGroupID != group || d.CurrentVersion != version {
		return "", fail(409, "version_conflict", "current group changed")
	}
	old := s.mediaPeers[id].PeerID
	if old != replace {
		return "", fail(409, "replace_conflict", "current Peer changed; refresh media state")
	}
	if old != "" {
		s.revokeMediaLease(id, old)
	}
	s.mediaPeers[id] = MediaBinding{DeviceID: id, PeerID: peer, GroupID: group, CurrentVersion: version, ServerEpoch: epoch}
	return old, nil
}
func (s *Service) revokeMediaLease(id, peer string) {
	for gid, l := range s.floors {
		if l.DeviceID == id && l.PeerID == peer {
			s.revokeOwned(id, gid, s.opts.Now().UTC())
		}
	}
}
func (s *Service) SetMediaReady(id, peer string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	if !s.mediaMatches(id, peer, false) {
		return false
	}
	b := s.mediaPeers[id]
	b.Ready = true
	s.mediaPeers[id] = b
	return true
}
func (s *Service) UnbindMediaPeer(id, peer string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	if s.mediaPeers[id].PeerID == peer {
		delete(s.mediaPeers, id)
	}
	s.revokeMediaLease(id, peer)
}
func (s *Service) MediaPeerValid(id, peer string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	return s.mediaMatches(id, peer, false)
}

// AuthorizeMedia checks authoritative state at admission. A permit is a stamp,
// not a lasting capability: the writer must call ValidateMedia immediately before
// writing and never hold the control lock while doing network I/O.
func (s *Service) AuthorizeMedia(sourceID, sourcePeer, destID, destPeer string) (MediaPermit, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	if sourceID == destID || !s.mediaMatches(sourceID, sourcePeer, true) || !s.mediaMatches(destID, destPeer, true) {
		return MediaPermit{}, false
	}
	src, dst := s.mediaPeers[sourceID], s.mediaPeers[destID]
	if src.GroupID != dst.GroupID {
		return MediaPermit{}, false
	}
	l := s.floors[src.GroupID]
	if l == nil || l.DeviceID != sourceID || l.PeerID != sourcePeer || l.Version != src.CurrentVersion {
		return MediaPermit{}, false
	}
	return MediaPermit{Source: src, Destination: dst, RequestID: l.RequestID, LeaseID: l.LeaseID}, true
}
func (s *Service) ValidateMedia(permit MediaPermit) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(s.opts.Now().UTC())
	src, dst := permit.Source, permit.Destination
	if !s.mediaMatches(src.DeviceID, src.PeerID, true) || !s.mediaMatches(dst.DeviceID, dst.PeerID, true) {
		return false
	}
	if s.mediaPeers[src.DeviceID] != src || s.mediaPeers[dst.DeviceID] != dst || src.GroupID != dst.GroupID {
		return false
	}
	l := s.floors[src.GroupID]
	return l != nil && l.DeviceID == src.DeviceID && l.PeerID == src.PeerID && l.Version == src.CurrentVersion && l.RequestID == permit.RequestID && l.LeaseID == permit.LeaseID
}
