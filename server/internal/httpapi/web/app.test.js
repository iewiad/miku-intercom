import test from 'node:test';
import assert from 'node:assert/strict';
import {ControlClient, claimOwner, ownerKey, bindPTTPointer} from './app.js';

const NOW = Date.parse('2026-10-23T10:00:00Z');
const identity = {device: {id: 'device_1', name: '<成员>'}, token: 'synthetic-local-token'};
const state = (patch = {}) => ({device: identity.device, server_epoch: 'epoch_one', server_time: new Date(NOW).toISOString(),
  current_group_id: 'group_1', current_version: 1, media_ready: false,
  groups: [{id: 'group_1', name: '<全员群>', member_count: 2, floor: null}, {id: 'group_2', name: '协作组', member_count: 1, floor: null}], ...patch});
const response = (status, data) => ({status, ok: status >= 200 && status < 300, json: async () => data});
const deferred = () => { let resolve, reject; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return {promise, resolve, reject}; };
const flush = async () => { for (let i = 0; i < 12; i++) await Promise.resolve(); };
function setup(handler = () => response(200, {released: true})) {
  const calls = [], timers = new Map(), pending = [];
  let timerID = 0, random = 0, clock = NOW;
  const client = new ControlClient({now: () => clock, random: () => `random_id_${++random}`,
    schedule(callback, delay) { const id = ++timerID; timers.set(id, {callback, delay}); return id; },
    cancel(id) { timers.delete(id); }, onPending: value => pending.push(value && {...value}),
    fetchImpl: async (url, init) => {
      const call = {url, ...init, body: init.body ? JSON.parse(init.body) : undefined};
      calls.push(call); return handler(call, calls);
    }});
  client.load(identity); client.applyState(state(), client.epoch);
  return {client, calls, timers, pending, advance(ms) { clock += ms; }};
}
const grant = request => ({floor: {device_id: identity.device.id, device_name: identity.device.name,
  request_id: request.request_id, lease_id: 'lease_one', expires_at: new Date(NOW + 3000).toISOString()}});
const applied = request => ({operation: {...request, status: 'applied', current_group_id: request.group_id, current_version: 2},
  state: state({current_group_id: request.group_id, current_version: 2})});

test('late acquire after release is released again and never becomes held', async () => {
  const delayed = deferred();
  const h = setup(call => call.url.endsWith('/floor/acquire') ? delayed.promise : response(200, {released: true}));
  const acquiring = h.client.pressFloor(); const request = h.calls[0].body;
  h.client.cancelFloor();
  assert.equal(h.client.pressed, false); assert.equal(h.client.floorPhase, 'idle');
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/release')).length, 1);
  delayed.resolve(response(200, grant(request))); await acquiring; await flush();
  assert.equal(h.client.floorPhase, 'idle'); assert.equal(h.client.lease, null);
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/release')).length, 2);
  for (const call of h.calls.filter(call => call.url.endsWith('/floor/release'))) {
    assert.equal(call.body.request_id, request.request_id); assert.equal(call.body.server_epoch, 'epoch_one');
    assert.equal('valid_until' in call.body, false); assert.equal('lease_id' in call.body, false);
  }
});

test('a new press is blocked while a cancelled acquire is unresolved', async () => {
  const delayed = deferred();
  const h = setup(call => call.url.endsWith('/floor/acquire') ? delayed.promise : response(200, {released: true}));
  const first = h.client.pressFloor(); const request = h.calls[0].body;
  h.client.cancelFloor(); await flush(); await h.client.pressFloor();
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/acquire')).length, 1);
  delayed.resolve(response(200, grant(request))); await first;
});

test('acquire binds group/version/epoch and a fixed five-second server deadline', async () => {
  const h = setup(call => response(200, call.url.endsWith('/floor/acquire') ? grant(call.body) : {released: true}));
  h.advance(1200); await h.client.pressFloor();
  assert.equal(h.client.floorPhase, 'held');
  assert.deepEqual(h.calls[0].body, {group_id: 'group_1', current_version: 1, request_id: 'random_id_1',
    server_epoch: 'epoch_one', valid_until: new Date(NOW + 6200).toISOString()});
  assert.equal(h.calls[0].cache, 'no-store'); assert.equal(h.calls[0].credentials, 'same-origin');
  assert.equal(h.calls[0].headers.Authorization, 'Bearer synthetic-local-token');
  assert.equal(h.calls[0].url.includes(identity.token), false);
  h.client.cancelFloor(); await flush();
});

