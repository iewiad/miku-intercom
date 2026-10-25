import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {voiceTargetReason} from './recommendation.js';

const flush = async () => { for (let i = 0; i < 150; i++) await Promise.resolve(); };
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return {promise, resolve}; };

// Boot the real product. DOM, media devices, Peer and HTTP are inert doubles;
// no browser, network, microphone, playback or hardware boundary is exercised.
async function product({recommendationAvailable = true, proposalSource = 'exact'} = {}) {
  const originals = new Map(), install = (key, value) => {
    originals.set(key, Object.getOwnPropertyDescriptor(globalThis, key));
    Object.defineProperty(globalThis, key, {value, configurable: true, writable: true});
  };
  const documentEvents = {}, windowEvents = {}, requests = [], tracks = [], peers = [], timers = new Map();
  let timerID = 0;
  class Element {
    constructor(tag, attrs = '') { this.tag = tag; this.textContent = ''; this.value = ''; this.children = []; this.listeners = {}; this.dataset = {}; this.hidden = /\bhidden\b/.test(attrs); }
    set innerHTML(_) { throw new Error('Untrusted recommendation must stay text'); }
    append(...values) { this.children.push(...values); } replaceChildren(...values) { this.children = values; }
    addEventListener(type, callback) { (this.listeners[type] ??= []).push(callback); }
    dispatch(type) { for (const callback of this.listeners[type] || []) callback({preventDefault() {}}); }
    setAttribute() {} setCustomValidity() {} focus() {} pause() {} play() { return Promise.resolve(); }
    setPointerCapture() {}
  }
  const nodes = new Map(), html = await readFile(new URL('./index.html', import.meta.url), 'utf8');
  for (const match of html.matchAll(/<([a-z][\w-]*)\b([^>]*\bid="([^"]+)"[^>]*)>/g)) nodes.set(match[3], new Element(match[1], match[2]));
  const $ = id => nodes.get(id), identity = {device: {id: 'need_device', name: '测试'}, token: 'synthetic-need-token'};
  const store = new Map([['shenglu.control.identity.v1', JSON.stringify(identity)]]);
  const document = {hidden: false, getElementById: $, createElement: tag => new Element(tag), addEventListener(type, callback) { documentEvents[type] = callback; }};
  const settings = {stop: null, state: null, model: null, proposal: null, proposalError: null, emptyProposal: false, confirm: null};
  let state = {device: identity.device, server_epoch: 'need_epoch', server_time: new Date().toISOString(), current_group_id: 'group_one', current_version: 1,
    groups: [{id: 'group_one', name: '当前群', member_count: 1}, {id: 'group_two', name: '<维修组>', purpose: '设备排障', member_count: 1}]};
  let media = null, commandID = '';
  const result = (requestID, target = false) => ({recommendation: {recommendation_id: 'need_recommendation', request_id: requestID, server_epoch: state.server_epoch,
    current_version: state.current_version, candidate_digest: 'a'.repeat(64), expires_at: new Date(Date.now() + 60000).toISOString(),
    ...(target ? {source: proposalSource} : {}),
    suggestions: [{group_id: 'group_two', name: '<维修组>', purpose: '设备排障', reason: target ? voiceTargetReason : '<b>职责涵盖设备问题</b>'}]}});
  class Peer {
    constructor() { this.connectionState = 'connected'; this.iceGatheringState = 'complete'; peers.push(this); }
    addTransceiver() { return {setCodecPreferences() {}}; }
    createOffer() { return Promise.resolve({type: 'offer', sdp: 'v=0'}); }
    async setLocalDescription(value) { this.localDescription = value; }
    createDataChannel() { this.channel = {send() {}, close() {}}; return this.channel; }
    async setRemoteDescription() {
      if (this.channel) { this.channel.onopen(); this.channel.onmessage({data: JSON.stringify({type: 'session.updated'})}); }
      else { const track = {enabled: true, kind: 'audio'}; await this.ontrack({track, streams: [{getTracks: () => [track]}]}); }
    }
    close() { this.connectionState = 'closed'; }
  }
  try {
    install('document', document); install('window', {addEventListener(type, callback) { windowEvents[type] = callback; }});
    install('localStorage', {getItem: key => store.get(key) || null, setItem: (key, value) => store.set(key, value), removeItem: key => store.delete(key)});
    install('setInterval', () => 1); install('setTimeout', (fn, ms) => { const id = ++timerID; timers.set(id, {fn, ms}); return id; }); install('clearTimeout', id => timers.delete(id));
    install('navigator', {mediaDevices: {getUserMedia: async () => {
      const track = {enabled: true, kind: 'audio', stopped: false, stop() { this.stopped = true; }}; tracks.push(track);
      return {getAudioTracks: () => [track], getTracks: () => [track]};
    }}});
    install('RTCPeerConnection', Peer); install('RTCRtpReceiver', {getCapabilities: () => ({codecs: [{mimeType: 'audio/opus'}]})});
    install('fetch', async (url, options = {}) => {
      const path = url.replace('/api/v1', ''), body = options.body && JSON.parse(options.body); requests.push({path, method: options.method, body});
      let value;
      if (path === '/voice') value = {available: true, command_id: ''};
      else if (path === '/recommendations' && options.method === 'GET') value = {available: recommendationAvailable};
      else if (path === '/state') value = settings.state ? await settings.state.promise : state;
      else if (path === '/media') value = {available: true, server_epoch: state.server_epoch, peer_id: media?.peer_id || ''};
      else if (path === '/media/peers') { media = body; value = {...body, answer: {type: 'answer', sdp: 'v=0'}}; }
      else if (path.startsWith('/media/peers/')) { media = null; value = {closed: true}; }
      else if (path === '/voice/sessions') { commandID = body.command_id; value = {command_id: commandID}; }
      else if (path.endsWith('/offer')) value = {type: 'answer', sdp: 'v=0'};
      else if (path.endsWith('/stop')) { if (settings.stop) await settings.stop.promise; value = {stopped: true, command_id: commandID}; }
      else if (path === '/recommendations') { if (settings.model) await settings.model.promise; value = result(body.request_id); }
      else if (path === '/voice/proposals') {
        if (settings.proposal) await settings.proposal.promise;
        if (settings.proposalError) return {ok: false, status: 422, json: async () => ({error: {code: settings.proposalError, message: 'synthetic'}})};
        value = result(body.request_id, true);
        if (settings.emptyProposal) value.recommendation.suggestions = [];
      }
      else if (path === '/recommendations/confirm') {
        if (settings.confirm) await settings.confirm.promise;
        state = {...state, current_group_id: body.group_id, current_version: state.current_version + 1};
        value = {operation: {...body, recommendation_epoch: body.server_epoch, status: 'applied'}, state};
      } else throw new Error('Unexpected offline product request');
      return {ok: true, status: 200, json: async () => value};
    });
    await import(`./app.js?product-voice-need=${crypto.randomUUID()}`); await flush();
    return {$, identity, store, settings, requests, tracks, peers, document, documentEvents, windowEvents, get state() { return state; },
      final(text = '设备没有声音，需要维修') { peers.findLast(peer => peer.channel).channel.onmessage({data: JSON.stringify({type: 'conversation.item.input_audio_transcription.completed', transcript: text})}); },
      restore() { for (const [key, descriptor] of originals) { if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key]; } }};
  } catch (error) {
    for (const [key, descriptor] of originals) { if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key]; }
    throw error;
  }
}

