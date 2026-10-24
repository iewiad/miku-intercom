import test from 'node:test';
import assert from 'node:assert/strict';
import {pcmRMS, spectrumPeak, assessSamples, summarizeRTP, safeReport, captureStatus, remoteObservation, sourceObservation, rtpDelta, diagnosticSourceOptions, DiagnosticPTTDriver, SingleInitialization, Terminal, loadSyntheticSpeech, syntheticSpeechSource, speechSendDuration, bindDiagnosticPTTPointer, diagnosticSendMessage} from './diagnostics.js';
import {AudioClient} from './audio.js';
import {ControlClient} from './app.js';

test('PCM RMS measures receiver waveform and silence', () => {
  const pcm = Float32Array.from({length: 4800}, (_, i) => 0.1 * Math.sin(2 * Math.PI * 440 * i / 48000));
  assert.ok(Math.abs(pcmRMS(pcm) - Math.SQRT1_2 * 0.1) < 0.00001);
  assert.equal(pcmRMS(new Float32Array(8192)), 0);
});
test('FFT peak uses the AudioContext actual sample rate', () => {
  const bins = new Float32Array(4096).fill(-Infinity); bins[75] = -25;
  assert.deepEqual(spectrumPeak(bins, 48000, 8192), {frequency: 439.453125, db: -25});
});
test('silence is not a pass when a remote receiver never produced valid samples', () => {
  assert.equal(assessSamples([], null).pass, false);
  assert.equal(assessSamples([{valid: false, rms: 0, frequency: 0}], null).pass, false);
  assert.equal(assessSamples(Array.from({length: 10}, () => ({valid: true, rms: 0, frequency: 0})), null).pass, true);
});
test('tone requires both decoded amplitude and the expected frequency', () => {
  const tone = Array.from({length: 10}, () => ({valid: true, rms: 0.07, frequency: 440}));
  assert.equal(assessSamples(tone, 440).pass, true);
  assert.equal(assessSamples(tone, 997).pass, false);
  assert.equal(assessSamples(tone, null).pass, false);
});
test('RTP diagnostics select only inbound audio and resolve codec IDs', () => {
  const stats = new Map([['codec', {mimeType: 'audio/opus', clockRate: 48000}],
    ['in', {type: 'inbound-rtp', kind: 'audio', codecId: 'codec', packetsReceived: 50, bytesReceived: 1234}],
    ['out', {type: 'outbound-rtp', kind: 'audio', packetsSent: 999}]]);
  assert.deepEqual(summarizeRTP(stats), [{packetsReceived: 50, bytesReceived: 1234, totalAudioEnergy: null,
    totalSamplesDuration: null, codec: 'audio/opus', clockRate: 48000}]);
});
test('report export does not copy identity credentials, SDP or Peer objects', () => {
  const report = safeReport({mode: 'single-independent-tab', status: 'observations-only', results: [],
    token: 'never-export', sdp: 'never-export', identity: {token: 'never-export'}, peer: {}});
  assert.equal(JSON.stringify(report).includes('never-export'), false);
  assert.equal('identity' in report, false); assert.equal('peer' in report, false);
});


