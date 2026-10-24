import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {channelPresentation} from './app.js';

const flush = async () => { for (let i = 0; i < 100; i++) await Promise.resolve(); };
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return {promise, resolve}; };

// Real app boot, ControlClient, AudioClient and event handlers; only DOM/native
// media/network boundaries are doubles. No browser, microphone or audio I/O.
async function product({empty = false, stateRequest = null} = {}) {
  const originals = new Map(), install = (key, value) => {
    originals.set(key, Object.getOwnPropertyDescriptor(globalThis, key));
    Object.defineProperty(globalThis, key, {value, configurable: true, writable: true});
  };
  const documentEvents = {}, windowEvents = {}, intervals = [], timers = new Map(), requests = [], peers = [], tracks = [], clipboard = [];
  const doc = {hidden: false, activeElement: null, addEventListener(type, callback) { (documentEvents[type] ??= []).push(callback); }};
  class Element {
    constructor(tag, attrs = '') {
      this.tag = tag; this.value = ''; this.textContent = ''; this.children = []; this.listeners = {}; this.dataset = {}; this.attrs = new Map();
      this.hidden = /\bhidden\b/.test(attrs); this.disabled = /\bdisabled\b/.test(attrs); this.id = attrs.match(/\bid="([^"]+)"/)?.[1];
    }
    set innerHTML(value) { throw new Error('Untrusted content must remain text'); }
    append(...children) { this.children.push(...children); }
    replaceChildren(...children) { this.children = children; }
    setAttribute(name, value) { this.attrs.set(name, value); }
    setCustomValidity(value) { this.validityMessage = value; }
    focus() { doc.activeElement = this; }
    setPointerCapture() {} pause() {}
    async play() { if (settings.playbackBlocked) throw new Error('synthetic autoplay denial'); }
    addEventListener(type, callback) { (this.listeners[type] ??= []).push(callback); }
    async dispatch(type, fields = {}) { await Promise.all((this.listeners[type] || []).map(callback => callback({preventDefault() {}, ...fields}))); }
  }
  const html = await readFile(new URL('./index.html', import.meta.url), 'utf8'), nodes = new Map();
  for (const match of html.matchAll(/<([a-z][\w-]*)\b([^>]*\bid="[^"]+"[^>]*)>/g)) {
    const element = new Element(match[1], match[2]); nodes.set(element.id, element);
  }
  const $ = id => nodes.get(id) || null;
  doc.getElementById = $; doc.createElement = tag => new Element(tag);
  const identity = {device: {id: 'ui_device', name: '我'}, token: 'synthetic-ui-token'};
  const store = new Map([['shenglu.control.identity.v1', JSON.stringify(identity)]]);
  let state = {device: identity.device, server_epoch: 'ui_epoch', server_time: new Date().toISOString(), current_group_id: empty ? '' : 'one', current_version: empty ? 0 : 1,
    groups: empty ? [] : [{id: 'one', name: '当前群', member_count: 2}, {id: 'two', name: '后勤组', member_count: 1}]};
  const settings = {playbackBlocked: false, captureDenied: false, copyDenied: false, membershipDenied: false, membership: null, acquire: null, switching: null, stateRequest};
  const response = (status, value) => ({ok: status < 400, status, json: async () => value});
  const granted = body => response(200, {floor: {device_id: identity.device.id, request_id: body.request_id, peer_id: body.peer_id,
    lease_id: 'ui_lease', expires_at: new Date(Date.now() + 3000).toISOString()}});
  class Peer {
    constructor() { this.connectionState = 'connected'; this.iceGatheringState = 'complete'; peers.push(this); }
    addTransceiver() { return {setCodecPreferences() {}}; }
    async createOffer() { return {type: 'offer', sdp: 'synthetic-ui-offer'}; }
    async setLocalDescription(value) { this.localDescription = value; }
    async setRemoteDescription() { await this.ontrack({track: {}, streams: [{}]}); }
    close() { this.connectionState = 'closed'; }
  }
  try {
    install('document', doc); install('window', {addEventListener(type, callback) { (windowEvents[type] ??= []).push(callback); }});
    install('localStorage', {getItem: key => store.get(key) || null, setItem: (key, value) => store.set(key, value), removeItem: key => store.delete(key)});
    install('setInterval', callback => { intervals.push(callback); return intervals.length; });
    install('setTimeout', (callback, delay) => { const id = Symbol(); timers.set(id, {callback, delay}); return id; });
    install('clearTimeout', id => timers.delete(id));
    install('RTCPeerConnection', Peer); install('RTCRtpReceiver', {getCapabilities: () => ({codecs: [{mimeType: 'audio/opus'}]})});
    install('navigator', {
      mediaDevices: {async getUserMedia() {
        if (settings.captureDenied) throw Object.assign(new Error('synthetic permission denial'), {name: 'NotAllowedError'});
        let enabled = true; const changes = [];
        const track = {kind: 'audio', changes, get enabled() { return enabled; }, set enabled(value) { enabled = value; changes.push(value); }, stop() { this.enabled = false; this.readyState = 'ended'; }};
        tracks.push(track); return {getTracks: () => [track], getAudioTracks: () => [track]};
      }},
      clipboard: {async writeText(value) { if (settings.copyDenied) throw new Error('synthetic clipboard denial'); clipboard.push(value); }},
    });
    install('fetch', async (url, options = {}) => {
      const path = url.replace('/api/v1', ''), body = options.body && JSON.parse(options.body);
      requests.push({path, body, method: options.method, sourceEnabled: tracks.at(-1)?.enabled});
      if (path === '/voice') return response(200, {available: false, command_id: ''});
      if (path === '/state') {
        if (settings.stateRequest) { const result = await settings.stateRequest.promise; if (result) return result; }
        return response(200, state);
      }
      if (path === '/media') return response(200, {available: true, server_epoch: state.server_epoch, peer_id: ''});
      if (path === '/media/peers') return response(200, {...body, answer: {type: 'answer', sdp: 'synthetic-ui-answer'}});
      if (path.startsWith('/media/peers/') && options.method === 'DELETE') return response(200, {closed: true});
      if (path === '/floor/acquire') return settings.acquire ? settings.acquire.promise.then(() => granted(body)) : granted(body);
      if (path === '/floor/release') return response(200, {released: true});
      if (path === '/current-group') {
        if (settings.switching) await settings.switching.promise;
        state = {...state, current_group_id: body.group_id, current_version: state.current_version + 1};
        return response(200, {operation: {...body, status: 'applied'}, state});
      }
      if (path === '/groups' || path === '/memberships') {
        if (settings.membership) await settings.membership.promise;
        if (settings.membershipDenied) return response(400, {error: {code: 'invalid_code', message: '口令不正确'}});
        const group = {id: `new_group_${state.groups.length + 1}`, name: body.name || '伙伴的群', purpose: body.purpose, member_count: 1};
        state = {...state, groups: [...state.groups, group], current_group_id: state.current_group_id || group.id,
          current_version: state.current_group_id ? state.current_version : 1};
        return response(200, {group, state, ...(path === '/groups' ? {code: 'ABCDEFGH'} : {})});
      }
      throw new Error(`Unexpected product request: ${path}`);
    });
    await import(`./app.js?product-ui=${crypto.randomUUID()}`); await flush();
  } catch (error) { restore(); throw error; }
  function restore() { for (const [key, descriptor] of originals) { if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key]; } }
  return {$, settings, tracks, peers, requests, clipboard, doc, granted,
    async key(key) { for (const callback of documentEvents.keydown || []) callback({key, preventDefault() {}}); await flush(); },
    async poll() { intervals[0](); await flush(); },
    async enable() { await $('audio-enable').dispatch('click'); await flush(); },
    async close() { for (const callback of windowEvents.pagehide || []) callback(); await flush(); restore(); },
  };
}

