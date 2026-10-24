import test from 'node:test';
import assert from 'node:assert/strict';
import {LifecycleLog} from './lifecycle.js';
const wallTime = '2026-10-24T01:00:00.000Z';
function setup(options = {}) {
  let time = 100;
  const log = new LifecycleLog({startedAt: 100, now: () => time, wallNow: () => wallTime, ...options});
  return {log, tick(ms) { time += ms; }};
}
function active() {
  return {isTrusted: true, visibility: 'visible', hidden: false, focused: true, phase: 'ready', desired: true,
    pressed: true, floorPhase: 'held', currentVersion: 4, peer: {connectionState: 'connected'},
    track: {readyState: 'live', enabled: true, muted: false}, openPeers: 1, outputAttached: true};
}

test('captures native hide, synchronous closure and resumed fresh Peer while remaining unpressed', () => {
  const {log, tick} = setup(), state = active();
  log.record('visibility-hidden', {...state, visibility: 'hidden', hidden: true});
  tick(1.25);
  state.peer.connectionState = 'closed'; state.track.readyState = 'ended'; state.track.enabled = false;
  log.record('hidden-closed', {...state, visibility: 'hidden', hidden: true, focused: false, phase: 'waiting', pressed: false,
    floorPhase: 'idle', previousPeer: state.peer, previousTrack: state.track, peer: null, track: null, openPeers: 0, outputAttached: false});
  tick(10);
  const resumed = active(); resumed.pressed = false; resumed.floorPhase = 'idle'; resumed.track.enabled = false;
  log.record('visibility-visible', resumed);
  const {events, dropped_events} = log.snapshot();
  assert.equal(events[0].is_trusted, true); assert.equal(events[0].document_hidden, true);
  assert.deepEqual(events[0].peer, {seq: 1, state: 'connected'});
  assert.deepEqual(events[0].capture, {track_state: 'live', enabled: true, muted: false});
  assert.equal(events[1].at_ms, 1.25); assert.equal(events[1].at, wallTime);
  assert.deepEqual(events[1].previous_peer, {seq: 1, state: 'closed'});
  assert.deepEqual(events[1].previous_capture, {track_state: 'ended', enabled: false, muted: false});
  assert.equal(events[1].peer, null); assert.equal(events[1].capture, null);
  assert.equal(events[1].open_peers, 0); assert.equal(events[1].output_attached, false);
  assert.deepEqual(events[2].peer, {seq: 2, state: 'connected'});
  assert.equal(events[2].pressed, false); assert.equal(events[2].capture.enabled, false);
  assert.equal(dropped_events, 0);
});

test('history remains independent of mutable browser objects, returned entries and exported snapshots', () => {
  const {log} = setup(), state = active();
  const entry = log.record('audio-state', state);
  state.peer.connectionState = 'closed'; state.track.enabled = false;
  entry.peer.state = 'failed'; entry.capture.track_state = 'ended'; entry.seq = 999;
  const exported = log.snapshot();
  exported.events[0].capture.enabled = false; exported.events[0].peer.seq = 999;
  exported.events.push(entry); exported.dropped_events = 999;
  const fresh = log.snapshot();
  assert.equal(fresh.events.length, 1); assert.equal(fresh.events[0].seq, 1);
  assert.deepEqual(fresh.events[0].peer, {seq: 1, state: 'connected'});
  assert.deepEqual(fresh.events[0].capture, {track_state: 'live', enabled: true, muted: false});
  assert.equal(fresh.dropped_events, 0);
});

test('allowlists omit private identifiers, arbitrary fields and invalid scalar values', () => {
  const {log} = setup(), secret = 'private-token-device-label-sdp';
  assert.equal(log.record(secret, active()), null);
  const state = active();
  Object.assign(state, {token: secret, id: secret, label: secret, sdp: secret, visibility: secret, phase: secret,
    floorPhase: secret, currentVersion: secret, openPeers: Infinity, desired: secret, pressed: 1, isTrusted: secret});
  Object.assign(state.peer, {id: secret, localDescription: {sdp: secret}, connectionState: secret});
  Object.assign(state.track, {id: secret, label: secret, readyState: secret, enabled: secret, muted: secret});
  const record = log.record('audio-state', state), serialized = JSON.stringify(log.snapshot());
  assert.equal(serialized.includes(secret), false);
  assert.deepEqual(record.peer, {seq: 1, state: null});
  assert.deepEqual(record.capture, {track_state: null, enabled: null, muted: null});
  for (const field of ['visibility_state', 'phase', 'floor_phase', 'current_version', 'open_peers', 'desired', 'pressed', 'is_trusted']) assert.equal(record[field], null);
  assert.equal(record.seq, 1);
});

test('bounded history drops oldest events and preserves total drop count and sequence numbers', () => {
  const {log} = setup({limit: 2});
  for (let i = 0; i < 5; i++) log.record('window-focus', {focused: true});
  assert.deepEqual(log.snapshot().events.map(event => event.seq), [4, 5]);
  assert.equal(log.snapshot().dropped_events, 3);
  const defaultLog = setup().log;
  for (let i = 0; i < 130; i++) defaultLog.record('window-blur');
  assert.equal(defaultLog.snapshot().events.length, 128);
  assert.equal(defaultLog.snapshot().dropped_events, 2);
});

test('timestamps and missing values cannot inject arbitrary strings or non-finite numbers', () => {
  const {log} = setup({now: () => Infinity, wallNow: () => 'token-secret', limit: -1});
  const entry = log.record('disabled-closed');
  assert.equal(entry.at_ms, null); assert.equal(entry.at, null);
  assert.equal(entry.peer, null); assert.equal(entry.previous_capture, null);
  assert.equal(log.record('not-an-event'), null);
  assert.equal(log.snapshot().events.length, 1);
});