test('real capture status permits numeric/audio settings and excludes hardware identifiers', () => {
  const track = {readyState: 'live', enabled: false, muted: false, label: 'PRIVATE_LABEL', getSettings: () => ({
    deviceId: 'PRIVATE_ID', groupId: 'PRIVATE_GROUP', sampleRate: 48000, channelCount: 1, latency: .01,
    echoCancellation: true, noiseSuppression: false, autoGainControl: true, unknown: 'PRIVATE'})};
  const result = captureStatus({getAudioTracks: () => [track]});
  assert.deepEqual(result, {track_state: 'live', enabled: false, muted: false, settings: {sampleRate: 48000,
    channelCount: 1, latency: .01, echoCancellation: true, noiseSuppression: false, autoGainControl: true}});
  assert.equal(JSON.stringify(result).includes('PRIVATE'), false);
  assert.equal(captureStatus(null).track_state, 'none');
});
test('real microphone uses the default gUM only after explicit enable, with no oscillator path', async () => {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  const calls = [], track = {enabled: true, stopped: false, stop() { this.stopped = true; }};
  const stream = {getAudioTracks: () => [track], getTracks: () => [track]};
  Object.defineProperty(globalThis, 'navigator', {configurable: true, value: {mediaDevices: {getUserMedia: async options => { calls.push(options); return stream; }}}});
  let client;
  try {
    const context = new Proxy({}, {get() { throw new Error('synthetic WebAudio source must not be used'); }});
    const sourceOptions = diagnosticSourceOptions('system_microphone', context, 440);
    assert.equal(Object.hasOwn(sourceOptions, 'sourceFactory'), false);
    client = new AudioClient({...sourceOptions, request: async () => ({available: false}),
      getContext: () => ({device_id: 'synthetic_test_device', group_id: 'synthetic_test_group', current_version: 1, server_epoch: 'test_epoch'}),
      random: () => 'test_peer', output: {pause() {}}});
    await client.sync(); assert.equal(calls.length, 0);
    await client.enable(); assert.equal(calls.length, 1);
    assert.deepEqual(calls[0], {audio: {channelCount: {ideal: 1}, echoCancellation: true, noiseSuppression: true, autoGainControl: true}, video: false});
    assert.equal(track.enabled, false); assert.equal(track.stopped, true);
  } finally { client?.disable(); if (descriptor) Object.defineProperty(globalThis, 'navigator', descriptor); else delete globalThis.navigator; }
});
test('quiet real mic and remote observations remain measurements, without frequency or success judgement', () => {
  const samples = [{valid: true, rms: 0, enabled: true, track_state: 'live'}, {valid: true, rms: 0, enabled: false, track_state: 'live'}, {valid: false, rms: NaN}];
  assert.deepEqual(remoteObservation(samples), {total_samples: 3, valid_samples: 2, rms_peak: 0, rms_mean: 0, rms_latest: 0});
  assert.deepEqual(sourceObservation(samples), {...remoteObservation(samples), enabled_samples: 1, live_samples: 2});
  assert.deepEqual(remoteObservation([]), {total_samples: 0, valid_samples: 0, rms_peak: null, rms_mean: null, rms_latest: null});
  const report = safeReport({source_kind: 'system_microphone', mode: 'single-independent-window', status: 'passed', results: [],
    measurements: [{remote_pcm: remoteObservation(samples), inbound: [{packetsReceived: 30, totalSamplesDuration: .6}]}]});
  assert.equal(report.status, 'observations-only'); assert.equal(report.source_kind, 'system_microphone');
  assert.equal(report.human_listening, 'not_performed'); assert.match(report.scope, /real system microphone/);
  assert.equal('pass' in report.measurements[0].remote_pcm, false); assert.equal('frequency' in report.measurements[0].remote_pcm, false);
  assert.equal(safeReport({status: 'passed'}).source_kind, 'synthetic');
});
test('RTP window counters distinguish decoded duration from PCM energy', () => {
  assert.deepEqual(rtpDelta([{packetsReceived: 10, totalSamplesDuration: 2}], [{packetsReceived: 30, totalSamplesDuration: 2.4, totalAudioEnergy: 0}]), {packets: 20, decoded_seconds: .4});
  assert.deepEqual(rtpDelta([{packetsReceived: 100}], [{packetsReceived: 2}]), {packets: 0, decoded_seconds: 0});
});
test('diagnostic 3-second driver follows actual ControlClient grant and cancels track before release', async () => {
  const now = Date.parse('2026-10-24T04:00:00Z'), requests = [], timers = new Map(); let serial = 0, enabled = false;
  const control = new ControlClient({now: () => now, random: () => 'test_request', schedule: (fn, delay) => { const id = ++serial; timers.set(id, {fn, delay}); return id; }, cancel: id => timers.delete(id),
    fetchImpl: async (url, options) => {
      const body = JSON.parse(options.body); requests.push({url, body, enabled});
      return {ok: true, json: async () => ({floor: {device_id: 'test_device', request_id: body.request_id, peer_id: 'test_peer', lease_id: 'test_lease', expires_at: new Date(now + 3000).toISOString()}})};
    }});
  control.load({token: 'synthetic_test_token', device: {id: 'test_device'}});
  control.state = {device: {id: 'test_device'}, current_group_id: 'test_group', current_version: 1, server_epoch: 'test_epoch', server_time: new Date(now).toISOString()}; control.stateAt = now; control.connected = true;
  control.audio = {peerID: 'test_peer', isReady: () => true, setSending(value) { enabled = value; return value; }};
  let finish, delay;
  const driver = new DiagnosticPTTDriver(control, {schedule(fn, ms) { finish = fn; delay = ms; return 1; }, cancel() {}});
  await driver.start(); assert.equal(enabled, true); assert.equal(control.floorPhase, 'held'); assert.equal(delay, 3000);
  assert.equal(requests[0].body.peer_id, 'test_peer'); assert.equal(requests[0].enabled, false);
  finish(); await Promise.resolve(); assert.equal(enabled, false); assert.equal(control.pressed, false); assert.equal(driver.active, false);
  assert.ok(requests.some(request => request.url.endsWith('/floor/release') && request.enabled === false));
  assert.equal(timers.size, 0);
});
test('cancel during pending diagnostic acquire cannot schedule a later hold', async () => {
  let resolve, scheduled = 0, cancellations = 0;
  const client = {pressed: false, floorRequest: null, floorPhase: 'idle',
    pressFloor() { this.pressed = true; this.floorRequest = {}; return new Promise(done => { resolve = done; }); },
    cancelFloor() { this.pressed = false; this.floorRequest = null; cancellations++; }};
  const driver = new DiagnosticPTTDriver(client, {schedule() { scheduled++; }, cancel() {}});
  const pending = driver.start(); driver.stop(); resolve(); await pending;
  assert.equal(scheduled, 0); assert.equal(cancellations, 1); assert.equal(driver.active, false); assert.equal(client.pressed, false);
});
test('diagnostic timer defaults retain the global receiver required by browser native methods', async () => {
  const originalSet = globalThis.setTimeout, originalClear = globalThis.clearTimeout; let callback;
  const client = {pressed: false, floorRequest: null, floorPhase: 'idle', async pressFloor() { this.pressed = true; this.floorPhase = 'held'; }, cancelFloor() { this.pressed = false; }};
  try {
    globalThis.setTimeout = function(fn, delay) { assert.equal(this, globalThis); assert.equal(delay, 3000); callback = fn; return 7; };
    globalThis.clearTimeout = function(id) { assert.equal(this, globalThis); assert.equal(id, 7); };
    const driver = new DiagnosticPTTDriver(client); await driver.start(); callback(); assert.equal(client.pressed, false);
  } finally { globalThis.setTimeout = originalSet; globalThis.clearTimeout = originalClear; }
});