test('product needs explicit speech action, waits for cleanup and state, shows plain-text reasons, and requires durable confirmation', async () => {
  const f = await product();
  try {
    assert.equal(f.tracks.length, 0); assert.equal(f.$('recommendation-voice').disabled, false);
    f.$('audio-enable').dispatch('click'); await flush();
    assert.equal(f.requests.some(r => r.path === '/voice/sessions' || r.path === '/recommendations' && r.method === 'POST'), false);
    f.$('recommendation-voice').dispatch('click'); await flush();
    assert.match(f.$('voice-status').textContent, /描述需要的帮助/); assert.equal(f.$('voice-start').disabled, true);
    assert.equal(f.$('floor-label').textContent, '正在语音找组');
    const asrTrack = f.tracks.at(-1), asrPeer = f.peers.at(-1); assert.equal(asrTrack.enabled, true);
    f.settings.stop = deferred(); f.settings.state = deferred(); f.final(); await flush();
    assert.equal(asrTrack.stopped, true); assert.equal(asrTrack.enabled, false); assert.equal(asrPeer.connectionState, 'closed');
    assert.equal(f.requests.some(r => r.path === '/recommendations' && r.method === 'POST'), false);
    f.settings.stop.resolve(); await flush(); assert.equal(f.requests.some(r => r.path === '/recommendations' && r.method === 'POST'), false);
    f.settings.state.resolve(f.state); await flush();
    const row = f.$('recommendation-results').children[0]; assert.equal(row.children[2].textContent, '推荐说明：<b>职责涵盖设备问题</b>');
    assert.equal(f.$('current-name').textContent, '当前群'); assert.equal(f.$('ptt').disabled, true);
    assert.equal(f.requests.some(r => r.path === '/voice/resolve' || r.path === '/current-group' || r.path === '/recommendations/confirm'), false);
    assert.equal([...f.store.values()].some(value => value.includes('设备没有声音')), false);
    f.settings.confirm = deferred(); row.children.at(-1).dispatch('click'); await flush();
    const pending = JSON.parse(f.store.get('shenglu.control.pending.v1.need_device'));
    assert.equal(pending.recommendation_id, 'need_recommendation'); assert.equal(pending.group_id, 'group_two'); assert.equal(f.$('current-name').textContent, '当前群');
    f.settings.confirm.resolve(); await flush();
    assert.equal(f.$('current-name').textContent, '<维修组>'); assert.equal(f.store.has('shenglu.control.pending.v1.need_device'), false);
    assert.equal(f.tracks.at(-1).enabled, false); assert.equal(f.requests.some(r => r.path.startsWith('/floor/')), false);
    assert.equal(f.requests.filter(r => r.path === '/recommendations' && r.method === 'POST').length, 1);
  } finally { f.restore(); }
});

