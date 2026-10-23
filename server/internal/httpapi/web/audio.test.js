import test from 'node:test';
import assert from 'node:assert/strict';
import {AudioClient} from './audio.js';
const NOW = Date.parse('2026-10-24T01:00:00Z');
const binding = {device_id: 'device_a', group_id: 'group_a', current_version: 1, server_epoch: 'epoch_a'};
const deferred = () => { let resolve, reject; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return {promise, resolve, reject}; };
const flush = async () => { for (let i = 0; i < 16; i++) await Promise.resolve(); };
function source() {
  const track = {kind: 'audio', enabled: true, stopped: false, stop() { this.stopped = true; }};
  return {track, stream: {getTracks: () => [track], getAudioTracks: () => [track]}, closes: 0, close() { this.closes++; }};
}
class FakePeer {
  constructor() { this.connectionState = 'new'; this.iceConnectionState = 'new'; this.iceGatheringState = 'complete'; this.transceivers = []; this.listeners = new Map(); this.remote = source(); this.closed = 0; }
  addTransceiver(track, options) { this.transceivers.push({track, options}); return {setCodecPreferences: codecs => { this.codecs = codecs; }}; }
  async createOffer() { return {type: 'offer', sdp: 'synthetic-offer'}; }
  async setLocalDescription(value) { this.localDescription = value; }
  async setRemoteDescription(value) {
    this.remoteDescription = value; this.connectionState = 'connected'; this.iceConnectionState = 'connected';
    this.ontrack?.({streams: [this.remote.stream], track: this.remote.track}); this.onconnectionstatechange?.();
  }
  addEventListener(name, callback) { this.listeners.set(name, callback); }
  removeEventListener(name) { this.listeners.delete(name); }
  close() { this.closed++; this.connectionState = 'closed'; }
  async getStats() { return new Map([
    ['out', {type: 'outbound-rtp', kind: 'audio', packetsSent: 4, bytesSent: 400}],
    ['in', {type: 'inbound-rtp', kind: 'audio', packetsReceived: 8, bytesReceived: 800, totalAudioEnergy: .1, totalSamplesDuration: 2}],
    ['codec', {type: 'codec', mimeType: 'audio/opus'}],
  ]); }
}
const answer = body => ({peer_id: body.peer_id, group_id: body.group_id, current_version: body.current_version,
  server_epoch: body.server_epoch, answer: {type: 'answer', sdp: 'synthetic-answer'}});
function setup(options = {}) {
  const calls = [], peers = [], sources = [], remoteEvents = [], timers = new Map();
  let context = {...binding}, id = 0, timer = 0;
  const output = {srcObject: null, muted: false, pauses: 0, async play() { return options.play?.(); }, pause() { this.pauses++; }};
  const request = async (path, method = 'GET', body, extra) => {
    calls.push({path, method, body, extra});
    if (options.request) return options.request(path, method, body, extra);
    if (method === 'GET') return {available: true, peer_id: '', server_epoch: context.server_epoch};
    if (method === 'POST') return answer(body);
    return {closed: true};
  };
  const audio = new AudioClient({request, scopeRequest: options.scopeRequest, getContext: () => context, serverNow: () => NOW,
    sourceFactory: options.sourceFactory || (() => { const value = source(); sources.push(value); return value; }),
    createPeer: () => { const peer = options.createPeer ? options.createPeer() : new FakePeer(); peers.push(peer); return peer; },
    createStream: tracks => ({getTracks: () => tracks}),
    getCodecs: () => [{mimeType: 'audio/PCMU'}, {mimeType: 'audio/opus', clockRate: 48000}], output,
    random: () => `peer_id_${++id}`, onRemoteStream: (stream, value) => remoteEvents.push({stream, binding: value}),
    schedule(callback, delay) { const key = ++timer; timers.set(key, {callback, delay}); return key; }, cancel: key => timers.delete(key),
  });
  return {audio, calls, peers, sources, output, remoteEvents, timers, setContext(value) { context = value; }};
}

test('no source or Peer before explicit enable; one Opus sendrecv track stays disabled until authorized', async () => {
  const h = setup(); await h.audio.sync(); assert.equal(h.sources.length, 0); assert.equal(h.peers.length, 0);
  await h.audio.enable(); await flush();
  assert.equal(h.audio.isReady(), true); assert.equal(h.peers[0].transceivers.length, 1);
  assert.equal(h.peers[0].transceivers[0].options.direction, 'sendrecv');
  assert.deepEqual(h.peers[0].codecs.map(codec => codec.mimeType), ['audio/opus']);
  assert.equal(h.sources[0].track.enabled, false); assert.equal(h.audio.setSending(true), true); assert.equal(h.sources[0].track.enabled, true);
  h.audio.setSending(false); assert.equal(h.sources[0].track.enabled, false); h.audio.disable();
});