test('busy rejection ends the press and never automatically retries acquire', async () => {
  const h = setup(call => call.url.endsWith('/floor/acquire') ? response(409, {error: {code: 'floor_busy', message: 'busy'}}) : response(200, {released: true}));
  await h.client.pressFloor(); await flush();
  assert.equal(h.client.pressed, false); assert.equal(h.client.floorPhase, 'idle');
  assert.equal(h.timers.size, 0); assert.equal(h.calls.filter(call => call.url.endsWith('/floor/acquire')).length, 1);
});

test('renew only happens while pressed; late renew cannot restore holding', async () => {
  const delayed = deferred();
  const h = setup(call => call.url.endsWith('/floor/acquire') ? response(200, grant(call.body)) :
    call.url.endsWith('/floor/renew') ? delayed.promise : response(200, {released: true}));
  await h.client.pressFloor();
  const renewal = [...h.timers.values()].find(timer => timer.delay === 1000);
  renewal.callback(); await flush();
  const renewCall = h.calls.find(call => call.url.endsWith('/floor/renew'));
  assert.equal(renewCall.body.lease_id, 'lease_one'); assert.equal(renewCall.body.server_epoch, 'epoch_one');
  h.client.cancelFloor(); delayed.resolve(response(200, grant(renewCall.body))); await flush();
  assert.equal(h.client.pressed, false); assert.equal(h.client.floorPhase, 'idle'); assert.equal(h.timers.size, 0);
  renewal.callback(); await flush();
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/renew')).length, 1);
  assert.ok(h.calls.filter(call => call.url.endsWith('/floor/release')).length >= 2);
});

test('lease expiry immediately ends local holding', async () => {
  const h = setup(call => response(200, call.url.endsWith('/floor/acquire') ? grant(call.body) : {released: true}));
  await h.client.pressFloor(); const expiry = [...h.timers.values()].find(timer => timer.delay === 3000);
  h.advance(3000); expiry.callback(); await flush();
  assert.equal(h.client.floorPhase, 'idle'); assert.equal(h.client.pressed, false);
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/release')).length, 1);
});

test('unknown switch outcome retries exactly the same operation after query 404', async () => {
  let putCount = 0;
  const h = setup(call => {
    if (call.method === 'PUT') { if (++putCount === 1) throw new Error('response lost'); return response(200, applied(call.body)); }
    if (call.url.includes('/operations/')) return response(404, {error: {code: 'not_found', message: 'unknown'}});
    throw new Error('unexpected request');
  });
  await h.client.switchGroup('group_2');
  const original = {...h.client.pendingSwitch};
  assert.equal(h.client.canPress(), false); assert.equal(h.client.state.current_group_id, 'group_1');
  await h.client.refresh(); assert.equal(h.calls.some(call => call.url.endsWith('/state')), false);
  await h.client.confirmSwitch();
  const puts = h.calls.filter(call => call.method === 'PUT');
  assert.deepEqual(puts[0].body, original); assert.deepEqual(puts[1].body, original);
  assert.equal(h.client.pendingSwitch, null); assert.equal(h.client.state.current_group_id, 'group_2');
  assert.deepEqual(h.pending, [original, null]);
});

test('terminal query followed by failed state refresh keeps confirmation pending', async () => {
  const h = setup(call => {
    if (call.method === 'PUT') throw new Error('timeout');
    if (call.url.includes('/operations/')) return response(200, {operation: {...h.client.pendingSwitch, status: 'applied', current_group_id: 'group_2', current_version: 2}});
    throw new Error('state unavailable');
  });
  await h.client.switchGroup('group_2'); await h.client.confirmSwitch();
  assert.ok(h.client.pendingSwitch); assert.equal(h.client.canPress(), false);
  assert.equal(h.client.pressed, false);
});

test('an earlier polling response cannot overwrite a successful switch', async () => {
  const oldPoll = deferred();
  const h = setup(call => call.method === 'PUT' ? response(200, applied(call.body)) : oldPoll.promise);
  const poll = h.client.refresh(); await h.client.switchGroup('group_2');
  oldPoll.resolve(response(200, state())); await poll;
  assert.equal(h.client.state.current_group_id, 'group_2'); assert.equal(h.client.state.current_version, 2);
});