test('task panel immediately closes a pending or held send; closing/Escape never captures, reconnects or resumes a press', async () => {
  const h = await product();
  try {
    await h.enable(); assert.equal(h.$('channel-panel').dataset.phase, 'ready');
    const originalPeer = h.peers[0], track = h.tracks[0];
    h.settings.acquire = deferred();
    await h.$('ptt').dispatch('keydown', {key: ' ', repeat: false}); await flush();
    assert.equal(h.$('channel-panel').dataset.phase, 'requesting');
    const sendingBefore = track.changes.filter(Boolean).length;
    await h.$('add-group-open').dispatch('click');
    assert.equal(track.enabled, false); assert.equal(h.$('ptt').disabled, true); assert.equal(h.$('ptt').attrs.get('aria-pressed'), 'false');
    assert.equal(h.$('add-group-panel').hidden, false); assert.equal(h.doc.activeElement, h.$('join-code'));
    await h.$('ptt').dispatch('keydown', {key: 'Enter'});
    await h.$('audio-enable').dispatch('click'); // Even a forced click cannot capture behind the panel.
    await h.$('voice-start').dispatch('click');
    assert.equal(h.requests.filter(request => request.path === '/floor/acquire').length, 1);
    h.settings.acquire.resolve(); await flush();
    assert.equal(track.changes.filter(Boolean).length, sendingBefore); assert.equal(track.enabled, false);
    assert.ok(h.requests.filter(request => request.path === '/floor/release').length >= 2);
    assert.ok(h.requests.filter(request => request.path === '/floor/release').every(request => !request.sourceEnabled));
    await h.key('Escape');
    assert.equal(h.$('add-group-panel').hidden, true); assert.equal(h.doc.activeElement, h.$('add-group-open'));
    assert.equal(h.$('ptt').attrs.get('aria-pressed'), 'false'); assert.equal(track.enabled, false);
    assert.equal(h.peers.length, 1); assert.equal(h.tracks.length, 1); assert.equal(originalPeer.connectionState, 'connected');
    h.settings.acquire = null;
    await h.$('ptt').dispatch('keydown', {key: ' ', repeat: false}); await flush();
    assert.equal(track.enabled, true); assert.equal(h.$('channel-panel').dataset.phase, 'speaking');
    const beforeFilter = h.requests.length;
    h.$('group-filter').value = '后勤'; await h.$('group-filter').dispatch('input');
    assert.equal(track.enabled, true); assert.equal(h.$('current-name').textContent, '当前群'); assert.equal(h.requests.length, beforeFilter);
    await h.$('add-group-open').dispatch('click'); assert.equal(track.enabled, false);
    await h.$('add-group-close').dispatch('click');
    assert.equal(track.enabled, false); assert.equal(h.$('ptt').attrs.get('aria-pressed'), 'false'); assert.equal(h.peers.length, 1);
  } finally { await h.close(); }
});

