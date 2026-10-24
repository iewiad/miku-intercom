import {ControlClient, bindPTTPointer} from './app.js';
import {AudioClient} from './audio.js';
import {LifecycleLog} from './lifecycle.js';

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const uid = () => crypto.randomUUID();
const SINGLE_KEY = 'shenglu.diagnostics.single.v1';
const FREQUENCIES = [440, 997, 1777];
const SAMPLE_LIMIT = 3000;
const sampleWindow = (samples, origin) => ({from_ms: samples.length ? Math.round(samples[0].at - origin) : null,
  to_ms: samples.length ? Math.round(samples.at(-1).at - origin) : null, sample_limit: SAMPLE_LIMIT});
const assert = (condition, message) => { if (!condition) throw new Error(message); };
const round = value => Number.isFinite(value) ? Math.round(value * 100000) / 100000 : null;

export function pcmRMS(samples) {
  return Math.sqrt(samples.reduce((sum, value) => sum + value * value, 0) / Math.max(1, samples.length));
}
export function spectrumPeak(bins, sampleRate, fftSize, low = 200, high = 2200) {
  let index = 0, db = -Infinity;
  for (let i = Math.ceil(low * fftSize / sampleRate); i < Math.min(bins.length, high * fftSize / sampleRate); i++) {
    if (bins[i] > db) { db = bins[i]; index = i; }
  }
  return {frequency: index ? index * sampleRate / fftSize : 0, db};
}
export function assessSamples(samples, expectedFrequency) {
  const valid = samples.filter(sample => sample.valid);
  if (valid.length < 3) return {pass: false, reason: '不足三个有效远端 PCM 样本', samples: valid.length};
  const rmsMax = Math.max(...valid.map(sample => sample.rms));
  const rmsMean = valid.reduce((sum, sample) => sum + sample.rms, 0) / valid.length;
  const voiced = valid.filter(sample => sample.rms > 0.015 && Math.abs(sample.frequency - expectedFrequency) < 20);
  return {pass: expectedFrequency === null ? rmsMax < 0.005 : voiced.length / valid.length >= 0.65,
    samples: valid.length, rmsMax: round(rmsMax), rmsMean: round(rmsMean),
    expectedFrequency, matchingSamples: expectedFrequency === null ? undefined : voiced.length,
    peakFrequencies: [...new Set(valid.filter(s => s.rms > 0.005).map(s => Math.round(s.frequency)))].slice(0, 10)};
}
export function summarizeRTP(reports) {
  const list = [...reports.values()];
  return list.filter(stat => stat.type === 'inbound-rtp' && stat.kind === 'audio').map(stat => ({
    packetsReceived: stat.packetsReceived || 0, bytesReceived: stat.bytesReceived || 0,
    totalAudioEnergy: round(stat.totalAudioEnergy), totalSamplesDuration: round(stat.totalSamplesDuration),
    codec: reports.get(stat.codecId)?.mimeType || '', clockRate: reports.get(stat.codecId)?.clockRate || 0,
  }));
}
export function captureStatus(stream) {
  const track = stream?.getAudioTracks?.()[0];
  if (!track) return {track_state: 'none', enabled: false, muted: false, settings: {}};
  const input = track.getSettings?.() || {}, settings = {};
  for (const key of ['sampleRate', 'channelCount', 'latency']) if (Number.isFinite(input[key])) settings[key] = input[key];
  for (const key of ['echoCancellation', 'noiseSuppression', 'autoGainControl']) if (typeof input[key] === 'boolean') settings[key] = input[key];
  return {track_state: track.readyState || 'unknown', enabled: !!track.enabled, muted: !!track.muted, settings};
}
export function remoteObservation(samples) {
  const valid = samples.filter(sample => sample.valid && Number.isFinite(sample.rms));
  return {total_samples: samples.length, valid_samples: valid.length,
    rms_peak: valid.length ? round(Math.max(...valid.map(sample => sample.rms))) : null,
    rms_mean: valid.length ? round(valid.reduce((sum, sample) => sum + sample.rms, 0) / valid.length) : null,
    rms_latest: valid.length ? round(valid.at(-1).rms) : null};
}
export function sourceObservation(samples) {
  return {...remoteObservation(samples), enabled_samples: samples.filter(sample => sample.enabled).length,
    live_samples: samples.filter(sample => sample.track_state === 'live').length};
}
export function rtpDelta(before, after) {
  const total = (list, key) => list.reduce((sum, value) => sum + (Number.isFinite(value[key]) ? value[key] : 0), 0);
  return {packets: Math.max(0, total(after, 'packetsReceived') - total(before, 'packetsReceived')),
    decoded_seconds: round(Math.max(0, total(after, 'totalSamplesDuration') - total(before, 'totalSamplesDuration')))};
}
// The explicit developer driver uses the same ControlClient acquire/cancel path as normal PTT.
export class DiagnosticPTTDriver {
  constructor(client, {schedule = (callback, delay) => globalThis.setTimeout(callback, delay), cancel = id => globalThis.clearTimeout(id), duration = 3000,
    sourceDuration = duration, drainDuration = 0, timeoutReason = 'duration-limit', now = () => performance.now(), onChange = () => {},
    onGranted = () => {}, onStop = () => {}} = {}) {
    Object.assign(this, {client, schedule, cancel, duration, sourceDuration, drainDuration, timeoutReason, now, onChange, onGranted, onStop});
    this.generation = 0; this.active = false; this.timer = this.drainTimer = null; this.phase = 'idle'; this.attempts = [];
  }
  snapshot() {
    const elapsed = this.grantedAt === null || this.grantedAt === undefined ? 0 : Math.max(0, (this.active ? this.now() : this.endedAt) - this.grantedAt);
    const remaining = this.phase === 'draining' ? this.drainDuration - (this.now() - this.sourceEndedAt) : this.sourceDuration - elapsed;
    return {phase: this.phase, stop_reason: this.stopReason || null, elapsed_ms: Math.round(elapsed),
      remaining_ms: this.active ? Math.max(0, Math.ceil(remaining)) : 0, expected_duration_ms: this.sourceDuration,
      attempts: this.attempts.map(attempt => ({...attempt}))};
  }
  setPhase(phase) { this.phase = phase; this.onChange(); }
  async start({prepare = null, isCurrent = () => true} = {}) {
    this.stop('superseded'); const generation = ++this.generation; this.active = true;
    this.grantedAt = this.endedAt = this.sourceEndedAt = null; this.stopReason = null;
    this.requestedAt = new Date().toISOString(); this.setPhase('preparing');
    try {
      // Preparation belongs to this cancellable attempt too: a pending native
      // AudioContext.resume must not acquire a floor after blur or navigation.
      if (prepare) await prepare();
      if (generation !== this.generation) return;
      if (!isCurrent()) { this.stop('state-changed'); return; }
      this.setPhase('acquiring');
      await this.client.pressFloor();
      if (generation !== this.generation) return;
      if (!isCurrent()) { this.stop('state-changed'); return; }
      if (!this.client.pressed || this.client.floorPhase !== 'held') { this.stop('not-granted'); return; }
      this.grantedAt = this.now(); this.setPhase('sending');
      this.onGranted(() => {
        if (generation !== this.generation || !this.active || this.phase !== 'sending') return;
        this.sourceEndedAt = this.now(); this.setPhase('draining');
        this.drainTimer = this.schedule(() => { if (generation === this.generation) this.stop('source-ended'); }, this.drainDuration);
      });
      if (generation === this.generation && this.active) this.timer = this.schedule(() => {
        if (generation === this.generation) this.stop(this.timeoutReason);
      }, this.duration);
    } catch (error) { if (generation === this.generation) this.stop('error'); throw error; }
  }
  stop(reason = 'user-cancelled') {
    ++this.generation; const wasActive = this.active; this.active = false;
    for (const timer of [this.timer, this.drainTimer]) if (timer !== null) this.cancel(timer);
    this.timer = this.drainTimer = null;
    if (wasActive) {
      this.endedAt = this.now(); this.stopReason = reason; this.phase = 'stopped';
      this.attempts.push({requested_at: this.requestedAt, ended_at: new Date().toISOString(), stop_reason: reason,
        sending_ms: this.grantedAt === null ? 0 : Math.round(Math.max(0, this.endedAt - this.grantedAt)), expected_duration_ms: this.sourceDuration});
      if (this.attempts.length > 30) this.attempts.shift();
    }
    try { this.onStop(); } finally {
      if (this.client.pressed || this.client.floorRequest) this.client.cancelFloor();
      if (wasActive) this.onChange();
    }
  }
  observeFloor() {
    if (this.active && ['sending', 'draining'].includes(this.phase) && (!this.client.pressed || this.client.floorPhase !== 'held')) this.stop('floor-lost');
  }
}
export function diagnosticSendMessage(send, sourceKind) {
  if (!send || send.phase === 'idle') return sourceKind === 'synthetic_speech' ? '发送完整合成语音前，请留在本窗口；失焦会立即停发。' : '尚未使用限时发送；手动 PTT 松手即停发。';
  if (send.phase === 'preparing') return '正在准备音源，请留在本窗口。';
  if (send.phase === 'acquiring') return '正在申请麦权，获权前不会播放；请留在本窗口。';
  if (send.phase === 'sending') return `正在发送，约剩 ${(send.remaining_ms / 1000).toFixed(1)} 秒。请留在本窗口，看到结束提示后再切换。`;
  if (send.phase === 'draining') return '语音已播放完，正在排空尾音；请继续留在本窗口。';
  const reasons = {
    'source-ended': '合成语音已完整发送，已释放麦权。现在可以切换窗口并检查接收端。',
    'duration-limit': '已达到限时发送上限并停止，已释放麦权。',
    'source-timeout': '等待语音自然结束或尾音排空超时，已停止；本次未确认完整发送。',
    'window-blur': '窗口失焦，已立即停发；本次未完整发送。重发时请留在本窗口。',
    'page-hidden': '页面隐藏，已立即停发并关闭旧连接；本次未完整发送。',
    'user-cancelled': '已手动取消发送或申请；本次未完整发送。',
    'floor-lost': '麦权已结束，已停止发送；本次未确认完整发送。',
    'media-interrupted': '媒体连接变化，已停止发送；本次未完整发送，连接就绪后可重试。',
    'not-granted': '未获麦权，音源未开始播放；可在麦权空闲后重试。',
    'state-changed': '窗口、当前群或连接状态变化，已取消发送；本次未完整发送。',
    'group-changed': '正在切换群组，已停止发送；本次未完整发送。',
    'offline': '网络离线，已停止发送并关闭旧连接；本次未完整发送。',
    'disabled': '测试音频已关闭，发送已停止。',
    'superseded': '旧发送已取消，请等待本次发送结果。',
    'error': '准备音源或申请麦权失败，已停止；本次未完整发送。',
  };
  return reasons[send.stop_reason] || '发送已停止；本次未确认完整发送。';
}
export function bindDiagnosticPTTPointer(button, client, getDriver) {
  const endPointerPress = bindPTTPointer(button, client);
  // End pending preparation too, even when no floor request exists yet.
  return (reason = 'user-cancelled') => { getDriver()?.stop(typeof reason === 'string' ? reason : 'user-cancelled'); endPointerPress(); };
}
const SPEECH_MAX_BYTES = 2 * 1024 * 1024;
export async function loadSyntheticSpeech(file, context) {
  assert(file && Number.isFinite(file.size) && file.size > 0, '请选择本机合成语音 WAV 文件。');
  assert(file.size <= SPEECH_MAX_BYTES, '合成语音 WAV 文件不能超过 2 MiB。');
  let data;
  try { data = await file.arrayBuffer(); } catch { throw new Error('无法读取本机合成语音 WAV 文件，请重新选择。'); }
  assert(data.byteLength <= SPEECH_MAX_BYTES, '合成语音 WAV 文件不能超过 2 MiB。');
  const header = new Uint8Array(data, 0, Math.min(12, data.byteLength));
  const signature = offset => String.fromCharCode(...header.slice(offset, offset + 4));
  assert(header.length === 12 && signature(0) === 'RIFF' && signature(8) === 'WAVE', '请选择 RIFF/WAVE 格式的合成语音 WAV 文件。');
  let buffer;
  try { buffer = await context.decodeAudioData(data); } catch { throw new Error('合成语音 WAV 解码失败，请重新生成有效的 WAV 文件。'); }
  assert(Number.isFinite(buffer.duration) && buffer.duration > 0 && buffer.duration <= 12, '合成语音时长必须大于 0 且不超过 12 秒。');
  assert(Number.isInteger(buffer.numberOfChannels) && buffer.numberOfChannels >= 1 && buffer.numberOfChannels <= 2, '合成语音仅支持单声道或双声道。');
  return buffer;
}
// This source has no local monitor or microphone. A new one-shot node starts
// only after the normal floor grant; stopped nodes are never reused or looped.
export function syntheticSpeechSource(context, buffer) {
  const destination = context.createMediaStreamDestination();
  let current = null, closed = false;
  const stop = () => {
    const old = current; current = null;
    if (old) { old.onended = null; try { old.stop(); } catch { /* Already ended. */ } old.disconnect(); }
  };
  return {stream: destination.stream,
    play(onEnded = () => {}) {
      assert(!closed, '合成语音音源已关闭，请重新连接。'); stop();
      const node = context.createBufferSource(); node.buffer = buffer; node.loop = false;
      node.connect(destination); current = node;
      node.onended = () => { const finished = current === node && !closed; if (finished) current = null; node.disconnect(); if (finished) onEnded(); };
      try { node.start(); } catch (error) { stop(); throw error; }
    }, stop,
    close() { if (closed) return; closed = true; stop(); for (const track of destination.stream.getTracks()) { track.enabled = false; track.stop(); } destination.disconnect(); },
  };
}
// The watchdog includes the 250 ms natural-end drain plus event-loop slack. It
// never proves completion: only the source's onended followed by drain does.
export const speechSendDuration = buffer => Math.min(13000, Math.ceil(buffer.duration * 1000) + 1000);
export class SingleInitialization {
  constructor(onChange = () => {}) { this.onChange = onChange; this.busy = false; this.generation = 0; }
  cancel() { ++this.generation; }
  async run(create, ready) {
    if (this.busy) return false;
    this.busy = true; const generation = ++this.generation, current = () => generation === this.generation;
    let terminal; this.onChange();
    try {
      terminal = await create(current); if (!terminal) return false;
      await terminal.identify();
      if (!current() || terminal.closed) return false;
      await ready(terminal, current); return current() && !terminal.closed;
    } finally {
      if (terminal && !current()) await terminal.close();
      this.busy = false; this.onChange();
    }
  }
}
export function diagnosticSourceOptions(kind, context, frequency, speechFactory = null) {
  if (kind === 'system_microphone') return {}; // AudioClient's default is real getUserMedia.
  if (kind === 'synthetic_speech') { assert(typeof speechFactory === 'function', '尚未准备合成语音 WAV。'); return {sourceFactory: speechFactory}; }
  assert(kind === 'synthetic', '未知诊断音源');
  return {sourceFactory: async () => {
    const source = context.createOscillator(), gain = context.createGain(), destination = context.createMediaStreamDestination();
    source.type = 'sine'; source.frequency.value = frequency; gain.gain.value = 0.1;
    source.connect(gain).connect(destination); source.start();
    return {stream: destination.stream, close: () => { source.stop(); source.disconnect(); gain.disconnect(); }};
  }};
}
export function safeReport(report) {
  // Export an explicit diagnostic schema; never serialize a client/Peer/identity or arbitrary errors.
  const microphone = report.source_kind === 'system_microphone', speech = report.source_kind === 'synthetic_speech';
  return {schema: 'shenglu.browser-audio-diagnostic.v1', started_at: report.started_at,
    finished_at: report.finished_at, mode: report.mode, source_kind: microphone ? 'system_microphone' : speech ? 'synthetic_speech' : 'synthetic',
    scope: microphone ? 'real system microphone and browser WebRTC; remote PCM observations; human listening not performed' : speech ? 'local synthetic speech WAV through real browser WebRTC; no microphone acquisition; aggregate PCM only, no audio retained; human listening not performed' : 'synthetic audio; real browser WebRTC; no microphone/speaker listening',
    human_listening: 'not_performed', user_agent: report.user_agent,
    status: (microphone || speech) && report.status === 'passed' ? 'observations-only' : report.status, results: report.results,
    resources: report.resources || [], resources_after_close: report.resources_after_close || [], measurements: report.measurements || []};
}