test('create binds current group/version/epoch, replaces queried Peer, and uses complete ICE offer', async () => {
  const h = setup({request: (path, method, body) => method === 'GET' ? {available: true, peer_id: 'old_peer', server_epoch: 'epoch_a'} : method === 'POST' ? answer(body) : {closed: true}});
  await h.audio.enable();
  assert.deepEqual(h.calls.find(call => call.method === 'POST').body, {
    peer_id: 'peer_id_1', replace_peer_id: 'old_peer', group_id: 'group_a', current_version: 1, server_epoch: 'epoch_a',
    valid_until: new Date(NOW + 9000).toISOString(), offer: {type: 'offer', sdp: 'synthetic-offer'},
  }); h.audio.disable();
});

test('autoplay rejection blocks sending and explicit retry reuses same Peer', async () => {
  let blocked = true;
  const h = setup({play: () => { if (blocked) throw Object.assign(new Error('blocked'), {name: 'NotAllowedError'}); }});
  await h.audio.enable(); await flush();
  assert.equal(h.audio.phase, 'playback_blocked'); assert.equal(h.audio.setSending(true), false);
  blocked = false; await h.audio.retryPlayback();
  assert.equal(h.audio.isReady(), true); assert.equal(h.peers.length, 1); h.audio.disable();
});

test('permission denial has explicit retry and cannot spin getUserMedia automatically', async () => {
  let count = 0;
  const h = setup({sourceFactory: () => { count++; return Promise.reject(Object.assign(new Error('denied'), {name: 'NotAllowedError'})); }});
  await h.audio.enable(); await h.audio.sync(); await h.audio.sync();
  assert.equal(h.audio.phase, 'error'); assert.match(h.audio.message, /麦克风权限/); assert.equal(count, 1); assert.equal(h.peers.length, 0);
  await h.audio.enable(); assert.equal(count, 2); h.audio.disable();
});

test('GET media failure closes a microphone that resolves after the failure', async () => {
  const capture = deferred(), value = source();
  const h = setup({sourceFactory: () => capture.promise, request: () => Promise.reject(new Error('offline'))});
  await h.audio.enable(); assert.equal(h.audio.phase, 'error');
  capture.resolve(value); await flush();
  assert.equal(value.track.enabled, false); assert.equal(value.track.stopped, true); assert.equal(value.closes, 1); assert.equal(h.peers.length, 0);
});

test('suspend during permission prompt stops late capture and never posts an offer', async () => {
  const capture = deferred(), value = source(); const h = setup({sourceFactory: () => capture.promise});
  const enabling = h.audio.enable(); h.audio.suspend('hidden'); capture.resolve(value); await enabling;
  assert.equal(value.track.stopped, true); assert.equal(h.calls.filter(call => call.method === 'POST').length, 0); assert.equal(h.audio.isReady(), false);
});

test('late answer and old ontrack cannot close or replace a newer Peer', async () => {
  const oldAnswer = deferred(); let oldBody;
  const h = setup({request: (path, method, body) => {
    if (method === 'GET') return {available: true, peer_id: '', server_epoch: 'epoch_a'};
    if (method === 'POST' && body.peer_id === 'peer_id_1') { oldBody = body; return oldAnswer.promise; }
    return method === 'POST' ? answer(body) : {closed: true};
  }});
  const old = h.audio.enable(); await flush(); const oldTrack = h.peers[0].ontrack;
  h.audio.suspend('switch'); h.setContext({...binding, group_id: 'group_b', current_version: 2});
  await h.audio.sync(); await flush(); const newPeer = h.audio.peer, newStream = h.output.srcObject;
  oldAnswer.resolve(answer(oldBody)); await old;
  const stale = source(); await oldTrack({track: stale.track, streams: [stale.stream]}); await flush();
  assert.equal(h.audio.peer, newPeer); assert.equal(h.output.srcObject, newStream); assert.equal(newPeer.closed, 0);
  assert.equal(stale.track.stopped, true); assert.equal(h.sources[0].track.stopped, true); assert.equal(h.sources[1].track.stopped, false);
  assert.equal(h.calls.filter(call => call.method === 'DELETE' && call.path.endsWith('peer_id_1')).length, 2); h.audio.disable();
});

