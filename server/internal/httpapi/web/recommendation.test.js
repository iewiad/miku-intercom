import test from 'node:test';
import assert from 'node:assert/strict';
import {ControlClient, APIError} from './app.js';
import {RecommendationClient, validNeed, validVoiceTarget, voiceTargetReason} from './recommendation.js';

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

test('cancelled or forged voice handoff cannot become an ordinary text request after the client is idle', async () => {
  const {c, r, requests} = setup();
  c.voice = {blocking: false, canHandoffNeed: () => false, interruptNeed() {}};
  const reservation = r.reserveVoiceNeed(); assert.ok(reservation); r.interrupt('取消');
  assert.equal(await r.request('迟到的语音需求', reservation), false);
  assert.equal(await r.request('伪造的语音需求', {...reservation}), false);
  assert.deepEqual(requests, []); assert.equal(r.result, null);
});

function targetSetup(source = 'exact') {
  const f = setup(), original = f.c.request;
  f.r.available = false; f.r.phase = 'unavailable';
  f.c.voice = {available: true, blocking: false, canHandoffVoice: () => true};
  const reservation = f.r.reserveVoice('target');
  f.c.voice.blocking = true;
  f.c.request = async (path, method, body, options) => {
    if (path !== '/voice/proposals') return original(path, method, body, options);
    f.requests.push({path, method, body, options});
    const result = f.result(body.request_id); result.source = source;
    result.suggestions[0].reason = voiceTargetReason;
    return {recommendation: result};
  };
  return {...f, reservation};
}

test('target bounds reject control characters and invalid Unicode without changing S5 need rules', () => {
  assert.equal(validVoiceTarget('切到维修组'), true);
  assert.equal(validVoiceTarget('😀'.repeat(256)), true);
  assert.equal(validVoiceTarget('\n  切到维修组\r\n'), true);
  assert.equal(validVoiceTarget(' '.repeat(300) + '😀'.repeat(256) + '\n'), true);
  for (const text of ['', ' ', '切到\n维修组', '\u0000维修组', '\ud800', '😀'.repeat(257)]) assert.equal(validVoiceTarget(text), false);
  for (const text of ['\n \r\t', ' \ud800\n', ' 切到\t维修组 ', ' ' + '😀'.repeat(257) + '\n']) assert.equal(validVoiceTarget(text), false);
  assert.equal(validNeed('中'.repeat(512)), true);
});

for (const source of ['exact', 'model']) test(`${source} voice target works without S5 model capability and still needs explicit confirmation`, async () => {
  const f = targetSetup(source);
  assert.ok(f.reservation); assert.equal(f.reservation.mode, 'target');
  assert.equal(await f.r.requestVoiceTarget('\n 切到维修组 \n', f.reservation), true);
  assert.equal(f.r.result.source, source); assert.equal(f.r.phase, 'review');
  assert.equal(f.c.state.current_group_id, 'group_one'); assert.equal(f.c.pendingSwitch, null);
  assert.deepEqual(f.requests[0].body, {request_id: 'synthetic_1', transcript: '切到维修组', expected_version: 2, server_epoch: 'epoch_one'});
  assert.equal(f.r.targetTranscript, f.requests[0].body.transcript);
  assert.equal(await f.r.confirm('group_two'), false); // Cleanup handoff has not released the voice owner yet.
  f.c.voice.blocking = false;
  assert.equal(await f.r.confirm('group_two'), true);
  assert.equal(f.r.targetTranscript, '');
  assert.equal(f.requests.filter(x => x.path === '/recommendations/confirm').length, 1);
  assert.equal(f.requests.some(x => x.path === '/current-group' || x.path === '/voice/resolve'), false);
});

test('voice target source and fixed reason are verified before review, including exact fast path', async () => {
  for (const change of ['missing_source', 'foreign_source', 'reason', 'exact_empty', 'exact_multiple']) {
    const f = targetSetup(), original = f.c.request;
    f.c.request = async (...args) => {
      const out = await original(...args);
      if (change === 'missing_source') delete out.recommendation.source;
      if (change === 'foreign_source') out.recommendation.source = 'other';
      if (change === 'reason') out.recommendation.suggestions[0].reason = '模型声称已经切换';
      if (change === 'exact_empty') out.recommendation.suggestions = [];
      if (change === 'exact_multiple') out.recommendation.suggestions.push({group_id: 'group_one', name: '全员组', purpose: '', reason: voiceTargetReason});
      return out;
    };
    assert.equal(await f.r.requestVoiceTarget('切到维修组', f.reservation), false, change);
    assert.equal(f.r.result, null); assert.equal(f.c.pendingSwitch, null);
  }
});