test('single initialization ignores double clicks and Stop invalidates a pending identity before source enable', async () => {
  let resolveIdentity, created = 0, ready = 0, closed = 0;
  const terminal = {closed: false, identify: () => new Promise(resolve => { resolveIdentity = resolve; }), async close() { this.closed = true; closed++; }};
  const initialization = new SingleInitialization();
  const first = initialization.run(async () => { created++; return terminal; }, async () => { ready++; });
  await Promise.resolve();
  assert.equal(initialization.busy, true);
  assert.equal(await initialization.run(async () => { created++; return terminal; }, async () => { ready++; }), false);
  initialization.cancel(); resolveIdentity(); assert.equal(await first, false);
  assert.equal(created, 1); assert.equal(ready, 0); assert.equal(closed, 1); assert.equal(initialization.busy, false);
  const fresh = {closed: false, async identify() {}, async close() { this.closed = true; }};
  assert.equal(await initialization.run(async () => fresh, async () => { ready++; }), true);
  assert.equal(ready, 1); assert.equal(initialization.busy, false);
});

test('ended PTT report retains frozen PCM after rolling buffers clear, while active windows remain dynamic', () => {
  // Use the actual Terminal window-close and report methods without constructing browser resources.
  const terminal = Object.create(Terminal.prototype), origin = performance.now();
  Object.assign(terminal, {label: 'report-test', sourceKind: 'system_microphone', startedAt: origin,
    control: {pressed: true, floorPhase: 'held', state: {current_group_id: 'test_group', current_version: 1}},
    audio: {localStream: null, peer: null}, sourceSamples: [], samples: [], rtpLatest: [], windows: [],
    activeWindow: null, lastCapture: null, history: []});
  terminal.observePTT();
  const at = performance.now();
  terminal.sourceSamples.push({at, valid: true, rms: .05, enabled: true, track_state: 'live'});
  terminal.samples.push({at, valid: true, rms: .04});
  assert.equal(terminal.observation().ptt_windows[0].source_pcm.valid_samples, 1);
  terminal.sourceSamples.push({at: performance.now(), valid: true, rms: .03, enabled: true, track_state: 'live'});
  assert.equal(terminal.observation().ptt_windows[0].source_pcm.valid_samples, 2);
  terminal.control.pressed = false; terminal.control.floorPhase = 'idle'; terminal.observePTT();
  const before = terminal.observation();
  assert.equal(before.ptt_windows[0].source_pcm.valid_samples, 2);
  assert.equal(before.ptt_windows[0].remote_pcm.rms_peak, .04);
  assert.equal(before.source_pcm.rolling_window.sample_limit, 3000);
  assert.ok(before.source_pcm.rolling_window.from_ms <= before.source_pcm.rolling_window.to_ms);
  terminal.sourceSamples.length = 0; terminal.samples.length = 0;
  const after = terminal.observation();
  assert.deepEqual(after.ptt_windows, before.ptt_windows);
  assert.equal(after.source_pcm.valid_samples, 0);
  assert.deepEqual(after.source_pcm.rolling_window, {from_ms: null, to_ms: null, sample_limit: 3000});
  assert.deepEqual(after.remote_pcm.rolling_window, {from_ms: null, to_ms: null, sample_limit: 3000});
});