test('polling is skipped while membership mutation is in flight', async () => {
  const delayed = deferred(); const h = setup(() => delayed.promise);
  const joining = h.client.joinGroup('ABCDEFGH'); await h.client.refresh();
  assert.equal(h.calls.length, 1);
  delayed.resolve(response(200, {state: state()})); await joining;
});

test('lower current version cannot overwrite newer state', () => {
  const h = setup(); h.client.applyState(state({current_version: 4}), h.client.epoch);
  assert.equal(h.client.applyState(state({current_version: 2}), h.client.epoch), false);
  assert.equal(h.client.state.current_version, 4);
});

test('server restart clears local floor, releases using old epoch, and never auto-presses', async () => {
  const h = setup(call => response(200, call.url.endsWith('/floor/acquire') ? grant(call.body) : {released: true}));
  await h.client.pressFloor();
  h.client.applyState(state({server_epoch: 'epoch_two'}), h.client.epoch); await flush();
  assert.equal(h.client.state.server_epoch, 'epoch_two'); assert.equal(h.client.floorPhase, 'idle'); assert.equal(h.client.pressed, false);
  assert.equal(h.calls.find(call => call.url.endsWith('/floor/release')).body.server_epoch, 'epoch_one');
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/acquire')).length, 1);
});

test('failed refresh cancels local floor and blocks new presses until resynced', async () => {
  const h = setup(call => call.url.endsWith('/floor/acquire') ? response(200, grant(call.body)) :
    call.url.endsWith('/state') ? Promise.reject(new Error('offline')) : response(200, {released: true}));
  await h.client.pressFloor(); await h.client.refresh(); await flush();
  assert.equal(h.client.connected, false); assert.equal(h.client.pressed, false); assert.equal(h.client.canPress(), false);
});

test('same-identity tabs cannot claim ownership together; takeover suspends and releases', async () => {
  const map = new Map(); const storage = {getItem: key => map.get(key) || null, setItem: (key, value) => map.set(key, value)};
  assert.equal(claimOwner(storage, 'device_1', 'tab_a'), true);
  assert.equal(claimOwner(storage, 'device_1', 'tab_b'), false);
  const h = setup(call => response(200, call.url.endsWith('/floor/acquire') ? grant(call.body) : {released: true}));
  h.client.authorize = () => storage.getItem(ownerKey('device_1')) === 'tab_a';
  await h.client.pressFloor(); assert.equal(claimOwner(storage, 'device_1', 'tab_b', true), true);
  h.client.suspend('another tab'); await flush(); await h.client.pressFloor();
  assert.equal(h.client.active, false); assert.equal(h.client.pressed, false);
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/acquire')).length, 1);
  assert.equal(h.calls.filter(call => call.url.endsWith('/floor/release')).length, 1);
});

test('401 invalidates saved identity, clears holding, and offers a clean identity reset', async () => {
  const h = setup(call => call.url.endsWith('/floor/acquire') ? response(200, grant(call.body)) :
    call.url.endsWith('/state') ? response(401, {error: {code: 'unauthorized', message: 'invalid identity'}}) : response(200, {released: true}));
  await h.client.pressFloor(); await h.client.refresh(); await flush();
  assert.equal(h.client.authInvalid, true); assert.equal(h.client.active, false);
  assert.equal(h.client.pressed, false); assert.equal(h.client.canPress(), false);
  h.client.forgetIdentity();
  assert.equal(h.client.identity, null); assert.equal(h.client.state, null); assert.equal(h.client.pendingSwitch, null);
  assert.equal(h.client.authInvalid, false); assert.equal(h.client.active, true);
});

test('stale 401 for an old token does not invalidate a replacement identity', async () => {
  const delayed = deferred(); const h = setup(() => delayed.promise);
  const refreshing = h.client.refresh();
  h.client.load({device: {id: 'replacement', name: '新成员'}, token: 'replacement-synthetic-token'});
  h.client.epoch++;
  delayed.resolve(response(401, {error: {code: 'unauthorized', message: 'old identity'}})); await refreshing;
  assert.equal(h.client.authInvalid, false); assert.equal(h.client.active, true);
});