test('close detaches remote playback, announces null stream, stops capture and cancels known ID', async () => {
  const h = setup(); await h.audio.enable(); await flush(); h.audio.setSending(true);
  h.audio.suspend('hidden');
  assert.equal(h.sources[0].track.enabled, false); assert.equal(h.sources[0].track.stopped, true);
  assert.equal(h.output.srcObject, null); assert.equal(h.output.pauses, 1);
  assert.equal(h.remoteEvents.at(-1).stream, null); assert.equal(h.peers[0].closed, 1);
  assert.equal(h.calls.at(-1).method, 'DELETE'); assert.equal(h.audio.desired, true); assert.equal(h.audio.isReady(), false);
  h.setContext(null); await h.audio.sync(); assert.equal(h.peers.length, 1);
  h.setContext({...binding}); await h.audio.sync(); assert.equal(h.peers.length, 2); assert.equal(h.sources[1].track.enabled, false); h.audio.disable();
});

test('ICE gathering timeout closes capture and never creates server Peer', async () => {
  const h = setup({createPeer: () => { const peer = new FakePeer(); peer.iceGatheringState = 'gathering'; return peer; }});
  const enabling = h.audio.enable(); await flush();
  assert.equal(h.timers.size, 1); [...h.timers.values()][0].callback(); await enabling;
  assert.equal(h.audio.phase, 'error'); assert.equal(h.sources[0].track.stopped, true); assert.equal(h.calls.some(call => call.method === 'POST'), false);
});

test('disconnect synchronously stops sending and requires explicit retry', async () => {
  const h = setup(); await h.audio.enable(); await flush(); h.audio.setSending(true);
  h.peers[0].connectionState = 'disconnected'; h.peers[0].onconnectionstatechange();
  assert.equal(h.sources[0].track.enabled, false); assert.equal(h.audio.isReady(), false); assert.equal(h.audio.phase, 'error');
  await h.audio.sync(); assert.equal(h.peers.length, 1); h.audio.disable();
});

test('listening mute is independent of capture; stats contain decoded transport counters without SDP', async () => {
  const h = setup(); await h.audio.enable(); await flush(); h.audio.setListeningMuted(true);
  assert.equal(h.output.muted, true); assert.equal(h.audio.isReady(), true); assert.equal(h.sources[0].track.enabled, false);
  const stats = await h.audio.stats(); assert.equal(stats.codec, 'audio/opus'); assert.equal(stats.received_energy, .1);
  assert.equal(stats.received_packets, 8); assert.equal(stats.sent_packets, 4); assert.equal(JSON.stringify(stats).includes('synthetic-offer'), false); h.audio.disable();
});

test('cleanup and compensating DELETE retain the original identity scope after replacement', async () => {
  let identity = 'original-synthetic', body; const waiting = deferred(), scopedCalls = [];
  const h = setup({scopeRequest: () => {
    const captured = identity;
    return async (path, method = 'GET', value) => {
      scopedCalls.push({identity: captured, path, method});
      if (method === 'GET') return {available: true, peer_id: '', server_epoch: 'epoch_a'};
      if (method === 'POST') { body = value; return waiting.promise; }
      return {closed: true};
    };
  }});
  const enabling = h.audio.enable(); await flush(); identity = 'replacement-synthetic';
  h.audio.suspend('identity replaced'); waiting.resolve(answer(body)); await enabling;
  const deletes = scopedCalls.filter(call => call.method === 'DELETE');
  assert.equal(deletes.length, 2); assert.ok(deletes.every(call => call.identity === 'original-synthetic'));
});


for (const failure of ['permission', 'connection']) test(`${failure} failure keeps explicit retry after hidden/offline/group recovery without recapturing`, async () => {
  let attempts = 0;
  const h = setup(failure === 'permission' ? {sourceFactory: () => {
    attempts++;
    if (attempts === 1) return Promise.reject(Object.assign(new Error('denied'), {name: 'NotAllowedError'}));
    return source();
  }} : {});
  await h.audio.enable(); await flush();
  if (failure === 'connection') { h.peers[0].connectionState = 'disconnected'; h.peers[0].onconnectionstatechange(); }
  const originalMessage = h.audio.message;
  for (const reason of ['hidden', 'offline', 'switching']) {
    h.setContext(null); h.audio.suspend(reason); await h.audio.sync();
    h.setContext({...binding, group_id: 'group_recovered', current_version: 2, server_epoch: 'epoch_recovered'}); await h.audio.sync();
    assert.equal(h.audio.phase, 'error'); // Product UI retains the explicit retry button.
    assert.equal(h.audio.retryRequired, true); assert.equal(h.audio.message, originalMessage);
    assert.equal(h.audio.isReady(), false);
    assert.equal(failure === 'permission' ? attempts : h.sources.length, 1);
  }
  await h.audio.enable(); await flush();
  assert.equal(failure === 'permission' ? attempts : h.sources.length, 2);
  assert.equal(h.audio.isReady(), true); assert.equal(h.audio.localStream.getAudioTracks()[0].enabled, false);
  h.audio.disable();
});
