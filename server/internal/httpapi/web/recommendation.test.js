import test from 'node:test';
import assert from 'node:assert/strict';
import {ControlClient, APIError} from './app.js';
import {RecommendationClient, validNeed} from './recommendation.js';

const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return {promise, resolve}; };
const baseTime = Date.parse('2026-10-25T03:00:00Z');
function setup() {
  let now = baseTime, sequence = 0, saved = null;
  const requests = [], timers = [], audio = {setSending() {}, suspend() {}, sync() {}, isReady: () => true};
  const c = new ControlClient({now: () => now, random: () => `synthetic_${++sequence}`, audio,
    onPending: pending => { saved = pending; }, schedule: fn => { timers.push(fn); return timers.length; }, cancel() {}});
  c.load({device: {id: 'device_one', name: '我'}, token: 'synthetic-token'});
  c.state = {device: c.identity.device, server_epoch: 'epoch_one', server_time: new Date(now).toISOString(), current_version: 2,
    current_group_id: 'group_one', groups: [{id: 'group_one', name: '全员组'}, {id: 'group_two', name: '维修组', purpose: '设备排障'}]};
  c.stateAt = now; c.connected = true;
  const r = new RecommendationClient({control: c}); c.recommendation = r; r.available = true; r.phase = 'idle';
  const result = requestID => ({recommendation_id: 'recommendation_one', request_id: requestID, server_epoch: 'epoch_one', current_version: 2,
    candidate_digest: 'a'.repeat(64), expires_at: new Date(baseTime + 60000).toISOString(), suggestions: [{group_id: 'group_two', name: '维修组', purpose: '设备排障', reason: '职责涵盖设备问题'}]});
  c.request = async (path, method, body, options) => {
    requests.push({path, method, body, options});
    if (path === '/recommendations') return method === 'POST' ? {recommendation: result(body.request_id)} : {available: true};
    if (path === '/recommendations/confirm') return {operation: {...body, recommendation_epoch: body.server_epoch, status: 'applied'}, state: {...c.state, current_group_id: body.group_id, current_version: 3}};
    throw new Error('unexpected request');
  };
  return {c, r, requests, result, timers, saved: () => saved, advance: ms => { now += ms; }};
}

test('explicit need is bounded; requesting/review never switch, capture, or acquire PTT', async () => {
  const {c, r, requests} = setup();
  assert.equal(validNeed('设备坏了'), true); assert.equal(validNeed(' '.repeat(4)), false);
  assert.equal(validNeed('中'.repeat(512)), true); assert.equal(validNeed('中'.repeat(513)), false); assert.equal(validNeed('设备\n维修'), false);
  assert.equal(await r.request('设备坏了'), true); assert.equal(r.phase, 'review');
  assert.equal(c.state.current_group_id, 'group_one'); assert.equal(c.canPress(), false);
  assert.equal(await c.switchGroup('group_two'), false); await c.joinGroup('ABCDEFGH');
  assert.deepEqual(requests.map(item => item.path), ['/recommendations']);
  assert.equal(requests[0].options.timeoutMs, 35000);
  r.interrupt('取消'); assert.equal(c.canPress(), true); assert.equal(c.state.current_version, 2);
});

test('cancel and membership change reject late model output even at unchanged current version', async () => {
  for (const change of ['cancel', 'membership', 'purpose', 'identity', 'epoch']) {
    const {c, r, result} = setup(), wait = deferred();
    c.request = async (_, __, body) => { await wait.promise; return {recommendation: result(body.request_id)}; };
    const pending = r.request('需要维修');
    if (change === 'cancel') r.interrupt();
    if (change === 'membership') { c.state = {...c.state, groups: [...c.state.groups, {id: 'group_three', name: '后勤'}]}; r.observeState(); }
    if (change === 'purpose') { c.state.groups[1].purpose = '改了职责'; r.observeState(); }
    if (change === 'identity') c.identity = {...c.identity, token: 'new-token'};
    if (change === 'epoch') c.state.server_epoch = 'epoch_new';
    wait.resolve(); assert.equal(await pending, false, change); assert.equal(r.result, null, change);
  }
});

test('untrusted recommendations fail closed for foreign, duplicate, malformed, expired and overlong output', async () => {
  const variants = [r => r.suggestions[0].group_id = 'foreign_group', r => r.suggestions.push({...r.suggestions[0]}),
    r => r.suggestions[0].name = '另一个名字', r => r.suggestions[0].purpose = '伪造职责',
    r => r.suggestions[0].reason = '字'.repeat(257), r => r.suggestions[0].reason = '不可信\u0000内容',
    r => r.candidate_digest = 'not-digest', r => r.expires_at = 'bad-date', r => r.expires_at = new Date(baseTime).toISOString(),
    r => r.server_epoch = 'stale_epoch', r => r.request_id = 'different_request'];
  for (const mutate of variants) {
    const {c, r, result} = setup();
    c.request = async (_, __, body) => { const rec = result(body.request_id); mutate(rec); return {recommendation: rec}; };
    assert.equal(await r.request('设备故障'), false); assert.equal(r.result, null); assert.equal(c.pendingSwitch, null);
  }
});