export class Terminal {
  constructor(label, frequency, context, changed, identity = null, sourceKind = 'synthetic', speechBuffer = null) {
    Object.assign(this, {label, frequency, context, changed, sourceKind, speechBuffer});
    this.history = []; this.segmentSamples = []; this.lastCapture = null; this.sourceSamples = []; this.windows = []; this.activeWindow = null; this.sourceStream = null;
    this.samples = []; this.peers = new Set(); this.analyser = null; this.receiver = null;
    this.observed = new Map(); this.rtpLatest = []; this.startedAt = performance.now();
    this.binding = null; this.closed = false; this.pollTimer = null;
    this.lifecycle = new LifecycleLog({startedAt: this.startedAt}); this.lifecyclePhase = null; this.lifecyclePeer = null;
    this.output = document.createElement('audio'); this.output.muted = true; this.output.autoplay = true;
    this.output.hidden = true; document.body.append(this.output);
    this.control = new ControlClient({onChange: () => { this.observePTT(); changed(); }, onIdentity: () => changed()});
    if (identity) this.control.load(identity);
    this.audio = new AudioClient({
      request: (...args) => this.control.request(...args),
      scopeRequest: () => { const token = this.control.identity?.token; return (path, method, body, options) => this.control.request(path, method, body, {...options, token}); }, serverNow: () => this.control.serverNow(),
      getContext: () => this.control.state?.current_group_id && this.control.allowed() && !this.control.pendingSwitch && this.control.connected && (this.sourceKind === 'synthetic' || !document.hidden) ? {
        device_id: this.control.identity.device.id, group_id: this.control.state.current_group_id,
        current_version: this.control.state.current_version, server_epoch: this.control.state.server_epoch,
      } : null,
      ...diagnosticSourceOptions(sourceKind, context, frequency, () => {
        this.speechSource?.close(); this.speechSource = syntheticSpeechSource(context, speechBuffer); return this.speechSource;
      }),
      output: this.output,
      onRemoteStream: (stream, binding) => this.attachRemote(stream, binding),
      onChange: () => {
        if (this.audio?.peer) this.peers.add(this.audio.peer);
        if (this.audio?.localStream) this.lastCapture = captureStatus(this.audio.localStream);
        if (this.audio && (this.audio.phase !== this.lifecyclePhase || this.audio.peer !== this.lifecyclePeer)) {
          this.lifecyclePhase = this.audio.phase; this.lifecyclePeer = this.audio.peer;
          this.recordLifecycle('audio-state');
        }
        if (this.driver?.active && !this.audio.isReady()) this.driver.stop('media-interrupted');
        changed();
      },
      onFailure: () => this.control.cancelFloor('媒体连接失败'),
    });
    this.audio.setListeningMuted(true);
    this.control.audio = this.audio;
    this.sampler = setInterval(() => this.sample(), 100);
  }
  recordLifecycle(event, nativeEvent = null, previous = {}) {
    if (this.sourceKind === 'synthetic' || !this.lifecycle) return;
    this.lifecycle.record(event, {
      isTrusted: nativeEvent?.isTrusted, visibility: document.visibilityState,
      hidden: document.hidden, focused: document.hasFocus(), phase: this.audio?.phase,
      desired: this.audio?.desired, pressed: this.control.pressed, floorPhase: this.control.floorPhase,
      currentVersion: this.control.state?.current_version, peer: this.audio?.peer,
      track: this.audio?.localStream?.getAudioTracks?.()[0],
      openPeers: [...this.peers].filter(peer => peer.connectionState !== 'closed').length,
      outputAttached: !!this.output.srcObject, ...previous,
    });
  }
  attachRemote(stream, binding) {
    if (this.analyser && this.binding) {
      this.history.push({group_id: this.binding.group_id, current_version: this.binding.current_version, server_epoch: this.binding.server_epoch,
        ended_ms: Math.round(performance.now() - this.startedAt), remote_pcm: remoteObservation(this.segmentSamples), inbound: this.rtpLatest});
      if (this.history.length > 30) this.history.shift();
    }
    this.segmentSamples = [];
    this.receiver?.disconnect(); this.analyser?.disconnect(); this.sink?.disconnect();
    this.receiver = this.analyser = this.sink = null; this.binding = binding;
    if (!stream) return;
    this.rtpLatest = [];
    this.receiver = this.context.createMediaStreamSource(stream);
    this.analyser = this.context.createAnalyser(); this.analyser.fftSize = 8192; this.analyser.smoothingTimeConstant = 0;
    this.sink = this.context.createGain(); this.sink.gain.value = 0;
    this.receiver.connect(this.analyser).connect(this.sink).connect(this.context.destination);
    this.pcm = new Float32Array(this.analyser.fftSize); this.bins = new Float32Array(this.analyser.frequencyBinCount);
  }
  attachLocal(stream) {
    this.sourceReceiver?.disconnect(); this.sourceAnalyser?.disconnect(); this.sourceSink?.disconnect();
    this.sourceStream = stream; this.sourceReceiver = this.sourceAnalyser = this.sourceSink = null;
    if (!stream || this.sourceKind === 'synthetic') return;
    this.sourceReceiver = this.context.createMediaStreamSource(stream); this.sourceAnalyser = this.context.createAnalyser(); this.sourceAnalyser.fftSize = 2048;
    this.sourceSink = this.context.createGain(); this.sourceSink.gain.value = 0;
    this.sourceReceiver.connect(this.sourceAnalyser).connect(this.sourceSink).connect(this.context.destination);
    this.sourcePCM = new Float32Array(this.sourceAnalyser.fftSize);
  }
  observePTT() {
    if (!this.control) return;
    this.driver?.observeFloor();
    if (this.sourceKind === 'synthetic') return;
    if (this.sourceKind === 'synthetic_speech' && (!this.control.pressed || this.control.floorPhase !== 'held')) this.speechSource?.stop();
    if (this.control.pressed && !this.activeWindow) this.activeWindow = {started: performance.now(),
      started_at: new Date().toISOString(), group_id: this.control.state?.current_group_id,
      current_version: this.control.state?.current_version, capture_at_start: captureStatus(this.audio?.localStream), inbound_before: this.rtpLatest};
    if (this.control.floorPhase === 'held' && this.activeWindow && !this.activeWindow.granted_at) { this.activeWindow.granted_at = new Date().toISOString(); this.activeWindow.capture_at_grant = captureStatus(this.audio?.localStream); }
    if (!this.control.pressed && this.activeWindow) {
      const window = this.activeWindow; this.activeWindow = null;
      window.ended = performance.now(); window.ended_at = new Date().toISOString(); window.capture_at_end = captureStatus(this.audio?.localStream);
      window.inbound_after = this.rtpLatest;
      window.source_pcm = sourceObservation(this.sourceSamples.filter(sample => sample.at >= window.started && sample.at <= window.ended));
      window.remote_pcm = remoteObservation(this.samples.filter(sample => sample.at >= window.started && sample.at <= window.ended));
      this.windows.push(window); if (this.windows.length > 30) this.windows.shift();
      // Capture final decoder counters from this Peer only; a later group cannot overwrite this window.
      const peer = this.audio?.peer;
      if (peer) void peer.getStats().then(stats => { window.inbound_after = summarizeRTP(stats); this.changed(); }).catch(() => {});
    }
  }
  observation() {
    const windows = [...this.windows, ...(this.activeWindow ? [{...this.activeWindow, ended: performance.now(), ended_at: null, capture_at_end: captureStatus(this.audio.localStream), inbound_after: this.rtpLatest}] : [])];
    return {terminal: this.label, source_kind: this.sourceKind, current_version: this.control.state?.current_version,
      diagnostic_send: this.driver?.snapshot() || null,
      ...(this.sourceKind === 'synthetic_speech' ? {synthetic_clip: {duration_seconds: round(this.speechBuffer?.duration), channels: this.speechBuffer?.numberOfChannels, sample_rate: this.speechBuffer?.sampleRate},
        remote_rms_frames: this.samples.slice(-200).map(sample => ({elapsed_ms: Math.round(sample.at - this.startedAt), rms: round(sample.rms), valid: !!sample.valid}))} : {}),
      capture: captureStatus(this.audio.localStream), last_capture: this.lastCapture,
      lifecycle: this.lifecycle?.snapshot() || {events: [], dropped_events: 0},
      source_pcm: {...sourceObservation(this.sourceSamples), rolling_window: sampleWindow(this.sourceSamples, this.startedAt)},
      remote_pcm: {...remoteObservation(this.samples), rolling_window: sampleWindow(this.samples, this.startedAt)},
      inbound: this.rtpLatest, receive_history: this.history,
      ptt_windows: windows.map(window => ({started_at: window.started_at, ended_at: window.ended_at,
        from_ms: Math.round(window.started - this.startedAt), to_ms: Math.round(window.ended - this.startedAt),
        duration_ms: Math.round(window.ended - window.started), group_id: window.group_id, current_version: window.current_version,
        granted_at: window.granted_at || null, capture_at_start: window.capture_at_start, capture_at_grant: window.capture_at_grant || null, capture_at_end: window.capture_at_end,
        source_pcm: window.ended_at ? window.source_pcm : sourceObservation(this.sourceSamples.filter(sample => sample.at >= window.started && sample.at <= window.ended)),
        remote_pcm: window.ended_at ? window.remote_pcm : remoteObservation(this.samples.filter(sample => sample.at >= window.started && sample.at <= window.ended)),
        inbound_before: window.inbound_before, inbound_after: window.inbound_after, inbound_delta: rtpDelta(window.inbound_before, window.inbound_after)})),
      note: this.sourceKind === 'synthetic_speech' ? '本机合成语音经正常麦权发送；仅保留聚合 PCM 数值，不保存音频；不代表真人采集或人工听测。' : '仅采集与传输数值观察；远端可来自其他持麦者；RMS=0可为安静环境，RTP增长不代表真实有声；未做人工听测。'};
  }
  sample() {
    if (this.sourceKind !== 'synthetic') {
      if (this.sourceStream !== this.audio.localStream) this.attachLocal(this.audio.localStream);
      if (this.sourceAnalyser && !this.closed && this.context.state === 'running') {
        this.sourceAnalyser.getFloatTimeDomainData(this.sourcePCM); const capture = captureStatus(this.audio.localStream);
        this.sourceSamples.push({at: performance.now(), valid: capture.track_state === 'live', rms: pcmRMS(this.sourcePCM), enabled: capture.enabled, track_state: capture.track_state});
        if (this.sourceSamples.length > SAMPLE_LIMIT) this.sourceSamples.shift(); this.lastCapture = capture;
      }
    }
    if (!this.analyser || this.closed || this.context.state !== 'running') { if (this.driver?.active) this.changed(); return; }
    this.analyser.getFloatTimeDomainData(this.pcm);
    let frequency = null;
    if (this.sourceKind === 'synthetic') { this.analyser.getFloatFrequencyData(this.bins); frequency = spectrumPeak(this.bins, this.context.sampleRate, this.analyser.fftSize).frequency; }
    const sample = {at: performance.now(), valid: this.audio.isReady(), rms: pcmRMS(this.pcm), frequency};
    this.samples.push(sample); if (this.samples.length > SAMPLE_LIMIT) this.samples.shift();
    this.segmentSamples.push(sample); if (this.segmentSamples.length > SAMPLE_LIMIT) this.segmentSamples.shift();
    const tone = this.sourceKind === 'synthetic' && FREQUENCIES.find(value => Math.abs(value - sample.frequency) < 20);
    if (sample.valid && sample.rms > 0.015 && tone) {
      const observed = this.observed.get(tone) || {frequency: tone, first_ms: Math.round(sample.at - this.startedAt), matching_samples: 0, max_rms: 0};
      observed.last_ms = Math.round(sample.at - this.startedAt); observed.matching_samples++;
      observed.max_rms = round(Math.max(observed.max_rms, sample.rms)); this.observed.set(tone, observed);
    }
    if (this.samples.length % 10 === 0) { const peer = this.audio.peer; void this.metrics().then(value => { if (peer === this.audio.peer) this.rtpLatest = value; }).catch(() => {}); }
    this.last = sample; this.changed();
  }
  async identify() {
    if (!this.control.identity) await this.control.createIdentity(`诊断${this.label}-${uid().slice(0, 6)}`);
    else await this.control.refresh();
    assert(this.control.identity && this.control.state && this.control.connected, `${this.label} 身份未建立`);
  }
  async connect(explicit = true) {
    if (explicit) await this.audio.enable(); else await this.audio.sync();
    const until = Date.now() + 10000;
    while (!this.audio.isReady() && Date.now() < until) await sleep(100);
    assert(this.audio.isReady(), `${this.label} 音频未就绪：${this.audio.phase}`);
    if (this.audio.peer) this.peers.add(this.audio.peer);
  }
  async switchTo(group) {
    this.driver?.stop('group-changed');
    this.speechSource?.stop();
    this.attachRemote(null, null);
    this.audio.suspend('diagnostic-switch');
    await this.control.switchGroup(group);
    assert(!this.control.pendingSwitch && this.control.state.current_group_id === group, `${this.label} 切群未确认`);
    await this.connect(this.sourceKind !== 'system_microphone');
  }
  async metrics() {
    const stats = await this.audio.peer?.getStats();
    return stats ? summarizeRTP(stats) : [];
  }
  async measure(duration, expectedFrequency, check = () => {}) {
    assert(this.sourceKind === 'synthetic', '真实麦克风不使用固定频率自动判定');
    const start = performance.now(), before = await this.metrics();
    await sleep(duration); check();
    const samples = this.samples.filter(sample => sample.at >= start), after = await this.metrics();
    const assessment = assessSamples(samples, expectedFrequency);
    const opus = after.some(stat => stat.codec.toLowerCase() === 'audio/opus' && stat.clockRate === 48000);
    const packetDelta = after.reduce((n, stat) => n + stat.packetsReceived, 0) - before.reduce((n, stat) => n + stat.packetsReceived, 0);
    return {terminal: this.label, frequency: expectedFrequency, ...assessment,
      pass: assessment.pass && opus && (expectedFrequency === null || packetDelta > 0),
      opus, packets_delta: packetDelta, inbound: after,
      current_version: this.control.state?.current_version, peer_id: this.audio.peerID};
  }
  async release() {
    this.control.cancelFloor();
    while (this.control.acquiring || this.control.releasing) await sleep(50);
  }
  async close() {
    this.closed = true; clearInterval(this.sampler); clearInterval(this.pollTimer);
    this.driver?.stop('disabled');
    const previous = {previousPeer: this.audio.peer, previousTrack: this.audio.localStream?.getAudioTracks?.()[0]};
    this.speechSource?.stop(); this.control.cancelFloor(); await this.audio.disable(); this.attachRemote(null, null); this.attachLocal(null); this.output.remove();
    this.recordLifecycle('disabled-closed', null, previous);
  }
}