const wavFile = (options = {}) => {
  const data = new ArrayBuffer(16), bytes = new Uint8Array(data);
  bytes.set([...Buffer.from('RIFF')], 0); bytes.set([...Buffer.from('WAVE')], 8);
  return {size: data.byteLength, arrayBuffer: async () => data, ...options};
};
test('speech WAV validation limits local input before decode and returns clear format/decode errors', async () => {
  let decodes = 0, reads = 0;
  const buffer = {duration: 2.1, numberOfChannels: 1, sampleRate: 48000};
  const context = {async decodeAudioData(data) { decodes++; assert.equal(data.byteLength, 16); return buffer; }};
  assert.equal(await loadSyntheticSpeech(wavFile(), context), buffer);
  await assert.rejects(loadSyntheticSpeech(null, context), /请选择/);
  await assert.rejects(loadSyntheticSpeech(wavFile({size: 2 * 1024 * 1024 + 1, arrayBuffer: async () => { reads++; }}), context), /2 MiB/);
  assert.equal(reads, 0);
  await assert.rejects(loadSyntheticSpeech(wavFile({arrayBuffer: async () => new ArrayBuffer(6)}), context), /RIFF\/WAVE/);
  await assert.rejects(loadSyntheticSpeech(wavFile({arrayBuffer: async () => new ArrayBuffer(2 * 1024 * 1024 + 1)}), context), /2 MiB/);
  await assert.rejects(loadSyntheticSpeech(wavFile({arrayBuffer: async () => { throw new Error('PRIVATE_FILE_NAME'); }}), context), /无法读取本机/);
  assert.equal(decodes, 1);
  await assert.rejects(loadSyntheticSpeech(wavFile(), {decodeAudioData: async () => { throw new Error('PRIVATE_DECODER_DETAIL'); }}), error => /解码失败/.test(error.message) && !/PRIVATE/.test(error.message));
  for (const duration of [0, -1, 12.01, Infinity, NaN]) await assert.rejects(loadSyntheticSpeech(wavFile(), {decodeAudioData: async () => ({...buffer, duration})}), /12 秒/);
  for (const numberOfChannels of [0, 3, 1.5]) await assert.rejects(loadSyntheticSpeech(wavFile(), {decodeAudioData: async () => ({...buffer, numberOfChannels})}), /单声道或双声道/);
  assert.equal(speechSendDuration({duration: 2.1}), 3100); assert.equal(speechSendDuration({duration: 12}), 13000);
});

