const bindingKey = value => value ? [value.device_id, value.server_epoch, value.group_id, value.current_version].join('|') : '';
const systemSource = () => navigator.mediaDevices.getUserMedia({audio: {channelCount: {ideal: 1}, echoCancellation: true, noiseSuppression: true, autoGainControl: true}, video: false});
const sourceObject = value => value?.stream ? value : {stream: value};
const stopSource = source => {
  for (const track of source?.stream?.getTracks?.() || []) { track.enabled = false; track.stop(); }
  try { Promise.resolve(source?.close?.()).catch(() => {}); } catch { /* Already closed. */ }
};
export function audioErrorText(error) {
  if (['NotAllowedError', 'PermissionDeniedError'].includes(error?.name)) return '麦克风权限未获允许，请在浏览器中允许后重试。';
  if (['NotFoundError', 'DevicesNotFoundError'].includes(error?.name)) return '没有找到可用麦克风，请连接设备后重试。';
  if (error?.code === 'media_unavailable') return '本机服务暂未开放音频。';
  if (error?.code === 'media_context_changed') return '群状态已变化，正在等待确认后恢复音频。';
  return '音频连接未完成，请检查本机服务和设备后重试。';
}
export class AudioClient {
  constructor({request, scopeRequest = null, getContext, serverNow = Date.now, sourceFactory = systemSource,
    createPeer = () => new RTCPeerConnection({iceServers: []}), createStream = tracks => new MediaStream(tracks),
    getCodecs = () => globalThis.RTCRtpReceiver?.getCapabilities('audio')?.codecs || [],
    output, random = () => crypto.randomUUID(), onRemoteStream = () => {}, onChange = () => {}, onFailure = () => {},
    schedule = (callback, delay) => globalThis.setTimeout(callback, delay), cancel = id => globalThis.clearTimeout(id)} = {}) {
    Object.assign(this, {request, scopeRequest, getContext, serverNow, sourceFactory, createPeer, createStream, getCodecs, output, random, onRemoteStream, onChange, onFailure, schedule, cancel});
    this.desired = false; this.phase = 'disabled'; this.message = ''; this.generation = 0;
    this.current = null; this.listeningMuted = false; this.retryRequired = false;
  }
  get peerID() { return this.current?.id || ''; }
  get peer() { return this.current?.peer || null; }
  get binding() { return this.current?.binding || null; }
  get localStream() { return this.current?.source?.stream || null; }
  get remoteStream() { return this.current?.remote || null; }
  emit() { this.onChange(this); }
  contextMatches(session) { return !!session && !session.closed && this.current === session && session.generation === this.generation && this.desired && bindingKey(this.getContext()) === bindingKey(session.binding); }
  isReady() { return this.phase === 'ready' && this.contextMatches(this.current) && this.peer?.connectionState === 'connected' && this.current.playing; }
  setSending(enabled) {
    const send = !!enabled && this.isReady();
    for (const track of this.localStream?.getAudioTracks?.() || []) track.enabled = send;
    return send;
  }
  setListeningMuted(muted) { this.listeningMuted = !!muted; if (this.output) this.output.muted = this.listeningMuted; this.emit(); }
  async enable() {
    this.desired = true; this.retryRequired = false;
    if (this.phase === 'playback_blocked') return this.retryPlayback();
    return this.sync();
  }
  disable() { this.desired = false; this.retryRequired = false; this.suspend(); }
  suspend(message = '') {
    ++this.generation;
    const old = this.current; this.current = null;
    this.closeSession(old);
    this.phase = this.desired ? (this.retryRequired ? 'error' : 'waiting') : 'disabled';
    if (!this.retryRequired || !this.message) this.message = message;
    this.emit();
  }
  closeSession(session) {
    if (!session || session.closed) return;
    session.closed = true; session.abort.abort();
    stopSource(session.source);
    if (session.peer) {
      session.peer.ontrack = session.peer.onconnectionstatechange = session.peer.oniceconnectionstatechange = null;
      session.peer.close();
    }
    if (this.output?.srcObject && this.output.srcObject === session.remote) {
      this.output.pause(); this.output.srcObject = null;
    }
    if (session.remote) this.onRemoteStream(null, session.binding);
    session.remote = null;
    if (session.posted) void this.deletePeer(session.id, session.request);
  }
  async deletePeer(id, request = this.request) { try { await request(`/media/peers/${encodeURIComponent(id)}`, 'DELETE', undefined, {keepalive: true}); } catch { /* Peer expiry and server binding checks remain authoritative. */ } }
  fail(session, error) {
    if (!this.contextMatches(session)) return;
    this.setSending(false); this.current = null; ++this.generation; this.closeSession(session);
    this.phase = 'error'; this.retryRequired = true; this.message = audioErrorText(error); this.emit(); this.onFailure(this.message);
  }
  async sync() {
    if (!this.desired) return;
    const context = this.getContext();
    if (!context) { if (this.current) this.suspend(); else if (!this.retryRequired) { this.phase = 'waiting'; this.emit(); } return; }
    if (this.current && this.contextMatches(this.current)) return this.current.task;
    if (this.retryRequired) return;
    this.suspend();
    const session = {request: this.scopeRequest ? this.scopeRequest() : this.request, id: this.random(), generation: this.generation, binding: {...context}, abort: new AbortController(), closed: false, source: null, peer: null, remote: null, playing: false, posted: false};
    this.current = session; this.phase = 'connecting'; this.message = ''; this.emit();
    session.task = this.connect(session); return session.task;
  }
  async connect(session) {
    try {
      // Invoke the source factory within the explicit enable click before yielding.
      const sourcePromise = Promise.resolve(this.sourceFactory()).then(value => {
        const source = sourceObject(value);
        for (const track of source.stream?.getAudioTracks?.() || []) track.enabled = false;
        if (!this.contextMatches(session)) { stopSource(source); return null; }
        session.source = source; return source;
      });
      const [source, capability] = await Promise.all([sourcePromise, session.request('/media')]);
      if (!this.contextMatches(session) || !source) return;
      if (!capability.available) throw Object.assign(new Error('unavailable'), {code: 'media_unavailable'});
      if (capability.server_epoch !== session.binding.server_epoch) throw Object.assign(new Error('context changed'), {code: 'media_context_changed'});
      const track = source.stream?.getAudioTracks?.()[0];
      if (!track) throw new Error('missing audio track');
      const peer = this.createPeer(); session.peer = peer;
      const transceiver = peer.addTransceiver(track, {direction: 'sendrecv', streams: [source.stream]});
      const codecs = this.getCodecs().filter(codec => codec.mimeType?.toLowerCase() === 'audio/opus');
      if (!codecs.length) throw new Error('opus unavailable');
      transceiver.setCodecPreferences?.(codecs);
      track.onended = () => this.fail(session, new Error('capture ended'));
      peer.ontrack = event => { void this.receiveTrack(session, event); };
      peer.onconnectionstatechange = () => {
        if (!this.contextMatches(session)) return;
        if (['failed', 'closed', 'disconnected'].includes(peer.connectionState)) this.fail(session, new Error('peer disconnected'));
        else this.updateReady(session);
      };
      peer.oniceconnectionstatechange = () => {
        if (this.contextMatches(session) && ['failed', 'disconnected'].includes(peer.iceConnectionState)) this.fail(session, new Error('ice disconnected'));
      };
      const offer = await peer.createOffer();
      if (!this.contextMatches(session)) return;
      await peer.setLocalDescription(offer);
      await this.waitForICE(session);
      if (!this.contextMatches(session)) return;
      const body = {peer_id: session.id, replace_peer_id: capability.peer_id || '', group_id: session.binding.group_id,
        current_version: session.binding.current_version, server_epoch: session.binding.server_epoch,
        valid_until: new Date(this.serverNow() + 9000).toISOString(), offer: {type: 'offer', sdp: peer.localDescription.sdp}};
      session.posted = true;
      const answer = await session.request('/media/peers', 'POST', body);
      if (!this.contextMatches(session)) { await this.deletePeer(session.id, session.request); return; }
      if (answer.peer_id !== session.id || answer.group_id !== session.binding.group_id || answer.current_version !== session.binding.current_version ||
        answer.server_epoch !== session.binding.server_epoch || answer.answer?.type !== 'answer' || typeof answer.answer.sdp !== 'string') throw new Error('invalid answer binding');
      await peer.setRemoteDescription(answer.answer);
      if (this.contextMatches(session)) this.updateReady(session);
    } catch (error) { if (this.contextMatches(session)) this.fail(session, error); }
  }
  waitForICE(session) {
    const peer = session.peer;
    if (peer.iceGatheringState === 'complete') return Promise.resolve();
    return new Promise((resolve, reject) => {
      let timer;
      const cleanup = () => { this.cancel(timer); peer.removeEventListener('icegatheringstatechange', changed); session.abort.signal.removeEventListener('abort', aborted); };
      const changed = () => { if (peer.iceGatheringState === 'complete') { cleanup(); resolve(); } };
      const aborted = () => { cleanup(); reject(new Error('media cancelled')); };
      peer.addEventListener('icegatheringstatechange', changed);
      session.abort.signal.addEventListener('abort', aborted, {once: true});
      timer = this.schedule(() => { cleanup(); reject(new Error('ice gathering timed out')); }, 5000);
      if (session.abort.signal.aborted) aborted(); else changed();
    });
  }
  async receiveTrack(session, event) {
    if (!this.contextMatches(session)) { event.track?.stop?.(); return; }
    const stream = event.streams?.[0] || this.createStream([event.track]);
    if (session.remote && session.remote !== stream) { this.fail(session, new Error('multiple remote streams')); return; }
    session.remote = stream; this.onRemoteStream(stream, session.binding);
    this.output.srcObject = stream; this.output.muted = this.listeningMuted;
    await this.play(session);
  }
  async play(session) {
    try {
      await this.output.play();
      if (!this.contextMatches(session)) return;
      session.playing = true; this.phase = 'connecting'; this.updateReady(session);
    } catch {
      if (!this.contextMatches(session)) return;
      session.playing = false; this.setSending(false); this.phase = 'playback_blocked';
      this.message = '浏览器暂停了声音播放，请点击开始收听。'; this.emit(); this.onFailure(this.message);
    }
  }
  async retryPlayback() { if (this.current && this.contextMatches(this.current)) return this.play(this.current); }
  updateReady(session) {
    if (!this.contextMatches(session) || this.phase === 'playback_blocked') return;
    if (session.peer?.connectionState === 'connected' && session.playing) { this.phase = 'ready'; this.message = ''; }
    else this.phase = 'connecting';
    this.emit();
  }
  async stats() {
    const session = this.current;
    const result = {peer_id: session?.id || '', binding: session ? {...session.binding} : null, phase: this.phase,
      codec: '', sent_packets: 0, sent_bytes: 0, received_packets: 0, received_bytes: 0, received_energy: 0, received_duration: 0,
      connection_state: session?.peer?.connectionState || 'closed', ice_state: session?.peer?.iceConnectionState || 'closed'};
    if (!session?.peer) return result;
    const report = await session.peer.getStats();
    if (!this.contextMatches(session)) return {...result, phase: 'closed'};
    report.forEach(item => {
      if (item.type === 'outbound-rtp' && (item.kind || item.mediaType) === 'audio') { result.sent_packets += item.packetsSent || 0; result.sent_bytes += item.bytesSent || 0; }
      if (item.type === 'inbound-rtp' && (item.kind || item.mediaType) === 'audio') {
        result.received_packets += item.packetsReceived || 0; result.received_bytes += item.bytesReceived || 0;
        result.received_energy += item.totalAudioEnergy || 0; result.received_duration += item.totalSamplesDuration || 0;
      }
      if (item.type === 'codec' && item.mimeType?.toLowerCase() === 'audio/opus') result.codec = item.mimeType;
    });
    return result;
  }
}
