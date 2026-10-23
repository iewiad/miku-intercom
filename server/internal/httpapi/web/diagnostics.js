import {ControlClient} from './app.js';
import {AudioClient} from './audio.js';

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const uid = () => crypto.randomUUID();
const SINGLE_KEY = 'shenglu.diagnostics.single.v1';
const FREQUENCIES = [440, 997, 1777];
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
export function safeReport(report) {
  // Export an explicit diagnostic schema; never serialize a client/Peer/identity or arbitrary errors.
  return {schema: 'shenglu.browser-audio-diagnostic.v1', started_at: report.started_at,
    finished_at: report.finished_at, mode: report.mode, scope: 'synthetic audio; real browser WebRTC; no microphone/speaker listening',
    user_agent: report.user_agent, status: report.status, results: report.results,
    resources: report.resources || [], resources_after_close: report.resources_after_close || [], measurements: report.measurements || []};
}

class Terminal {
  constructor(label, frequency, context, changed, identity = null) {
    Object.assign(this, {label, frequency, context, changed});
    this.samples = []; this.peers = new Set(); this.analyser = null; this.receiver = null;
    this.observed = new Map(); this.rtpLatest = []; this.startedAt = performance.now();
    this.binding = null; this.closed = false; this.pollTimer = null;
    this.output = document.createElement('audio'); this.output.muted = true; this.output.autoplay = true;
    this.output.hidden = true; document.body.append(this.output);
    this.control = new ControlClient({onChange: () => changed(), onIdentity: () => changed()});
    if (identity) this.control.load(identity);
    this.audio = new AudioClient({
      request: (...args) => this.control.request(...args), serverNow: () => this.control.serverNow(),
      getContext: () => this.control.state && !this.control.pendingSwitch && this.control.connected ? {
        device_id: this.control.identity.device.id, group_id: this.control.state.current_group_id,
        current_version: this.control.state.current_version, server_epoch: this.control.state.server_epoch,
      } : null,
      sourceFactory: async () => {
        const source = context.createOscillator(), gain = context.createGain(), destination = context.createMediaStreamDestination();
        source.type = 'sine'; source.frequency.value = frequency; gain.gain.value = 0.1;
        source.connect(gain).connect(destination); source.start();
        return {stream: destination.stream, close: () => { source.stop(); source.disconnect(); gain.disconnect(); }};
      },
      output: this.output,
      onRemoteStream: (stream, binding) => this.attachRemote(stream, binding),
      onChange: () => { if (this.audio?.peer) this.peers.add(this.audio.peer); changed(); },
      onFailure: () => this.control.cancelFloor('媒体连接失败'),
    });
    this.audio.setListeningMuted(true);
    this.control.audio = this.audio;
    this.sampler = setInterval(() => this.sample(), 100);
  }
  attachRemote(stream, binding) {
    this.receiver?.disconnect(); this.analyser?.disconnect(); this.sink?.disconnect();
    this.receiver = this.analyser = this.sink = null; this.binding = binding;
    if (!stream) return;
    this.receiver = this.context.createMediaStreamSource(stream);
    this.analyser = this.context.createAnalyser(); this.analyser.fftSize = 8192; this.analyser.smoothingTimeConstant = 0;
    this.sink = this.context.createGain(); this.sink.gain.value = 0;
    this.receiver.connect(this.analyser).connect(this.sink).connect(this.context.destination);
    this.pcm = new Float32Array(this.analyser.fftSize); this.bins = new Float32Array(this.analyser.frequencyBinCount);
  }
  sample() {
    if (!this.analyser || this.closed || this.context.state !== 'running') return;
    this.analyser.getFloatTimeDomainData(this.pcm); this.analyser.getFloatFrequencyData(this.bins);
    const peak = spectrumPeak(this.bins, this.context.sampleRate, this.analyser.fftSize);
    const sample = {at: performance.now(), valid: this.audio.isReady(), rms: pcmRMS(this.pcm), frequency: peak.frequency};
    this.samples.push(sample); if (this.samples.length > 3000) this.samples.shift();
    const frequency = FREQUENCIES.find(value => Math.abs(value - sample.frequency) < 20);
    if (sample.valid && sample.rms > 0.015 && frequency) {
      const observed = this.observed.get(frequency) || {frequency, first_ms: Math.round(sample.at - this.startedAt), matching_samples: 0, max_rms: 0};
      observed.last_ms = Math.round(sample.at - this.startedAt); observed.matching_samples++;
      observed.max_rms = round(Math.max(observed.max_rms, sample.rms)); this.observed.set(frequency, observed);
    }
    if (this.samples.length % 10 === 0) void this.metrics().then(value => { this.rtpLatest = value; }).catch(() => {});
    this.last = sample; this.changed();
  }
  async identify() {
    if (!this.control.identity) await this.control.createIdentity(`诊断${this.label}-${uid().slice(0, 6)}`);
    else await this.control.refresh();
    assert(this.control.identity && this.control.state && this.control.connected, `${this.label} 身份未建立`);
  }
  async connect() {
    await this.audio.enable();
    const until = Date.now() + 10000;
    while (!this.audio.isReady() && Date.now() < until) await sleep(100);
    assert(this.audio.isReady(), `${this.label} 音频未就绪：${this.audio.phase}`);
    if (this.audio.peer) this.peers.add(this.audio.peer);
  }
  async switchTo(group) {
    this.attachRemote(null, null);
    this.audio.suspend('diagnostic-switch');
    await this.control.switchGroup(group);
    assert(!this.control.pendingSwitch && this.control.state.current_group_id === group, `${this.label} 切群未确认`);
    await this.connect();
  }
  async metrics() {
    const stats = await this.audio.peer?.getStats();
    return stats ? summarizeRTP(stats) : [];
  }
  async measure(duration, expectedFrequency, check = () => {}) {
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
    this.control.cancelFloor(); await this.audio.disable(); this.attachRemote(null, null); this.output.remove();
  }
}