test('deferred/failed state is never presented as an empty directory; only a confirmed empty state exposes onboarding', async () => {
  const stateRequest = deferred(), h = await product({empty: true, stateRequest});
  try {
    assert.equal(h.$('channel-panel').dataset.phase, 'loading'); assert.equal(h.$('current-name').textContent, '正在获取群组');
    assert.equal(h.$('empty-actions').hidden, true); assert.equal(h.$('empty-groups').hidden, true); assert.equal(h.$('group-count').textContent, '—');
    stateRequest.resolve({ok: false, status: 503, json: async () => ({error: {code: 'unavailable', message: '连接暂时不可用'}})});
    await flush();
    assert.equal(h.$('channel-panel').dataset.phase, 'error'); assert.equal(h.$('current-name').textContent, '群组暂未加载');
    assert.equal(h.$('empty-actions').hidden, true); assert.equal(h.$('empty-groups').hidden, true);
    h.settings.stateRequest = deferred(); await h.$('refresh-state').dispatch('click'); await flush();
    assert.equal(h.$('current-name').textContent, '正在获取群组'); assert.equal(h.$('empty-actions').hidden, true);
    h.settings.stateRequest.resolve(); await flush();
    assert.equal(h.$('channel-panel').dataset.phase, 'empty'); assert.equal(h.$('group-count').textContent, '0 个');
    assert.equal(h.$('empty-actions').hidden, false); assert.equal(h.$('empty-groups').hidden, false);
    assert.notEqual(h.$('notice').dataset.tone, 'error');
    assert.equal(h.tracks.length, 0); assert.equal(h.peers.length, 0);
  } finally { await h.close(); }
});

test('Escape closes only the task UI while an already submitted membership request still completes', async () => {
  const h = await product({empty: true});
  try {
    await h.$('empty-join').dispatch('click'); h.$('join-code').value = 'ABCDEFGH';
    h.settings.membership = deferred();
    const submitted = h.$('join-form').dispatch('submit'); await flush();
    assert.equal(h.requests.filter(request => request.path === '/memberships').length, 1);
    await h.key('Escape');
    assert.equal(h.$('add-group-panel').hidden, true); assert.equal(h.$('add-group-open').disabled, true);
    await h.$('add-group-open').dispatch('click'); assert.equal(h.$('add-group-panel').hidden, true);
    h.settings.membership.resolve(); await submitted; await flush();
    assert.equal(h.$('current-name').textContent, '伙伴的群'); assert.equal(h.$('join-code').value, '');
    assert.equal(h.$('add-group-open').disabled, false); assert.equal(h.$('ptt').attrs.get('aria-pressed'), 'false');
    assert.equal(h.tracks.length, 0); assert.equal(h.peers.length, 0);
  } finally { await h.close(); }
});