test('product hidden, audio-off, owner loss and identity update during ASR cleanup cannot launch late recommendations', async () => {
  for (const action of ['hidden', 'audio-off', 'owner', 'identity']) {
    const f = await product();
    try {
      f.$('recommendation-voice').dispatch('click'); await flush();
      const late = f.peers.at(-1).channel.onmessage; f.settings.stop = deferred(); f.final(); await flush();
      if (action === 'hidden') { f.document.hidden = true; f.documentEvents.visibilitychange(); }
      if (action === 'audio-off') f.$('audio-disable').dispatch('click');
      if (action === 'owner') { f.store.set('shenglu.control.owner.v1.need_device', 'other_page'); f.windowEvents.storage({key: 'shenglu.control.owner.v1.need_device', newValue: 'other_page'}); }
      if (action === 'identity') { f.store.set('shenglu.control.identity.v1', JSON.stringify({...f.identity, token: 'new_token'})); f.windowEvents.storage({key: 'shenglu.control.identity.v1', newValue: 'updated'}); }
      f.settings.stop.resolve(); await flush();
      late({data: JSON.stringify({type: 'session.updated'})}); late({data: JSON.stringify({type: 'conversation.item.input_audio_transcription.done', transcript: '迟到文本'})}); await flush();
      assert.equal(f.tracks.at(-1).enabled, false, action); assert.equal(f.tracks.at(-1).stopped, true, action);
      assert.equal(f.requests.some(r => r.path === '/recommendations' && r.method === 'POST'), false, action);
      assert.equal(f.requests.some(r => r.path === '/current-group' || r.path === '/recommendations/confirm'), false, action);
      assert.equal(f.$('recommendation-results').children.length, 0, action);
    } finally { f.restore(); }
  }
});