function speechContext(events = []) {
  const nodes = [], tracks = [{enabled: true, stop() { events.push('track-stop'); }}];
  const destination = {stream: {getTracks: () => tracks, getAudioTracks: () => tracks}, disconnect() { events.push('destination-disconnect'); }};
  return {nodes, destination, createMediaStreamDestination: () => destination,
    createBufferSource() {
      const node = {connect(target) { assert.equal(target, destination); }, start() { events.push('speech-start'); }, stop() { events.push('speech-stop'); }, disconnect() { events.push('node-disconnect'); }};
      nodes.push(node); return node;
    }};
}
test('speech source is silent until grant playback, restarts from a fresh node and never monitors locally', () => {
  const events = [], context = speechContext(events), buffer = {duration: 2};
  const source = syntheticSpeechSource(context, buffer);
  assert.equal(context.nodes.length, 0); assert.deepEqual(events, []);
  source.play(); assert.equal(context.nodes.length, 1); assert.equal(context.nodes[0].buffer, buffer); assert.equal(context.nodes[0].loop, false);
  source.play(); assert.equal(context.nodes.length, 2); assert.equal(events.filter(x => x === 'speech-stop').length, 1);
  assert.equal(context.nodes[0].onended, null);
  context.nodes[1].onended(); source.stop(); assert.equal(events.filter(x => x === 'speech-stop').length, 1);
  source.play(); source.close(); source.close();
  assert.equal(events.filter(x => x === 'speech-stop').length, 2); assert.equal(events.filter(x => x === 'track-stop').length, 1);
  assert.equal(source.stream.getTracks()[0].enabled, false); assert.throws(() => source.play(), /已关闭/);
});
test('speech source factory bypasses system capture and report keeps its synthetic scope', async () => {
  let opens = 0;
  const options = diagnosticSourceOptions('synthetic_speech', null, null, async () => { opens++; return 'synthetic_source'; });
  assert.equal(await options.sourceFactory(), 'synthetic_source'); assert.equal(opens, 1);
  assert.throws(() => diagnosticSourceOptions('synthetic_speech', null, null), /尚未准备/);
  const report = safeReport({source_kind: 'synthetic_speech', status: 'passed', results: []});
  assert.equal(report.source_kind, 'synthetic_speech'); assert.equal(report.human_listening, 'not_performed'); assert.equal(report.status, 'observations-only');
  assert.match(report.scope, /no microphone acquisition/); assert.match(report.scope, /no audio retained/);
});
test('speech waits for actual ControlClient grant and stops the node before normal floor release', async () => {
  const now = Date.parse('2026-10-24T04:00:00Z'), events = [], context = speechContext(events), source = syntheticSpeechSource(context, {duration: 2});
  let grant, finish;
  const control = new ControlClient({now: () => now, random: () => 'speech_request', schedule: () => 1, cancel() {},
    fetchImpl: async (url, options) => {
      const body = JSON.parse(options.body);
      if (url.endsWith('/floor/acquire')) await new Promise(resolve => { grant = resolve; });
      else if (url.endsWith('/floor/release')) events.push('floor-release');
      return {ok: true, json: async () => ({floor: {device_id: 'speech_device', request_id: body.request_id, peer_id: 'speech_peer', lease_id: 'speech_lease', expires_at: new Date(now + 3000).toISOString()}})};
    }});
  control.load({token: 'synthetic_test_token', device: {id: 'speech_device'}});
  control.state = {device: {id: 'speech_device'}, current_group_id: 'speech_group', current_version: 1, server_epoch: 'test_epoch', server_time: new Date(now).toISOString()}; control.stateAt = now; control.connected = true;
  control.audio = {peerID: 'speech_peer', isReady: () => true, setSending(value) { events.push(value ? 'tx-open' : 'tx-close'); return value; }};
  const driver = new DiagnosticPTTDriver(control, {duration: 2250, schedule(fn, ms) { assert.equal(ms, 2250); finish = fn; return 2; }, cancel() {}, onGranted: () => source.play(), onStop: () => source.stop()});
  const pending = driver.start(); assert.equal(control.floorPhase, 'acquiring'); assert.equal(context.nodes.length, 0);
  grant(); await pending; assert.equal(context.nodes.length, 1); assert.ok(events.indexOf('speech-start') > events.indexOf('tx-open'));
  finish(); await Promise.resolve(); assert.equal(control.pressed, false);
  assert.ok(events.indexOf('speech-stop') < events.indexOf('floor-release'));
  assert.equal(driver.active, false);
});
test('cancel before delayed speech grant never starts a clip, and later loss of held state stops it', async () => {
  const events = [], context = speechContext(events), source = syntheticSpeechSource(context, {duration: 2}); let grant;
  const client = {pressed: false, floorPhase: 'idle', pressFloor() { this.pressed = true; this.floorPhase = 'acquiring'; return new Promise(resolve => { grant = resolve; }); }, cancelFloor() { this.pressed = false; this.floorPhase = 'idle'; }};
  const driver = new DiagnosticPTTDriver(client, {onGranted: () => source.play(), onStop: () => source.stop(), schedule() { assert.fail('cancelled grant must not schedule playback'); }});
  const pending = driver.start(); driver.stop(); grant(); await pending; assert.equal(context.nodes.length, 0);
  source.play(); const terminal = Object.create(Terminal.prototype);
  Object.assign(terminal, {sourceKind: 'synthetic_speech', speechSource: source, control: client, activeWindow: null});
  terminal.observePTT(); assert.equal(events.filter(x => x === 'speech-stop').length, 1);
});
test('pending speech resume is cancelled by actual diagnostic blur/end path, Terminal close and switch', async () => {
  for (const reason of ['blur', 'close', 'switch']) {
    let resume, acquisitions = 0, starts = 0, scheduled = 0;
    const client = {pressed: false, floorRequest: null, floorPhase: 'idle', state: {current_group_id: 'old'},
      async pressFloor() { acquisitions++; this.pressed = true; this.floorPhase = 'held'; }, cancelFloor() { this.pressed = false; this.floorRequest = null; },
      async switchGroup(group) { this.state.current_group_id = group; }};
    const driver = new DiagnosticPTTDriver(client, {onGranted: () => { starts++; }, schedule() { scheduled++; return 1; }});
    const terminal = Object.create(Terminal.prototype);
    Object.assign(terminal, {driver, control: client, audio: {suspend() {}, disable() {}}, sourceKind: 'synthetic_speech',
      attachRemote() {}, attachLocal() {}, async connect() {}, output: {remove() {}}, recordLifecycle() {}});
    const pending = driver.start({prepare: () => new Promise(resolve => { resume = resolve; }), isCurrent: () => !terminal.closed});
    assert.equal(driver.active, true); assert.equal(acquisitions, 0); assert.equal(client.pressed, false);
    if (reason === 'blur') {
      const endPress = bindDiagnosticPTTPointer({addEventListener() {}}, client, () => driver); endPress();
    } else if (reason === 'close') await terminal.close();
    else await terminal.switchTo('new');
    assert.equal(driver.active, false, reason); resume(); await pending;
    assert.equal(acquisitions, 0, reason); assert.equal(starts, 0, reason); assert.equal(scheduled, 0, reason);
  }
});
test('foreground/current-state guard rejects a late resume or a grant delivered after focus was lost', async () => {
  for (const waitAt of ['resume', 'grant']) {
    let finish, focused = true, acquisitions = 0, starts = 0, cancellations = 0;
    const client = {pressed: false, floorPhase: 'idle', async pressFloor() { acquisitions++; this.pressed = true; this.floorPhase = 'held'; if (waitAt === 'grant') await new Promise(resolve => { finish = resolve; }); },
      cancelFloor() { cancellations++; this.pressed = false; this.floorPhase = 'idle'; }};
    const driver = new DiagnosticPTTDriver(client, {onGranted: () => { starts++; }, schedule() { assert.fail('unfocused attempt must not schedule'); }});
    const pending = driver.start({prepare: waitAt === 'resume' ? () => new Promise(resolve => { finish = resolve; }) : null, isCurrent: () => focused});
    focused = false; finish(); await pending;
    assert.equal(starts, 0); assert.equal(driver.active, false); assert.equal(acquisitions, waitAt === 'grant' ? 1 : 0);
    assert.equal(cancellations, waitAt === 'grant' ? 1 : 0);
  }
});
test('speech RMS envelopes contain only the latest 200 aggregate frames without waveform or source file', () => {
  const terminal = Object.create(Terminal.prototype);
  Object.assign(terminal, {label: 'speech', sourceKind: 'synthetic_speech', speechBuffer: {duration: 2, numberOfChannels: 1, sampleRate: 48000}, startedAt: 100,
    control: {state: {current_version: 1}}, audio: {localStream: null}, sourceSamples: [], samples: Array.from({length: 250}, (_, i) => ({at: 100 + i * 100, valid: true, rms: i / 1000, waveform: 'PRIVATE_WAVEFORM', token: 'PRIVATE_TOKEN'})),
    rtpLatest: [], windows: [], activeWindow: null, lastCapture: null, history: []});
  const observation = terminal.observation();
  assert.equal(observation.remote_rms_frames.length, 200);
  assert.deepEqual(observation.remote_rms_frames[0], {elapsed_ms: 5000, rms: .05, valid: true});
  assert.deepEqual(observation.remote_rms_frames.at(-1), {elapsed_ms: 24900, rms: .249, valid: true});
  assert.equal(JSON.stringify(observation).includes('PRIVATE'), false);
  terminal.sourceKind = 'system_microphone'; assert.equal('remote_rms_frames' in terminal.observation(), false);
});