function bootDiagnostics() {
  const $ = id => document.getElementById(id);
  let context = null, terminals = [], single = null, running = false, cancelled = false, report = null, driver = null, initialization = null;
  const check = () => { if (cancelled) throw new Error('测试已停止'); };
  const status = message => { $('status').textContent = message; };
  const render = () => {
    $('enable-single').disabled = running || !!initialization?.busy; $('run-matrix').disabled = running || !!initialization?.busy;
    const container = $('terminals'); container.replaceChildren();
    for (const terminal of terminals) {
      const tile = document.createElement('article'), title = document.createElement('h3'), summary = document.createElement('pre');
      title.textContent = terminal.sourceKind === 'system_microphone' ? `${terminal.label} · 真实系统麦克风` : terminal.sourceKind === 'synthetic_speech' ? `${terminal.label} · 本机合成语音 WAV` : `${terminal.label} · ${terminal.frequency} Hz`;
      summary.textContent = `媒体：${terminal.audio.phase}\n群版本：${terminal.control.state?.current_version ?? '—'}\n麦权：${terminal.control.floorPhase}\n远端 RMS：${round(terminal.last?.rms) ?? '—'}\n远端峰值：${Math.round(terminal.last?.frequency || 0)} Hz\n接收样本：${terminal.samples.length}\n本次启用以来的有效远端频率：${JSON.stringify([...terminal.observed.values()])}\n接收 RTP：${JSON.stringify(terminal.rtpLatest)}`;
      if (terminal.sourceKind !== 'synthetic') summary.textContent = `媒体：${terminal.audio.phase} · 麦权：${terminal.control.floorPhase}\n${JSON.stringify(terminal.observation(), null, 2)}`;
      tile.append(title, summary); container.append(tile);
    }
    if (single?.samples.length >= 3) $('export').disabled = false;
    $('single-source').disabled = !!single || running || !!initialization?.busy;
    const kind = single?.sourceKind || $('single-source').value;
    $('single-frequency').disabled = !!single || kind !== 'synthetic';
    $('speech-file-label').hidden = $('speech-file-help').hidden = kind !== 'synthetic_speech';
    $('single-speech-file').disabled = !!single || running || !!initialization?.busy;
    $('single-tone').hidden = kind !== 'synthetic';
    $('single-tone').disabled = !single || !!driver?.active || !single.control.canPress();
    $('single-speech').hidden = kind !== 'synthetic_speech';
    $('single-speech').disabled = !single || !!driver?.active || !single.control.canPress();
    $('single-send-state').textContent = diagnosticSendMessage(driver?.snapshot(), kind);
    $('single-ptt').hidden = kind === 'synthetic_speech';
    $('single-ptt').disabled = !single || (!single.control.pressed && !single.control.canPress());
    $('single-ptt').setAttribute('aria-pressed', String(!!single?.control.pressed));
    $('single-ptt').textContent = single?.control.floorPhase === 'held' ? '正在发送，松手停发' : '按住发送当前音源';
    $('single-play').disabled = !single?.audio.peer;
    $('single-timed').hidden = (single?.sourceKind || $('single-source').value) !== 'system_microphone';
    $('single-timed').disabled = !single || !!driver?.active || !single.control.canPress();
    $('single-cancel').disabled = !single?.control.pressed && !single?.control.floorRequest && !driver?.active;
    $('single-retry').disabled = !single?.control.connected || !single?.control.state?.current_group_id || running;
    $('single-retry').hidden = !single || single.audio.phase !== 'error';
    $('single-muted').disabled = !single;
    $('single-muted').checked = single ? single.audio.listeningMuted : true;
    const selector = $('single-groups'), selected = selector.value;
    selector.replaceChildren();
    for (const group of single?.control.state?.groups || []) { const option = document.createElement('option'); option.value = group.id; option.textContent = `${group.name}${group.id === single.control.state.current_group_id ? '（当前群）' : ''}`; selector.append(option); }
    if ([...selector.options].some(option => option.value === selected)) selector.value = selected;
    selector.disabled = !single || !!single.control.pendingSwitch;
    $('single-switch').disabled = !single || !selector.value || !!single.control.pendingSwitch;
    $('single-media-state').textContent = single ? `音频：${single.audio.phase}；${single.audio.message || (single.audio.listeningMuted ? '收听静音；远端PCM仍测量，未做人工听测。' : '已允许播放；仍未记录人工听测结论。')}` : '尚未采集麦克风。';
    const events = single?.lifecycle.snapshot().events || [], hiddenClose = events.filter(event => event.event_type === 'hidden-closed');
    const last = events.at(-1), closed = hiddenClose.at(-1);
    $('single-lifecycle-state').textContent = single ? `被动生命周期：${events.length} 条；隐藏关闭 ${hiddenClose.length} 次；最近 ${last?.event_type || '无'}；${closed ? `旧采集 ${closed.previous_capture?.track_state || 'none'} / 旧Peer ${closed.previous_peer?.state || 'none'}` : '尚未观察到隐藏关闭'}。` : '被动生命周期尚未开始；停止后的事件保留在报告中。';
  };
  initialization = new SingleInitialization(render);
  async function ensureContext() {
    if (!context || context.state === 'closed') context = new AudioContext({sampleRate: 48000});
    await context.resume(); assert(context.state === 'running', '请点击启用以允许 WebAudio 运行');
  }
  function addResult(name, measures, extra = {}) {
    const pass = measures.every(m => m.pass) && extra.pass !== false;
    const result = {...extra, name, pass, measurements: measures}; report.results.push(result);
    const row = document.createElement('tr');
    for (const value of [name, pass ? '通过' : '未通过', JSON.stringify(result)]) { const cell = document.createElement('td'); cell.textContent = value; row.append(cell); }
    row.className = pass ? 'pass' : 'fail'; $('results').append(row);
    $('report-summary').textContent = JSON.stringify(safeReport(report), null, 2); $('export').disabled = false;
    return pass;
  }
  async function closeAll(invalidateInitialization = true) {
    if (invalidateInitialization) initialization.cancel();
    const closing = terminals; driver?.stop('disabled'); driver = null;
    await Promise.allSettled(closing.map(t => t.close()));
    if (report && closing.length) report.resources_after_close = closing.map(t => ({terminal: t.label,
      created_peers: t.peers.size, open_peers: [...t.peers].filter(p => p.connectionState !== 'closed').length}));
    if (report && closing.some(t => t.sourceKind !== 'synthetic')) { report.measurements = closing.map(t => t.observation()); report.status = 'observations-only'; report.finished_at = new Date().toISOString(); $('report-summary').textContent = JSON.stringify(safeReport(report), null, 2); $('export').disabled = false; }
    terminals = []; single = null; render();
  }
  async function acquire(terminal) {
    check(); await terminal.control.pressFloor();
    assert(terminal.control.floorPhase === 'held', `${terminal.label} 未获麦权：${terminal.control.notice}`);
  }
  async function transmit(sender, expected, name) {
    status(name); await sleep(1100); check(); await acquire(sender); await sleep(450); check();
    try { return addResult(name, await Promise.all(expected.map(([receiver, hz]) => receiver.measure(1500, hz, check)))); }
    finally { await sender.release(); }
  }
  function newReport(mode, sourceKind = 'synthetic') {
    report = {started_at: new Date().toISOString(), mode, source_kind: sourceKind, user_agent: navigator.userAgent, status: 'running', results: []};
    $('results').replaceChildren(); $('report-summary').textContent = '运行中';
  }
  $('run-matrix').addEventListener('click', async () => {
    if (running || initialization.busy) return; running = true; cancelled = false; $('run-matrix').disabled = true; $('enable-single').disabled = true;
    try {
      await ensureContext(); await closeAll(); newReport('three-logical-terminals-in-one-page');
      terminals = ['A', 'B', 'C'].map((label, i) => new Terminal(label, FREQUENCIES[i], context, render)); render();
      const [a, b, c] = terminals;
      status('建立三个独立测试身份和两个群'); await Promise.all(terminals.map(t => t.identify())); check();
      const created = await a.control.createGroup(`诊断全员-${uid().slice(0, 8)}`);
      const second = await a.control.createGroup(`诊断分组-${uid().slice(0, 8)}`);
      assert(created?.code && second?.code, '测试群未建立');
      await Promise.all([b.control.joinGroup(created.code), c.control.joinGroup(created.code)]);
      await c.control.joinGroup(second.code); await Promise.all(terminals.map(t => t.connect())); check();
      await transmit(a, [[b, 440], [c, 440]], 'A → B/C 一对多 Opus');
      await sleep(1000);
      await transmit(b, [[a, 997], [c, 997]], 'B → A/C 双向与换源');
      status('释放后的尾音排空及两秒静默'); await sleep(1100);
      addResult('松手后排空并连续两秒静默', await Promise.all([a.measure(2200, null, check), c.measure(2200, null, check)]));
      await c.switchTo(second.group.id);
      await transmit(a, [[b, 440], [c, null]], 'C 在第二群：A → B，C 隔离');
      await transmit(c, [[a, null], [b, null]], 'C 在第二群：C → A/B 隔离');
      await c.switchTo(created.group.id);
      status('同时抢麦：只有获权频率可进入 C');
      await Promise.all([a.control.pressFloor(), b.control.pressFloor()]);
      const winners = [a, b].filter(t => t.control.floorPhase === 'held');
      assert(winners.length === 1, '同时抢麦没有恰好一个持有人');
      const winner = winners[0], loser = winner === a ? b : a;
      // Explicit hostile driver: keep rejected sender transmitting to prove server-side gating.
      for (const track of loser.audio.localStream.getAudioTracks()) track.enabled = true;
      await sleep(450); addResult('同时抢麦及败方持续发包隔离', [await c.measure(1800, winner.frequency, check)], {winner: winner.label, loser: loser.label});
      winner.audio.setSending(false);
      await sleep(1100); check();
      const loserBefore = (await loser.audio.stats()).sent_packets;
      const maskedLeak = await c.measure(2200, null, check);
      const loserAfter = (await loser.audio.stats()).sent_packets;
      addResult('赢家静音、败方持续上行：排除主频遮蔽泄漏', [maskedLeak], {
        pass: loserAfter > loserBefore && loser.audio.localStream.getAudioTracks().every(track => track.enabled),
        loser: loser.label, loser_outbound_packets_delta: loserAfter - loserBefore});
      for (const track of loser.audio.localStream.getAudioTracks()) track.enabled = false;
      await Promise.all([a.release(), b.release()]);
      status('故意停止续租并持续上行，验证服务端自然到期门控');
      await acquire(a); await sleep(400);
      a.control.cancel(a.control.renewTimer); a.control.cancel(a.control.expiryTimer);
      a.control.renewTimer = a.control.expiryTimer = null;
      for (const track of a.audio.localStream.getAudioTracks()) track.enabled = true;
      const expiryAt = Date.parse(a.control.lease.expires_at);
      const expiryBaseline = await b.measure(1000, 440, check);
      const sourceBefore = [...await a.audio.peer.getStats()].filter(([, stat]) => stat.type === 'outbound-rtp' && stat.kind === 'audio').map(([, stat]) => stat.packetsSent || 0).reduce((x, y) => x + y, 0);
      await sleep(Math.max(0, expiryAt - a.control.serverNow()) + 1100); check();
      const expiryMeasures = await Promise.all([b.measure(2200, null, check), c.measure(2200, null, check)]);
      const sourceAfter = [...await a.audio.peer.getStats()].filter(([, stat]) => stat.type === 'outbound-rtp' && stat.kind === 'audio').map(([, stat]) => stat.packetsSent || 0).reduce((x, y) => x + y, 0);
      addResult('无续租且持续 RTP 上行：到期后两秒静默', expiryMeasures, {
        pass: expiryBaseline.pass && sourceAfter > sourceBefore && a.audio.localStream.getAudioTracks().every(track => track.enabled),
        positive_baseline: expiryBaseline, source_track_enabled: a.audio.localStream.getAudioTracks().every(track => track.enabled),
        outbound_packets_delta: sourceAfter - sourceBefore});
      await a.release(); await transmit(b, [[a, 997], [c, 997]], '失联到期后其他成员重新获权');
      status('连续十次切群 / Peer 重建与旧群隔离');
      await acquire(a); const switches = [];
      try {
        for (let i = 0; i < 10; i++) {
          check(); const target = i % 2 === 0 ? second.group.id : created.group.id;
          const started = performance.now(); await c.switchTo(target); await sleep(500);
          const measure = await c.measure(600, i % 2 === 0 ? null : 440, check);
          switches.push({index: i + 1, elapsed_ms: Math.round(performance.now() - started), ...measure});
          status(`连续切群 ${i + 1}/10：${measure.pass ? '通过' : '未通过'}`);
        }
      } finally { await a.release(); }
      addResult('十次切群、旧声音与 Peer 残留', switches,
        {pass: switches.every(s => s.pass) && terminals.every(t => [...t.peers].filter(p => p.connectionState !== 'closed').length === 1)});
      report.resources = terminals.map(t => ({terminal: t.label, created_peers: t.peers.size,
        open_peers: [...t.peers].filter(p => p.connectionState !== 'closed').length}));
      report.status = report.results.every(r => r.pass) ? 'passed' : 'failed';
      status(`三终端同页矩阵完成：${report.status === 'passed' ? '全部通过' : '有未通过项'}。不代表系统麦克风 / 扬声器听测。`);
    } catch (error) {
      if (!report) newReport('three-logical-terminals-in-one-page');
      addResult('运行终止', [], {pass: false, reason: cancelled ? '用户停止' : String(error.message).slice(0, 180)});
      report.status = cancelled ? 'stopped' : 'failed'; status(`诊断结束：${report.status}，查看结果。`);
    } finally {
      if (report) { report.finished_at = new Date().toISOString(); $('report-summary').textContent = JSON.stringify(safeReport(report), null, 2); $('export').disabled = false; }
      await Promise.allSettled(terminals.map(t => t.release())); running = false; $('run-matrix').disabled = false; $('enable-single').disabled = false;
    }
  });
  $('enable-single').addEventListener('click', async () => {
    if (running || initialization.busy) return;
    try {
      await initialization.run(async current => {
        await ensureContext(); if (!current()) return null;
        await closeAll(false); if (!current()) return null;
        cancelled = false;
        // Duplicated tabs never reuse a copied diagnostic identity.
        const kind = $('single-source').value;
        const speechBuffer = kind === 'synthetic_speech' ? await loadSyntheticSpeech($('single-speech-file').files[0], context) : null;
        if (!current()) return null;
        const terminal = new Terminal($('single-name').value.trim() || '单端', Number($('single-frequency').value), context, render, null, kind, speechBuffer);
        single = terminal; terminals = [terminal];
        driver = new DiagnosticPTTDriver(terminal.control, {...(kind === 'synthetic_speech' ? {
          duration: speechSendDuration(speechBuffer),
          sourceDuration: Math.ceil(speechBuffer.duration * 1000), drainDuration: 250, timeoutReason: 'source-timeout',
          onGranted: onEnded => { assert(terminal.speechSource, '合成语音尚未连接，请重试连接。'); terminal.speechSource.play(onEnded); },
          onStop: () => terminal.speechSource?.stop(),
        } : {}), onChange: render}); terminal.driver = driver; return terminal;
      }, async (terminal, current) => {
        sessionStorage.setItem(SINGLE_KEY, JSON.stringify(terminal.control.identity));
        terminal.pollTimer = setInterval(async () => { if (single !== terminal || terminal.closed || terminal.control.pressed) return; await terminal.control.refresh(); }, 1200);
        newReport('single-independent-window', terminal.sourceKind);
        await terminal.audio.enable(); if (!current() || single !== terminal || terminal.closed) return;
        for (const id of ['create-group', 'join-group']) $(id).disabled = false;
        if (terminal.control.state.current_group_id) { await terminal.connect(false); if (!current() || single !== terminal || terminal.closed) return; $('single-tone').disabled = false; }
        $('single-status').textContent = terminal.sourceKind === 'system_microphone' ? '真实麦克风模式已准备；建群或入群后请求系统权限，默认收听静音。' : terminal.sourceKind === 'synthetic_speech' ? `已在本页准备 ${round(terminal.speechBuffer.duration)} 秒合成语音；建立或加入同一测试群后可发送，不申请麦克风。` : '本窗口独立测试身份已启用；建立或加入同一测试群。'; render();
      });
    } catch (error) { $('single-status').textContent = `单端初始化失败：${String(error.message).slice(0, 120)}`; }
  });
  async function groupAction(create) {
    const terminal = single; if (!terminal) return;
    driver?.stop('group-changed');
    try {
      const result = create ? await terminal.control.createGroup($('group-name').value.trim()) : await terminal.control.joinGroup($('group-code').value.trim());
      if (single !== terminal || terminal.closed) return;
      assert(result, '建群 / 入群失败');
      if (create) $('share-code').textContent = `仅分享本机测试群口令：${result.code}`;
      await terminal.connect(false); if (single !== terminal || terminal.closed) return; $('single-tone').disabled = false; $('single-status').textContent = terminal.sourceKind === 'system_microphone' ? '真实系统麦克风已连接。按住正常PTT验采集传输；本轮未做人工听测。' : terminal.sourceKind === 'synthetic_speech' ? '真实 Opus Peer 已连接；发送时请留在本窗口，看到“已完整发送”后再切换到接收端。' : '真实 Opus Peer 已连接，可互发测试音。';
    } catch (error) { if (single !== terminal || terminal.closed) return; $('single-status').textContent = `连接失败：${String(error.message).slice(0, 120)}`; }
  }
  $('create-group').addEventListener('click', () => void groupAction(true));
  $('join-group').addEventListener('click', () => void groupAction(false));
  $('single-tone').addEventListener('click', async () => {
    if (!single || single.sourceKind !== 'synthetic' || !single.control.canPress() || driver?.active) return;
    try { await driver.start(); }
    catch (error) { $('single-status').textContent = `测试失败：${String(error.message).slice(0, 120)}`; }
    finally { render(); }
  });
  $('single-speech').addEventListener('click', async () => {
    if (!single || single.sourceKind !== 'synthetic_speech' || !single.control.canPress() || driver?.active) return;
    const terminal = single, sender = driver, peer = terminal.audio.peer;
    $('single-status').textContent = '发送时请留在本窗口；发送进度和停止原因显示在按钮下方。';
    try {
      const pending = sender.start({prepare: ensureContext,
        isCurrent: () => single === terminal && driver === sender && !terminal.closed && terminal.audio.peer === peer && terminal.audio.isReady() && !document.hidden && document.hasFocus()});
      render(); await pending;
    } catch { $('single-status').textContent = '合成语音发送未完成，请确认群状态和音源后重试。'; } render();
  });
  $('stop').addEventListener('click', async () => {
    cancelled = true; await closeAll(); for (const id of ['single-tone', 'create-group', 'join-group']) $(id).disabled = true;
    status('全部测试音频已关闭。');
  });
  async function updateReport() {
    if (!report) return;
    if (single) {
      report.finished_at = new Date().toISOString(); report.status = 'observations-only';
      report.measurements = [{terminal: single.label, current_version: single.control.state?.current_version,
        observed_remote_tones: [...single.observed.values()],
        window: {from_ms: Math.round((single.samples.at(-600)?.at || single.samples[0]?.at || single.startedAt) - single.startedAt),
          to_ms: Math.round((single.samples.at(-1)?.at || single.startedAt) - single.startedAt)},
        remote_pcm: single.samples.slice(-600).map(s => ({elapsed_ms: Math.round(s.at - single.startedAt),
          valid: s.valid, rms: round(s.rms), frequency: round(s.frequency)})), inbound: await single.metrics()}];
    }
    if (single && single.sourceKind !== 'synthetic') { single.rtpLatest = await single.metrics(); report.measurements = [single.observation()]; }
    $('report-summary').textContent = JSON.stringify(safeReport(report), null, 2);
  }
  $('show-report').addEventListener('click', () => { void updateReport(); });
  $('export').addEventListener('click', async () => {
    if (!report) return; await updateReport();
    const blob = new Blob([JSON.stringify(safeReport(report), null, 2)], {type: 'application/json'});
    const link = document.createElement('a'), url = URL.createObjectURL(blob);
    link.href = url; link.download = `shenglu-audio-${Date.now()}.json`; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  });
  // Standalone capture follows the ordinary product's pointer and lifecycle rules.
  const endPress = bindDiagnosticPTTPointer($('single-ptt'), {
    get pressed() { return !!single?.control.pressed; }, get floorRequest() { return single?.control.floorRequest; },
    canPress: () => !running && !driver?.active && !!single?.control.canPress(), pressFloor: () => single?.control.pressFloor(), cancelFloor: () => single?.control.cancelFloor(),
  }, () => driver);
  $('single-ptt').addEventListener('keydown', event => { if ([' ', 'Enter'].includes(event.key)) { event.preventDefault(); if (!event.repeat && !running && !driver?.active) void single?.control.pressFloor(); } });
  $('single-ptt').addEventListener('keyup', event => { if ([' ', 'Enter'].includes(event.key)) { event.preventDefault(); endPress(); } });
  $('single-ptt').addEventListener('blur', endPress);
  $('single-timed').addEventListener('click', async () => {
    if (!single || single.sourceKind !== 'system_microphone' || !single.control.canPress() || driver?.active) return;
    $('single-status').textContent = '显式诊断驱动：正常获权后最多发送3秒，可立即停止；失焦/隐藏立即取消。未做人工听测。';
    try { await driver.start(); } catch { $('single-status').textContent = '诊断申请未完成，请确认状态后重试。'; } render();
  });
  $('single-cancel').addEventListener('click', () => { endPress('user-cancelled'); render(); });
  $('single-play').addEventListener('click', async () => {
    if (!single) return;
    await ensureContext(); single.audio.setListeningMuted(false); await single.audio.retryPlayback(); render();
  });
  $('single-muted').addEventListener('change', () => single?.audio.setListeningMuted($('single-muted').checked));
  $('single-retry').addEventListener('click', async () => {
    if (!single) return;
    try { await ensureContext(); await single.connect(); } catch (error) { $('single-status').textContent = `重试失败：${String(error.message).slice(0, 120)}`; }
  });
  $('single-switch').addEventListener('click', async () => {
    if (!single || !$('single-groups').value) return;
    try { endPress('group-changed'); await single.switchTo($('single-groups').value); $('single-status').textContent = '当前群已确认；恢复后未按麦。'; }
    catch (error) { $('single-status').textContent = `切群未完成：${String(error.message).slice(0, 120)}`; }
  });
  $('single-source').addEventListener('change', render);
  window.addEventListener('blur', event => {
    if (running || !single) return;
    single.recordLifecycle('window-blur', event); endPress('window-blur'); single.recordLifecycle('blur-cancelled', event);
  });
  window.addEventListener('focus', event => { if (!running && single) single.recordLifecycle('window-focus', event); });
  document.addEventListener('visibilitychange', event => {
    if (running || !single) return;
    if (document.hidden) {
      single.recordLifecycle('visibility-hidden', event);
      const previous = {previousPeer: single.audio.peer, previousTrack: single.audio.localStream?.getAudioTracks?.()[0]};
      endPress('page-hidden'); single.audio.suspend('页面隐藏，采集与旧Peer已关闭。');
      single.recordLifecycle('hidden-closed', event, previous);
    } else { single.recordLifecycle('visibility-visible', event); void single.control.refresh(); }
  });
  window.addEventListener('offline', () => { if (!running && single) { endPress('offline'); single.control.connected = false; single.audio.suspend('离线，采集与旧Peer已关闭。'); } });
  window.addEventListener('online', () => { if (!running && single && !document.hidden) void single.control.refresh(); });
  window.addEventListener('pagehide', () => { void closeAll(); });
  render();
}
if (typeof document !== 'undefined' && document.getElementById('run-matrix')) bootDiagnostics();