function bootDiagnostics() {
  const $ = id => document.getElementById(id);
  let context = null, terminals = [], single = null, running = false, cancelled = false, report = null;
  const check = () => { if (cancelled) throw new Error('测试已停止'); };
  const status = message => { $('status').textContent = message; };
  const render = () => {
    const container = $('terminals'); container.replaceChildren();
    for (const terminal of terminals) {
      const tile = document.createElement('article'), title = document.createElement('h3'), summary = document.createElement('pre');
      title.textContent = `${terminal.label} · ${terminal.frequency} Hz`;
      summary.textContent = `媒体：${terminal.audio.phase}\n群版本：${terminal.control.state?.current_version ?? '—'}\n麦权：${terminal.control.floorPhase}\n远端 RMS：${round(terminal.last?.rms) ?? '—'}\n远端峰值：${Math.round(terminal.last?.frequency || 0)} Hz\n接收样本：${terminal.samples.length}\n本次启用以来的有效远端频率：${JSON.stringify([...terminal.observed.values()])}\n接收 RTP：${JSON.stringify(terminal.rtpLatest)}`;
      tile.append(title, summary); container.append(tile);
    }
    if (single?.samples.length >= 3) $('export').disabled = false;
  };
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
  async function closeAll() {
    const closing = terminals;
    await Promise.allSettled(closing.map(t => t.close()));
    if (report && closing.length) report.resources_after_close = closing.map(t => ({terminal: t.label,
      created_peers: t.peers.size, open_peers: [...t.peers].filter(p => p.connectionState !== 'closed').length}));
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
  function newReport(mode) {
    report = {started_at: new Date().toISOString(), mode, user_agent: navigator.userAgent, status: 'running', results: []};
    $('results').replaceChildren(); $('report-summary').textContent = '运行中';
  }
  $('run-matrix').addEventListener('click', async () => {
    if (running) return; running = true; cancelled = false; $('run-matrix').disabled = true; $('enable-single').disabled = true;
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
    if (running) return;
    try {
      await ensureContext(); await closeAll(); cancelled = false;
      // A duplicated browser tab can inherit sessionStorage. Each explicit enable creates
      // its own identity rather than trusting a copied token as a distinct terminal.
      single = new Terminal($('single-name').value.trim() || '单端', Number($('single-frequency').value), context, render);
      terminals = [single]; await single.identify(); sessionStorage.setItem(SINGLE_KEY, JSON.stringify(single.control.identity));
      single.pollTimer = setInterval(async () => { if (!single || single.control.pressed) return; await single.control.refresh(); }, 1200);
      newReport('single-independent-tab');
      for (const id of ['create-group', 'join-group']) $(id).disabled = false;
      if (single.control.state.current_group_id) { await single.connect(); $('single-tone').disabled = false; }
      $('single-status').textContent = '本标签独立测试身份已启用；建立或加入同一测试群。'; render();
    } catch (error) { $('single-status').textContent = `单端初始化失败：${String(error.message).slice(0, 120)}`; }
  });
  async function groupAction(create) {
    if (!single) return;
    try {
      const result = create ? await single.control.createGroup($('group-name').value.trim()) : await single.control.joinGroup($('group-code').value.trim());
      assert(result, '建群 / 入群失败');
      if (create) $('share-code').textContent = `仅分享本机测试群口令：${result.code}`;
      await single.connect(); $('single-tone').disabled = false; $('single-status').textContent = '真实 Opus Peer 已连接，可互发测试音。';
    } catch (error) { $('single-status').textContent = `连接失败：${String(error.message).slice(0, 120)}`; }
  }
  $('create-group').addEventListener('click', () => void groupAction(true));
  $('join-group').addEventListener('click', () => void groupAction(false));
  $('single-tone').addEventListener('click', async () => {
    if (!single) return; $('single-tone').disabled = true;
    try { await acquire(single); $('single-status').textContent = '获权后发送合成测试音，3 秒后释放。'; await sleep(3000); }
    catch (error) { $('single-status').textContent = `测试失败：${String(error.message).slice(0, 120)}`; }
    finally { await single?.release(); $('single-tone').disabled = false; $('single-status').textContent = '测试音已停止；远端记录自动保留在报告中。'; }
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
    $('report-summary').textContent = JSON.stringify(safeReport(report), null, 2);
  }
  $('show-report').addEventListener('click', () => { void updateReport(); });
  $('export').addEventListener('click', async () => {
    if (!report) return; await updateReport();
    const blob = new Blob([JSON.stringify(safeReport(report), null, 2)], {type: 'application/json'});
    const link = document.createElement('a'), url = URL.createObjectURL(blob);
    link.href = url; link.download = `shenglu-audio-${Date.now()}.json`; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  });
  // These affect only standalone manual test operation; the same-page automatic driver is explicit.
  window.addEventListener('blur', () => { if (!running) single?.control.cancelFloor(); });
  document.addEventListener('visibilitychange', () => { if (document.hidden && !running) single?.control.cancelFloor(); });
  window.addEventListener('pagehide', () => { void closeAll(); });
}
if (typeof document !== 'undefined' && document.getElementById('run-matrix')) bootDiagnostics();