test('identity and group names accept 32 Unicode codepoints but reject 33', async () => {
  let name;
  const h = setup(call => {
    name = call.body.name;
    return response(201, {group: {id: 'created', name}, code: 'ABCDEFGH', state: state()});
  });
  await h.client.createGroup('😀'.repeat(32));
  assert.equal(name, '😀'.repeat(32)); const count = h.calls.length;
  await h.client.createGroup('😀'.repeat(33)); assert.equal(h.calls.length, count);
  await h.client.createIdentity('中'.repeat(33)); assert.equal(h.calls.length, count);
});

test('already-open tab reloads persisted pending operation on takeover before state or PTT', async () => {
  const pending = {operation_id: 'previous_owner_operation', group_id: 'group_2', expected_version: 1};
  const h = setup(call => {
    if (call.url.includes('/operations/')) return response(404, {error: {code: 'not_found', message: 'not yet known'}});
    if (call.method === 'PUT') return response(200, applied(call.body));
    throw new Error('must not guess current state before operation terminal');
  });
  assert.equal(h.client.pendingSwitch, null);
  h.client.restorePending = () => ({...pending});
  await h.client.activate();
  assert.match(h.calls[0].url, /\/operations\/previous_owner_operation$/);
  assert.deepEqual(h.calls[1].body, pending);
  assert.equal(h.client.state.current_group_id, 'group_2'); assert.equal(h.client.pressed, false);
});

test('default timer adapters preserve the browser global receiver', () => {
  const originalSet = globalThis.setTimeout, originalClear = globalThis.clearTimeout;
  try {
    globalThis.setTimeout = function () { assert.equal(this, globalThis); return 123; };
    globalThis.clearTimeout = function () { assert.equal(this, globalThis); };
    const client = new ControlClient();
    assert.equal(client.schedule(() => {}, 1), 123);
    client.cancelFloor();
  } finally { globalThis.setTimeout = originalSet; globalThis.clearTimeout = originalClear; }
});

function mediaMock(ready = true) {
  return {ready, peerID: 'current_peer', sending: false, events: [],
    isReady() { return this.ready; }, setSending(value) { this.events.push(`send:${value}`); this.sending = value && this.ready; return this.sending; },
    suspend() { this.events.push('suspend'); this.ready = false; this.sending = false; },
    sync() { this.events.push('sync'); }, disable() { this.suspend(); }};
}

test('S2 cannot acquire before local media is connected and playable', async () => {
  const h = setup(); h.client.audio = mediaMock(false);
  await h.client.pressFloor(); assert.equal(h.calls.length, 0); assert.equal(h.client.pressed, false);
});

test('S2 acquire/renew/release retain peer_id; cancel disables capture before release request', async () => {
  const h = setup(call => {
    if (call.url.endsWith('/floor/release')) h.client.audio.events.push('release-request');
    return response(200, call.url.endsWith('/floor/release') ? {released: true} : {floor: {...grant(call.body).floor, peer_id: call.body.peer_id}});
  });
  h.client.audio = mediaMock(); await h.client.pressFloor();
  assert.equal(h.client.audio.sending, true); assert.equal(h.calls[0].body.peer_id, 'current_peer');
  await h.client.renewFloor(h.client.floorGeneration, h.client.floorRequest);
  h.client.cancelFloor(); await flush();
  assert.ok(h.calls.every(call => call.body.peer_id === 'current_peer'));
  assert.deepEqual(h.client.audio.events.slice(-2), ['send:false', 'release-request']);
  assert.equal(h.client.audio.sending, false);
});

test('S2 late acquire after media becomes unavailable never starts capture and ends local press', async () => {
  const delayed = deferred(); const h = setup(call => call.url.endsWith('/floor/acquire') ? delayed.promise : response(200, {released: true}));
  h.client.audio = mediaMock(); const acquiring = h.client.pressFloor(); const request = h.calls[0].body;
  assert.equal(h.client.audio.sending, false); h.client.audio.ready = false;
  delayed.resolve(response(200, {floor: {...grant(request).floor, peer_id: request.peer_id}})); await acquiring; await flush();
  assert.equal(h.client.pressed, false); assert.equal(h.client.audio.sending, false);
  assert.equal(h.client.audio.events.includes('send:true'), false);
});

test('S2 unknown switch closes media immediately and does not resume before terminal', async () => {
  const h = setup(() => Promise.reject(new Error('response lost'))); h.client.audio = mediaMock();
  await h.client.switchGroup('group_2');
  assert.ok(h.client.pendingSwitch); assert.equal(h.client.audio.events.includes('suspend'), true);
  assert.equal(h.client.audio.events.includes('sync'), false); assert.equal(h.client.canPress(), false);
});