test('empty-group shortcuts, error retention, successful collapse and one-time code copy use the real forms', async () => {
  const h = await product({empty: true});
  try {
    assert.equal(h.$('empty-actions').hidden, false); assert.equal(h.$('audio-enable').disabled, true);
    await h.$('empty-join').dispatch('click'); assert.equal(h.doc.activeElement, h.$('join-code'));
    await h.key('Escape'); assert.equal(h.doc.activeElement, h.$('empty-join'));
    await h.$('empty-create').dispatch('click'); assert.equal(h.doc.activeElement, h.$('create-name'));
    h.$('create-name').value = '周末同行'; h.$('create-purpose').value = '协助伙伴';
    h.settings.membershipDenied = true;
    await h.$('create-form').dispatch('submit'); await flush();
    assert.equal(h.$('add-group-panel').hidden, false); assert.equal(h.$('add-group-status').hidden, false);
    assert.equal(h.$('create-name').value, '周末同行'); assert.equal(h.$('create-purpose').value, '协助伙伴');
    h.settings.membershipDenied = false;
    await h.$('create-form').dispatch('submit'); await flush();
    assert.equal(h.$('add-group-panel').hidden, true); assert.equal(h.$('create-name').value, ''); assert.equal(h.$('create-purpose').value, '');
    assert.equal(h.$('empty-actions').hidden, true); assert.equal(h.doc.activeElement, h.$('add-group-open'));
    assert.equal(h.$('share-card').hidden, false); assert.equal(h.$('share-code').textContent, 'ABCDEFGH');
    h.settings.copyDenied = true; await h.$('copy-code').dispatch('click');
    assert.equal(h.$('notice').dataset.tone, 'error'); assert.equal(h.$('share-code').textContent, 'ABCDEFGH');
    h.settings.copyDenied = false; await h.$('copy-code').dispatch('click');
    assert.deepEqual(h.clipboard, ['ABCDEFGH']); assert.equal(h.$('notice').dataset.tone, 'success');
    await h.$('add-group-open').dispatch('click'); h.$('join-code').value = 'ABCDEFGH';
    await h.$('join-form').dispatch('submit'); await flush();
    assert.equal(h.$('join-code').value, ''); assert.equal(h.$('add-group-panel').hidden, true);
    assert.equal(h.tracks.length, 0); assert.equal(h.peers.length, 0);
  } finally { await h.close(); }
});

test('pending switch keeps the authoritative current name and excludes new task panels until confirmed', async () => {
  const h = await product();
  try {
    h.settings.switching = deferred();
    await h.$('group-list').children[1].children[1].dispatch('click'); await flush();
    assert.equal(h.$('channel-panel').dataset.phase, 'switching'); assert.equal(h.$('current-name').textContent, '当前群');
    assert.equal(h.$('add-group-open').disabled, true); assert.equal(h.$('ptt').disabled, true);
    await h.$('add-group-open').dispatch('click'); assert.equal(h.$('add-group-panel').hidden, true);
    h.settings.switching.resolve(); await flush();
    assert.equal(h.$('current-name').textContent, '后勤组'); assert.equal(h.$('add-group-open').disabled, false);
    assert.equal(h.$('channel-panel').dataset.phase, 'disconnected');
    assert.equal(h.requests.filter(request => request.path === '/current-group').length, 1);
    assert.equal(h.tracks.length, 0); assert.equal(h.peers.length, 0);
  } finally { await h.close(); }
});

test('playback and permission failures remain distinct from connected/ready and allow explicit recovery', async () => {
  const h = await product();
  try {
    h.settings.playbackBlocked = true; await h.enable();
    assert.equal(h.$('channel-panel').dataset.phase, 'playback'); assert.equal(h.$('audio-play').hidden, false); assert.equal(h.$('ptt').disabled, true);
    h.settings.playbackBlocked = false; await h.$('audio-play').dispatch('click'); await flush();
    assert.equal(h.$('channel-panel').dataset.phase, 'ready'); assert.equal(h.tracks.length, 1);
    await h.$('audio-disable').dispatch('click'); await flush();
    assert.equal(h.$('channel-panel').dataset.phase, 'disconnected');
    h.settings.captureDenied = true; await h.enable();
    assert.equal(h.$('channel-panel').dataset.phase, 'error'); assert.equal(h.$('audio-enable').hidden, false); assert.equal(h.$('ptt').disabled, true);
  } finally { await h.close(); }
});

test('presentation requires live media even with held floor; teammate status also excludes an expired lease', () => {
  let ready = false;
  const control = {allowed: () => true, identity: {device: {id: 'me'}}, connected: true, pressed: true, floorPhase: 'held', serverNow: () => 1000,
    state: {current_group_id: 'g', groups: [{id: 'g', name: '群', floor: {device_id: 'other', device_name: '伙伴', expires_at: new Date(5000).toISOString()}}]},
    audio: {desired: true, phase: 'ready', isReady: () => ready}};
  assert.equal(channelPresentation(control).phase, 'connecting');
  ready = true; assert.equal(channelPresentation(control).phase, 'speaking');
  control.pressed = false; control.floorPhase = 'idle'; assert.equal(channelPresentation(control).phase, 'listening');
  control.state.groups[0].floor.expires_at = new Date(500).toISOString(); assert.equal(channelPresentation(control).phase, 'ready');
  control.uiBlocked = true; assert.equal(channelPresentation(control).phase, 'managing');
  control.uiBlocked = false; control.mutationBusy = true; assert.equal(channelPresentation(control).phase, 'updating');
  control.mutationBusy = false; control.voice = {blocking: true, current: {}}; assert.equal(channelPresentation(control).phase, 'voice');
  control.pendingSwitch = {}; assert.equal(channelPresentation(control).phase, 'switching');
});