function speechDriverHarness({duration = 5.39988, prepare, grant} = {}) {
  const events = [], context = speechContext(events), source = syntheticSpeechSource(context, {duration});
  const timers = new Map(); let serial = 0, clock = 0;
  const client = {pressed: false, floorPhase: 'idle', floorRequest: null,
    async pressFloor() { this.pressed = true; this.floorPhase = 'acquiring'; if (grant) await grant(); this.floorPhase = 'held'; },
    cancelFloor() { this.pressed = false; this.floorPhase = 'idle'; events.push('floor-release'); }};
  const driver = new DiagnosticPTTDriver(client, {duration: speechSendDuration({duration}), sourceDuration: Math.ceil(duration * 1000),
    drainDuration: 250, timeoutReason: 'source-timeout', now: () => clock,
    schedule(callback, delay) { const id = ++serial; timers.set(id, {callback, at: clock + delay, delay}); return id; }, cancel: id => timers.delete(id),
    onGranted: onEnded => source.play(onEnded), onStop: () => source.stop()});
  return {driver, client, source, context, timers, events, start: () => driver.start({prepare}), setClock: value => { clock = value; },
    fire(id) { const timer = timers.get(id); assert.ok(timer); clock = timer.at; timer.callback(); }};
}
test('speech progress follows preparation and grant, then only natural end plus drain confirms complete send', async () => {
  let prepared, granted;
  const h = speechDriverHarness({prepare: () => new Promise(resolve => { prepared = resolve; }), grant: () => new Promise(resolve => { granted = resolve; })});
  const pending = h.start();
  assert.equal(h.driver.snapshot().phase, 'preparing'); assert.match(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /准备音源/);
  prepared(); await Promise.resolve();
  assert.equal(h.driver.snapshot().phase, 'acquiring'); assert.equal(h.context.nodes.length, 0);
  assert.match(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /获权前不会播放/);
  granted(); await pending;
  assert.equal(h.driver.snapshot().phase, 'sending'); assert.equal(h.context.nodes.length, 1);
  h.setClock(4026); assert.equal(h.driver.snapshot().remaining_ms, 1374);
  assert.match(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /约剩 1\.4 秒.*留在本窗口/);
  h.setClock(5400); h.context.nodes[0].onended();
  assert.equal(h.driver.snapshot().phase, 'draining'); assert.equal(h.client.pressed, true);
  assert.match(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /排空尾音/);
  const drain = [...h.timers].find(([, timer]) => timer.delay === 250);
  h.setClock(5649); assert.equal(h.driver.snapshot().remaining_ms, 1); assert.equal(h.client.pressed, true);
  h.fire(drain[0]);
  assert.equal(h.driver.snapshot().stop_reason, 'source-ended'); assert.equal(h.client.pressed, false); assert.equal(h.timers.size, 0);
  assert.match(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /已完整发送.*现在可以切换窗口/);
  assert.equal(h.driver.snapshot().attempts[0].sending_ms, 5650);
  assert.equal(h.events.filter(event => event === 'floor-release').length, 1);
});
test('blur during the observed partial clip stops immediately and a stale natural end cannot claim completion', async () => {
  const h = speechDriverHarness(); await h.start(); const staleEnded = h.context.nodes[0].onended;
  h.setClock(4026);
  const endPress = bindDiagnosticPTTPointer({addEventListener() {}}, h.client, () => h.driver);
  endPress('window-blur');
  assert.equal(h.driver.snapshot().stop_reason, 'window-blur'); assert.equal(h.driver.snapshot().attempts[0].sending_ms, 4026);
  assert.equal(h.client.pressed, false); assert.equal(h.timers.size, 0); assert.equal(h.context.nodes[0].onended, null);
  staleEnded(); assert.equal(h.driver.snapshot().stop_reason, 'window-blur');
  assert.match(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /窗口失焦.*未完整发送/);
  assert.ok(h.events.indexOf('speech-stop') < h.events.indexOf('floor-release'));
});
test('speech watchdog never reports completion without natural end and a full drain, including a late ended event', async () => {
  for (const endedLate of [false, true]) {
    const h = speechDriverHarness({duration: 12}); await h.start();
    const watchdog = [...h.timers].find(([, timer]) => timer.delay === 13000);
    if (endedLate) { h.setClock(12900); h.context.nodes[0].onended(); assert.equal(h.client.pressed, true); }
    h.fire(watchdog[0]);
    assert.equal(h.driver.snapshot().stop_reason, 'source-timeout'); assert.equal(h.timers.size, 0); assert.equal(h.client.pressed, false);
    assert.match(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /超时.*未确认完整发送/);
  }
});
test('source stop reasons stay distinct, lost floor stops playback, and diagnostic summaries remain bounded', async () => {
  const h = speechDriverHarness();
  for (const reason of ['user-cancelled', 'page-hidden', 'offline', 'media-interrupted', 'group-changed']) {
    await h.start(); h.driver.stop(reason);
    assert.equal(h.driver.snapshot().stop_reason, reason); assert.equal(h.timers.size, 0);
    assert.doesNotMatch(diagnosticSendMessage(h.driver.snapshot(), 'synthetic_speech'), /已完整发送/);
  }
  await h.start(); h.client.pressed = false; h.client.floorPhase = 'idle'; h.driver.observeFloor();
  assert.equal(h.driver.snapshot().stop_reason, 'floor-lost'); assert.equal(h.timers.size, 0);
  for (let i = 0; i < 35; i++) { await h.start(); h.driver.stop('user-cancelled'); }
  const snapshot = h.driver.snapshot(); assert.equal(snapshot.attempts.length, 30);
  assert.deepEqual(Object.keys(snapshot.attempts[0]).sort(), ['ended_at', 'expected_duration_ms', 'requested_at', 'sending_ms', 'stop_reason']);
  const copy = h.driver.snapshot(); copy.attempts[0].stop_reason = 'MUTATED'; assert.equal(h.driver.snapshot().attempts[0].stop_reason, 'user-cancelled');
});
test('non-speech timed capture reports its duration limit rather than claiming a complete speech clip', async () => {
  let finish;
  const client = {pressed: false, floorPhase: 'idle', async pressFloor() { this.pressed = true; this.floorPhase = 'held'; }, cancelFloor() { this.pressed = false; }};
  const driver = new DiagnosticPTTDriver(client, {schedule(callback, delay) { assert.equal(delay, 3000); finish = callback; return 1; }, cancel() {}});
  await driver.start(); finish();
  assert.equal(driver.snapshot().stop_reason, 'duration-limit'); assert.match(diagnosticSendMessage(driver.snapshot(), 'system_microphone'), /限时发送上限/);
  assert.doesNotMatch(diagnosticSendMessage(driver.snapshot(), 'synthetic'), /完整发送/);
});