test('product audio-off after handoff discards late model output without a confirmation or new capture', async () => {
  const f = await product();
  try {
    f.$('recommendation-voice').dispatch('click'); await flush(); f.settings.model = deferred(); f.final(); await flush();
    assert.equal(f.requests.filter(r => r.path === '/recommendations' && r.method === 'POST').length, 1);
    const captures = f.tracks.length; f.$('audio-disable').dispatch('click'); f.settings.model.resolve(); await flush();
    assert.equal(f.$('recommendation-results').children.length, 0); assert.equal(f.tracks.length, captures);
    assert.equal(f.requests.some(r => r.path === '/recommendations/confirm' || r.path === '/current-group'), false);
  } finally { f.restore(); }
});

for (const source of ['exact', 'model']) test(`product ${source} voice target has its own plain-text review and only a separate confirmation switches`, async () => {
  const f = await product({recommendationAvailable: source === 'model', proposalSource: source});
  try {
    assert.equal(f.tracks.length, 0); assert.equal(f.$('voice-start').disabled, false);
    f.$('voice-start').dispatch('click'); await flush();
    assert.match(f.$('voice-status').textContent, /识别后核对确认/);
    f.settings.stop = deferred(); f.settings.state = deferred(); f.final('\n 切到<维修组> \n'); await flush();
    assert.equal(f.tracks.at(-1).stopped, true); assert.equal(f.peers.at(-1).connectionState, 'closed');
    assert.equal(f.requests.some(r => r.path === '/voice/proposals'), false);
    f.settings.stop.resolve(); await flush();
    assert.equal(f.requests.some(r => r.path === '/voice/proposals'), false);
    f.settings.state.resolve(f.state); await flush();
    assert.equal(f.$('voice-proposal-panel').hidden, false);
    assert.equal(f.$('voice-proposal-transcript').textContent, '切到<维修组>');
    assert.equal(f.$('voice-proposal-transcript-area').hidden, false);
    assert.match(f.$('voice-proposal-source').textContent, source === 'exact' ? /群名匹配/ : /模型匹配/);
    const row = f.$('voice-proposal-results').children[0];
    assert.equal(row.children[0].textContent, '识别目标群：<维修组>');
    assert.equal(row.children[2].textContent, voiceTargetReason);
    assert.equal(f.$('recommendation-results').children.length, 0);
    row.dispatch('click'); await flush();
    assert.equal(f.$('current-name').textContent, '当前群');
    assert.equal(f.requests.some(r => r.path === '/recommendations/confirm' || r.path === '/current-group' || r.path === '/voice/resolve'), false);
    const lookup = f.requests.find(r => r.path === '/voice/proposals');
    assert.equal(lookup.body.transcript, '切到<维修组>');
    assert.equal(f.$('voice-proposal-transcript').textContent, lookup.body.transcript);
    assert.equal(lookup.body.expected_version, 1); assert.equal(lookup.body.server_epoch, 'need_epoch');
    assert.equal([...f.store.values()].some(value => value.includes('切到<维修组>')), false);
    f.settings.confirm = deferred(); row.children.at(-1).dispatch('click'); await flush();
    const pending = JSON.parse(f.store.get('shenglu.control.pending.v1.need_device'));
    assert.equal(pending.recommendation_id, 'need_recommendation');
    assert.equal(f.$('voice-proposal-panel').hidden, true);
    assert.equal(f.$('voice-proposal-transcript').textContent, '');
    f.$('voice-proposal-cancel').dispatch('click');
    assert.equal(f.store.has('shenglu.control.pending.v1.need_device'), true);
    f.settings.confirm.resolve(); await flush();
    assert.equal(f.$('current-name').textContent, '<维修组>');
    assert.equal(f.requests.filter(r => r.path === '/recommendations/confirm').length, 1);
    assert.equal(f.store.has('shenglu.control.pending.v1.need_device'), false);
    assert.equal(f.tracks.every(track => track.enabled === false && track.stopped), true);
  } finally { f.restore(); }
});