test('a target reservation cannot survive version, epoch, identity or authorized directory changes during capture', async () => {
  for (const change of ['version', 'epoch', 'identity', 'name', 'purpose', 'membership']) {
    const f = targetSetup();
    if (change === 'version') f.c.state.current_version++;
    if (change === 'epoch') f.c.state.server_epoch = 'new_epoch';
    if (change === 'identity') f.c.identity.token = 'new_identity';
    if (change === 'name') f.c.state.groups[1].name = '改名';
    if (change === 'purpose') f.c.state.groups[1].purpose = '改职责';
    if (change === 'membership') f.c.state.groups.pop();
    f.r.observeState();
    assert.equal(await f.r.requestVoiceTarget('切到维修组', f.reservation), false, change);
    assert.equal(f.requests.length, 0); assert.equal(f.r.result, null);
  }
});

test('cancel, ownership loss and expiry discard target tokens; late or forged handoffs never reopen review', async () => {
  const f = targetSetup(), late = deferred(), original = f.c.request;
  f.c.request = async (...args) => { await late.promise; return original(...args); };
  const request = f.r.requestVoiceTarget('切到维修组', f.reservation);
  assert.equal(f.r.targetTranscript, '切到维修组');
  f.r.interrupt('已取消核对'); late.resolve();
  assert.equal(await request, false); assert.equal(f.r.result, null);
  assert.equal(f.r.targetTranscript, '');
  assert.equal(await f.r.requestVoiceTarget('切到维修组', {...f.reservation}), false);
  const expired = targetSetup(); await expired.r.requestVoiceTarget('切到维修组', expired.reservation);
  expired.c.voice.blocking = false; expired.advance(60001);
  assert.equal(await expired.r.confirm('group_two'), false); assert.equal(expired.r.result, null);
  assert.equal(expired.requests.some(x => x.path === '/recommendations/confirm'), false);
});

test('target empty, negated and failed terminals retain only the submitted text without confirmation authority', async () => {
  for (const outcome of ['empty', 'negated', 'failure', 'wrong_status']) {
    const f = targetSetup('model'), original = f.c.request;
    f.c.request = async (...args) => {
      if (outcome === 'negated') throw new APIError(422, 'voice_intent_negated', 'synthetic');
      if (outcome === 'wrong_status') throw new APIError(500, 'voice_intent_negated', 'synthetic');
      if (outcome === 'failure') throw new APIError(503, 'voice_unavailable', 'synthetic');
      const response = await original(...args); response.recommendation.suggestions = []; return response;
    };
    assert.equal(await f.r.requestVoiceTarget(' \n 不要切到维修组 \n', f.reservation), ['empty', 'negated'].includes(outcome), outcome);
    assert.equal(f.r.targetTranscript, '不要切到维修组', outcome);
    assert.equal(f.r.blocking, false, outcome); assert.equal(f.r.result, null, outcome);
    f.c.voice.blocking = false;
    assert.equal(await f.r.confirm('group_two'), false, outcome);
    assert.equal(f.c.pendingSwitch, null, outcome);
    if (outcome === 'negated') assert.equal(f.r.message, '已识别为不切群，当前群保持不变。');
    else assert.doesNotMatch(f.r.message, /已识别为不切群/);
    f.r.interrupt(); assert.equal(f.r.targetTranscript, '');
  }
});

test('terminal target text is cleared on binding loss, a new start, or explicit close', async () => {
  for (const change of ['identity', 'epoch', 'version', 'directory', 'ownership', 'start', 'close']) {
    const f = targetSetup('model');
    f.c.request = async () => { throw new APIError(422, 'voice_intent_negated', 'synthetic'); };
    await f.r.requestVoiceTarget('不要切群', f.reservation); f.c.voice.blocking = false;
    if (change === 'identity') f.c.identity.token = 'other';
    if (change === 'epoch') f.c.state.server_epoch = 'other';
    if (change === 'version') ++f.c.state.current_version;
    if (change === 'directory') f.c.state.groups[1].purpose = 'new';
    if (change === 'ownership') f.c.authorize = () => false;
    if (change === 'start') assert.ok(f.r.reserveVoice('target'));
    else if (change === 'close') f.r.interrupt();
    else f.r.observeState();
    assert.equal(f.r.targetTranscript, '', change); assert.equal(f.r.result, null, change);
  }
});

test('a late negated response cannot restore cleared text or overwrite a newer target intent', async () => {
  for (const next of ['close', 'start', 'binding']) {
    const f = targetSetup(), wait = deferred();
    f.c.request = async () => { await wait.promise; throw new APIError(422, 'voice_intent_negated', 'synthetic'); };
    const pending = f.r.requestVoiceTarget('不要切群', f.reservation);
    f.r.interrupt(); f.c.voice.blocking = false;
    if (next === 'start') assert.ok(f.r.reserveVoice('target'));
    if (next === 'binding') { ++f.c.state.current_version; f.r.observeState(); }
    const generation = f.r.generation; wait.resolve();
    assert.equal(await pending, false); assert.equal(f.r.generation, generation);
    assert.equal(f.r.targetTranscript, ''); assert.equal(f.r.result, null);
    assert.notEqual(f.r.message, '已识别为不切群，当前群保持不变。');
  }
});