test('S2 server epoch/group change closes old media and then synchronizes confirmed state', () => {
  const h = setup(); h.client.audio = mediaMock();
  h.client.applyState(state({server_epoch: 'restarted_epoch'}), h.client.epoch);
  assert.deepEqual(h.client.audio.events, ['send:false', 'suspend', 'sync']);
  assert.equal(h.client.pressed, false); assert.equal(h.client.state.server_epoch, 'restarted_epoch');
});

test('S2 identity replacement closes media while the original identity is still installed', () => {
  const h = setup(); let stoppedIdentity;
  h.client.audio = mediaMock(); h.client.audio.suspend = () => { stoppedIdentity = h.client.identity.device.id; };
  h.client.load({device: {id: 'new_device', name: '新伙伴'}, token: 'new-synthetic-token'});
  assert.equal(stoppedIdentity, 'device_1'); assert.equal(h.client.pressed, false);
});


test('PTT ignores another touch ending; active pointer cancellation and forced blur/hide still stop', () => {
  const handlers = new Map();
  const button = {addEventListener(type, callback) { handlers.set(type, callback); }, setPointerCapture() {}};
  const client = {pressed: false, presses: 0, stops: 0, canPress() { return !this.pressed; },
    pressFloor() { this.pressed = true; this.presses++; }, cancelFloor() { this.pressed = false; this.stops++; }};
  const forceEnd = bindPTTPointer(button, client);
  const event = pointerId => ({pointerId, button: 0, preventDefault() {}});
  handlers.get('pointerdown')(event(1)); handlers.get('pointerdown')(event(2));
  handlers.get('pointerup')(event(2)); assert.equal(client.pressed, true); assert.equal(client.presses, 1);
  handlers.get('pointerup')(event(1)); assert.equal(client.pressed, false); assert.equal(client.stops, 1);
  for (const type of ['pointercancel', 'lostpointercapture']) {
    handlers.get('pointerdown')(event(3)); handlers.get(type)(event(8)); assert.equal(client.pressed, true);
    handlers.get(type)(event(3)); assert.equal(client.pressed, false);
  }
  handlers.get('pointerdown')(event(4)); forceEnd(); assert.equal(client.pressed, false);
  handlers.get('pointerdown')(event(5)); assert.equal(client.pressed, true); forceEnd();
  assert.equal(client.stops, 5);
});

test('failed pending persistence never installs or submits a switch, even on a later confirmation tick',async()=>{
 const h=setup(call=>response(200,call.url.endsWith('/state')?state():applied(call.body)));
 h.client.onPending=()=>{throw new Error('QuotaExceededError');};
 const started=await h.client.switchGroup('group_2');assert.equal(started,false);assert.equal(h.client.pendingSwitch,null);
 await h.client.confirmSwitch();await h.client.refresh();
 assert.equal(h.calls.filter(c=>c.url.endsWith('/current-group')||c.url.includes('/operations/')).length,0);
 assert.equal(h.timers.size,0);assert.match(h.client.notice,/尚未提交/);
});

test('terminal persistence clear failure keeps original request pending and retries its terminal before media resumes',async()=>{
 let operation;const h=setup(call=>{
  if(call.url.endsWith('/current-group')){operation={...call.body};return response(200,applied(operation));}
  if(call.url.includes('/operations/'))return response(200,{operation:applied(operation).operation});
  if(call.url.endsWith('/state'))return response(200,state({current_group_id:'group_2',current_version:2}));
  return response(200,{});
 });
 let durable=null,failClear=true;
 h.client.onPending=value=>{if(!value&&failClear)throw new Error('SecurityError');durable=value&&{...value};};
 await h.client.switchGroup('group_2');assert.deepEqual(h.client.pendingSwitch,operation);assert.deepEqual(durable,operation);
 assert.equal(h.client.state.current_group_id,'group_1');assert.equal(h.client.canPress(),false);
 failClear=false;await h.client.confirmSwitch();assert.equal(h.client.pendingSwitch,null);assert.equal(durable,null);
 assert.equal(h.client.state.current_group_id,'group_2');assert.equal(h.calls.filter(c=>c.url.endsWith('/current-group')).length,1);
 assert.equal(h.calls.filter(c=>c.url.includes('/operations/')).length,1);
});