test('product target cancel, hidden, audio-off, owner, identity, version and catalog changes discard late proposal output', async () => {
  for (const action of ['cancel', 'hidden', 'audio-off', 'owner', 'identity', 'version', 'catalog']) {
    const f = await product();
    try {
      f.$('voice-start').dispatch('click'); await flush();
      f.settings.proposal = deferred(); f.final('切到<维修组>'); await flush();
      assert.equal(f.requests.filter(r => r.path === '/voice/proposals').length, 1);
      if (action === 'cancel') f.$('voice-proposal-cancel').dispatch('click');
      if (action === 'hidden') { f.document.hidden = true; f.documentEvents.visibilitychange(); }
      if (action === 'audio-off') f.$('audio-disable').dispatch('click');
      if (action === 'owner') { f.store.set('shenglu.control.owner.v1.need_device', 'other_page'); f.windowEvents.storage({key: 'shenglu.control.owner.v1.need_device', newValue: 'other_page'}); }
      if (action === 'identity') { f.store.set('shenglu.control.identity.v1', JSON.stringify({...f.identity, token: 'new_token'})); f.windowEvents.storage({key: 'shenglu.control.identity.v1', newValue: 'updated'}); }
      if (action === 'version') { f.state.current_version++; f.$('refresh-state').dispatch('click'); await flush(); }
      if (action === 'catalog') { f.state.groups[1].purpose = '修改职责'; f.$('refresh-state').dispatch('click'); await flush(); }
      f.settings.proposal.resolve(); await flush();
      assert.equal(f.$('voice-proposal-results').children.length, 0, action);
      assert.equal(f.$('voice-proposal-panel').hidden, true, action);
      assert.equal(f.$('voice-proposal-transcript').textContent, '', action);
      assert.equal(f.requests.some(r => r.path === '/recommendations/confirm' || r.path === '/current-group'), false, action);
      assert.equal(f.tracks.every(track => !track.enabled && track.stopped), true, action);
    } finally { f.restore(); }
  }
});

for (const outcome of ['empty', 'negated']) test(`product ${outcome} target retains the complete submitted plain text with a nonblocking close action`, async () => {
  const f = await product({proposalSource: 'model'});
  try {
    f.$('voice-start').dispatch('click'); await flush();
    f.settings.proposal = deferred();
    f.settings.emptyProposal = outcome === 'empty';
    if (outcome === 'negated') f.settings.proposalError = 'voice_intent_negated';
    const transcript = '切到<维修组><script>不应执行</script>，' + '这是需要核对的识别文字'.repeat(15) + '，等等，不要切群';
    assert.ok([...transcript].length <= 256);
    f.final(` \n ${transcript} \n `); await flush();
    assert.equal(f.$('voice-proposal-transcript').textContent, transcript);
    f.settings.proposal.resolve(); await flush();
    assert.equal(f.$('voice-proposal-panel').hidden, false);
    assert.equal(f.$('voice-proposal-transcript-area').hidden, false);
    assert.equal(f.$('voice-proposal-transcript').textContent, transcript);
    assert.ok(f.$('voice-proposal-transcript').textContent.endsWith('，等等，不要切群'));
    assert.equal(f.$('voice-proposal-transcript').children.length, 0);
    assert.equal(f.$('voice-proposal-transcript').textContent, f.requests.find(r => r.path === '/voice/proposals').body.transcript);
    assert.equal(f.$('voice-proposal-results').children.length, 0);
    assert.equal(f.$('voice-proposal-source').textContent, '');
    assert.equal(f.$('voice-proposal-cancel').hidden, false);
    assert.equal(f.$('voice-proposal-cancel').textContent, '返回对讲 / 关闭');
    assert.equal(f.$('voice-start').disabled, false);
    if (outcome === 'negated') assert.equal(f.$('voice-proposal-status').textContent, '已识别为不切群，当前群保持不变。');
    else assert.match(f.$('voice-proposal-status').textContent, /没有找到/);
    assert.equal(f.$('current-name').textContent, '当前群');
    assert.equal(f.requests.some(r => r.path === '/recommendations/confirm' || r.path === '/current-group'), false);
    assert.equal([...f.store.values()].some(value => value.includes(transcript)), false);
    f.$('voice-proposal-cancel').dispatch('click'); await flush();
    assert.equal(f.$('voice-proposal-panel').hidden, true);
    assert.equal(f.$('voice-proposal-transcript').textContent, '');
    assert.equal(f.tracks.every(track => !track.enabled && track.stopped), true);
  } finally { f.restore(); }
});

