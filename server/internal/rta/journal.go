package rta

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"github.com/iewiad/miku-intercom/server/internal/persist"
)

type journalEntry struct {
	Device  string `json:"device"`
	ID      string `json:"command_id"`
	Session string `json:"session_id"`
	Stopped bool   `json:"stopped"`
}
type journalSnapshot struct {
	Binding string                  `json:"binding"`
	Records map[string]journalEntry `json:"records"`
}

func (p *Proxy) binding() string {
	sum := sha256.Sum256([]byte(p.base.String() + "\n" + p.config.AppID + "\n" + p.rtcPolicy()))
	return hex.EncodeToString(sum[:])
}

// The journal contains no API key, endpoint token, SDP, transcript, or audio. A recovered
// session is only eligible for cleanup; it is never resumed or treated as a fresh command.
func (p *Proxy) openJournal(path string) error {
	store, e := persist.Open(path)
	if e != nil {
		return errors.New("cannot open private voice journal")
	}
	body, e := store.Load()
	if e != nil {
		store.Close()
		return errors.New("cannot read voice journal")
	}
	snapshot := journalSnapshot{Binding: p.binding(), Records: map[string]journalEntry{}}
	if len(body) > 0 {
		if len(body) > 4*1024*1024 || json.Unmarshal(body, &snapshot) != nil || snapshot.Binding != p.binding() || snapshot.Records == nil || len(snapshot.Records) > 4096 {
			store.Close()
			return errors.New("invalid or differently bound voice journal; preserve it and resolve pending sessions")
		}
	}
	for key, entry := range snapshot.Records {
		if !validID.MatchString(entry.Device) || !validID.MatchString(entry.ID) || key != entry.Device+"/"+entry.ID || (entry.Session != "" && !validID.MatchString(entry.Session)) {
			store.Close()
			return errors.New("invalid voice journal record")
		}
		phase := "stopped"
		if !entry.Stopped {
			phase = "stop_unconfirmed"
			if p.active[entry.Device] != "" {
				store.Close()
				return errors.New("duplicate pending voice session")
			}
			p.active[entry.Device] = entry.ID
		}
		p.records[key] = &record{id: entry.ID, session: entry.Session, phase: phase}
	}
	p.journal = store
	p.durable = snapshot.Records
	for device, id := range p.active {
		go p.reap(device, id)
	}
	return nil
}

// Caller owns record.mu; the separate immutable durable map avoids locking other sessions.
func (p *Proxy) saveRecord(device string, r *record, stopped bool) error {
	if p.journal == nil {
		return nil
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	next := make(map[string]journalEntry, len(p.durable)+1)
	for key, entry := range p.durable {
		next[key] = entry
	}
	next[device+"/"+r.id] = journalEntry{Device: device, ID: r.id, Session: r.session, Stopped: stopped}
	body, e := json.Marshal(journalSnapshot{Binding: p.binding(), Records: next})
	if e != nil {
		return fail(503, "voice_journal_unconfirmed")
	}
	if p.journal.Save(body) != nil {
		return fail(503, "voice_journal_unconfirmed")
	}
	p.durable = next
	return nil
}