test('no match and provider failure give no suggestion or switch', async () => {
  const {c, r, result} = setup();
  c.request = async (_, __, body) => ({recommendation: {...result(body.request_id), suggestions: []}});
  assert.equal(await r.request('需要其他帮助'), true); assert.match(r.message, /没有找到/); assert.equal(r.blocking, false);
  c.request = async () => { throw new APIError(503, 'recommendation_unavailable', 'unavailable'); };
  assert.equal(await r.request('设备故障'), false); assert.match(r.message, /暂不可用/); assert.equal(c.pendingSwitch, null);
});

test('explicit confirmation persists exact payload and uses recommendation endpoint before latest-state recovery', async () => {
  const {c, r, requests, saved} = setup(); await r.request('设备故障');
  assert.equal(await r.confirm('foreign_group'), false);
  assert.equal(await r.confirm('group_two'), true);
  const confirmed = requests.find(item => item.path === '/recommendations/confirm');
  assert.deepEqual(confirmed.body, {operation_id: 'synthetic_2', group_id: 'group_two', expected_version: 2,
    recommendation_id: 'recommendation_one', server_epoch: 'epoch_one'});
  assert.equal(c.state.current_group_id, 'group_two'); assert.equal(c.state.current_version, 3);
  assert.equal(c.pressed, false); assert.equal(c.pendingSwitch, null); assert.equal(saved(), null);
});

test('expiry and local persistence failure prevent submission', async () => {
  const expired = setup(); await expired.r.request('设备故障'); expired.advance(60001);
  assert.equal(await expired.r.confirm('group_two'), false); assert.equal(expired.requests.length, 1);
  const blocked = setup(); await blocked.r.request('设备故障'); blocked.c.onPending = () => { throw new Error('storage full'); };
  assert.equal(await blocked.r.confirm('group_two'), false); assert.equal(blocked.requests.length, 1); assert.equal(blocked.r.phase, 'review');
});

test('lost confirm response/reload looks up original terminal even after epoch and expiry, never falls back to manual switch', async () => {
  const {c, r, requests, saved} = setup(); await r.request('设备故障');
  const request = c.request;
  c.request = async (...args) => { if (args[0] === '/recommendations/confirm') throw new Error('response lost'); return request(...args); };
  await r.confirm('group_two'); const pending = {...saved()}; assert.equal(c.pendingSwitch.recommendation_id, 'recommendation_one');
  const recovered = setup(); recovered.c.pendingSwitch = pending; recovered.c.state.server_epoch = 'epoch_after_restart'; recovered.advance(90000);
  const terminal = {...pending, recommendation_epoch: pending.server_epoch, status: 'applied'}; const latest = {...recovered.c.state, current_group_id: 'group_two', current_version: 3};
  const replayPaths = [];
  recovered.c.request = async path => { replayPaths.push(path); return path.startsWith('/operations/') ? {operation: terminal} : latest; };
  await recovered.c.confirmSwitch();
  assert.deepEqual(replayPaths, [`/operations/${pending.operation_id}`, '/state']); assert.equal(recovered.c.pendingSwitch, null);
  assert.equal(recovered.c.state.current_version, 3); assert.equal(requests.some(item => item.path === '/current-group'), false);
});

test('unknown operation retries same recommendation payload; unrelated terminal cannot unlock media', async () => {
  const {c, r, saved} = setup(); await r.request('设备故障');
  c.request = async () => { throw new Error('response lost'); }; await r.confirm('group_two');
  const pending = {...saved()}, paths = [];
  c.request = async (path, method, body) => {
    paths.push({path, body});
    if (path.startsWith('/operations/')) throw new APIError(404, 'operation_not_found', 'missing');
    return {operation: {...body, recommendation_id: 'other_rec', status: 'applied'}, state: c.state};
  };
  await c.confirmSwitch(); assert.equal(paths[1].path, '/recommendations/confirm'); assert.deepEqual(paths[1].body, pending);
  assert.deepEqual(c.pendingSwitch, pending); assert.equal(c.canPress(), false);
});

test('terminal from a different recommendation epoch cannot clear the saved confirmation', async () => {
  const {c, r} = setup(); await r.request('设备故障');
  c.request = async (_, __, body) => ({operation: {...body, recommendation_epoch: 'wrong_epoch', status: 'applied'}, state: c.state});
  await r.confirm('group_two'); assert.ok(c.pendingSwitch); assert.equal(c.canPress(), false);
});