test('product terminal target text clears on hidden, audio-off, ownership, identity, directory change and new START', async () => {
  for (const action of ['hidden', 'audio-off', 'owner', 'identity', 'catalog', 'start']) {
    const f = await product();
    try {
      f.$('voice-start').dispatch('click'); await flush();
      f.settings.proposalError = 'voice_intent_negated'; f.final('不要切到<维修组>'); await flush();
      assert.equal(f.$('voice-proposal-transcript').textContent, '不要切到<维修组>');
      if (action === 'hidden') { f.document.hidden = true; f.documentEvents.visibilitychange(); }
      if (action === 'audio-off') f.$('audio-disable').dispatch('click');
      if (action === 'owner') { f.store.set('shenglu.control.owner.v1.need_device', 'other_page'); f.windowEvents.storage({key: 'shenglu.control.owner.v1.need_device', newValue: 'other_page'}); }
      if (action === 'identity') { f.store.set('shenglu.control.identity.v1', JSON.stringify({...f.identity, token: 'new_token'})); f.windowEvents.storage({key: 'shenglu.control.identity.v1', newValue: 'updated'}); }
      if (action === 'catalog') { f.state.groups[1].purpose = '新职责'; f.$('refresh-state').dispatch('click'); }
      if (action === 'start') f.$('voice-start').dispatch('click');
      await flush();
      assert.equal(f.$('voice-proposal-transcript').textContent, '', action);
      assert.equal(f.$('voice-proposal-transcript-area').hidden, true, action);
      if (action !== 'start') assert.equal(f.$('voice-proposal-panel').hidden, true, action);
      assert.equal(f.requests.some(r => r.path === '/recommendations/confirm' || r.path === '/current-group'), false, action);
    } finally { f.restore(); }
  }
});

test('product late negated response after close or a new START cannot revive the old text', async () => {
  for (const action of ['close', 'start']) {
    const f = await product();
    try {
      f.$('voice-start').dispatch('click'); await flush();
      f.settings.proposal = deferred(); f.settings.proposalError = 'voice_intent_negated';
      f.final('不要切到<维修组>'); await flush();
      f.$('voice-proposal-cancel').dispatch('click'); await flush();
      if (action === 'start') { f.$('voice-start').dispatch('click'); await flush(); }
      f.settings.proposal.resolve(); await flush();
      assert.equal(f.$('voice-proposal-transcript').textContent, '', action);
      assert.equal(f.$('voice-proposal-results').children.length, 0, action);
      assert.doesNotMatch(f.$('voice-proposal-status').textContent, /已识别为不切群/, action);
      assert.equal(f.requests.some(r => r.path === '/recommendations/confirm' || r.path === '/current-group'), false, action);
    } finally { f.restore(); }
  }
});

test('target transcript region is keyboard accessible and scrolls instead of truncating the final words', async () => {
  const html = await readFile(new URL('./index.html', import.meta.url), 'utf8');
  const css = await readFile(new URL('./style.css', import.meta.url), 'utf8');
  assert.match(html, /id="voice-proposal-transcript"[^>]*tabindex="0"[^>]*role="region"/);
  const rule = css.match(/\.voice-transcript\s*\{([^}]+)\}/)?.[1];
  assert.ok(rule); assert.match(rule, /max-height:/); assert.match(rule, /overflow:\s*auto/);
  assert.match(rule, /white-space:\s*pre-wrap/); assert.match(rule, /overflow-wrap:\s*anywhere/);
  assert.doesNotMatch(rule, /line-clamp|text-overflow/);
});
