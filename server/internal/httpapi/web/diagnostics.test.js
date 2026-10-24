import test from 'node:test';
import assert from 'node:assert/strict';
import {pcmRMS, spectrumPeak, assessSamples, summarizeRTP, safeReport, captureStatus, remoteObservation, sourceObservation, rtpDelta, diagnosticSourceOptions, DiagnosticPTTDriver, SingleInitialization, Terminal} from './diagnostics.js';
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
