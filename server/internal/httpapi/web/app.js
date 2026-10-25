import {AudioClient} from './audio.js';
import {VoiceCommandClient} from './voice.js';
import {RecommendationClient} from './recommendation.js';
const API = '/api/v1';
export const IDENTITY_KEY = 'shenglu.control.identity.v1';
export const pendingKey = id => `shenglu.control.pending.v1.${id}`;
export const ownerKey = id => `shenglu.control.owner.v1.${id}`;
const randomID = () => globalThis.crypto.randomUUID();

export function claimOwner(storage, id, tab, force = false) {
  const key = ownerKey(id);
  const owner = storage.getItem(key);
  if (owner && owner !== tab && !force) return false;
  storage.setItem(key, tab);
  return storage.getItem(key) === tab;
}
export function bindPTTPointer(button, client) {
  let activePointer = null;
  const endPress = () => {
    activePointer = null;
    if (client.pressed || client.floorRequest) client.cancelFloor();
  };
  button.addEventListener('pointerdown', event => {
    if (activePointer !== null || event.button !== 0 || !client.canPress()) return;
    event.preventDefault(); activePointer = event.pointerId;
    button.setPointerCapture(event.pointerId); void client.pressFloor();
  });
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) button.addEventListener(type, event => {
    if (event.pointerId === activePointer) endPress();
  });
  return endPress;
}
// The product's audio-off action also ends command capture synchronously. Do not
// advance the control epoch when there is no PTT request: a submitted durable
// switch must still be able to confirm its in-flight response.
export function bindAudioDisable(button, client) {
  button.addEventListener('click', () => {
    client.voice?.interrupt('音频已关闭，语音指令已停止。');
    client.recommendation?.interrupt(client.recommendation.mode === 'target' ? '音频已关闭，目标核对已取消。' : '音频已关闭，需求推荐已取消。');
    if (client.pressed || client.floorRequest) client.cancelFloor();
    client.audio?.disable();
  });
}
export const validName = value => typeof value === 'string' && [...value.trim()].length >= 1 && [...value.trim()].length <= 32;
// Unicode White_Space matches Go strings.TrimSpace; JS trim also removes FEFF
// and does not remove NEL, so it would normalize this API field differently.
const trimPurpose = value => value.replace(/^\p{White_Space}+|\p{White_Space}+$/gu, '');
export const validPurpose = value => typeof value === 'string' && [...trimPurpose(value)].length <= 256 && !/\p{Cc}/u.test(trimPurpose(value));
const groupPurpose = group => typeof group.purpose === 'string' ? group.purpose : '';
export function filterGroups(groups, query = '') {
  const term = query.trim().toLowerCase();
  return term ? groups.filter(group => `${group.name}\n${groupPurpose(group)}`.toLowerCase().includes(term)) : groups;
}
export class APIError extends Error {
  constructor(status, code, message) { super(message); this.status = status; this.code = code; }
}
const messages = {
  floor_busy: '伙伴正在发言，稍后再按住说话。',
  join_rate_limited: '口令尝试次数较多，请稍后再试。',
  request_ended: '这次发言已结束，请重新按住说话。',
  request_capacity: '操作较频繁，请稍后再按住说话。',
  lease_not_held: '这次发言已结束，请重新按住说话。',
  operation_capacity: '此浏览器的切换记录已满，请联系维护者处理。',
  version_conflict: '当前群版本已变化，已刷新状态，请重新操作。',
  epoch_conflict: '连接正在恢复，请稍后重新按住说话。',
  request_expired: '这次申请已过期，请松手后重新按下。',
  request_cancelled: '这次申请已取消，请重新按下。',
  not_member: '尚未加入这个群，无法切换。',
  invalid_code: '口令不正确，请检查后再试。',
  rate_limited: '尝试次数较多，请稍后再试。',
  recommendation_expired: '建议已过期，请重新获取。',
  recommendation_stale: '群组或职责已变化，请重新获取建议。',
  recommendation_consumed: '这条建议已经确认，请重新获取建议。',
  recommendation_unavailable: '需求推荐暂不可用，请从群列表选择。',
  recommendation_unknown: '建议已失效，请重新获取。',
  recommendation_candidates_changed: '已加入群或职责已变化，请重新获取建议。',
  recommendation_target_invalid: '目标不属于本次建议，请重新获取建议。',
  server_restarted: '服务已重新启动，请重新获取建议。',
};
export const errorText = error => error.status === 401 ? '本机身份已失效，请重新建立身份并重新入群。' : messages[error.code] || (error instanceof APIError ? error.message : '网络请求未确认，请检查本机服务后重试。');

export class ControlClient {
  constructor({fetchImpl = globalThis.fetch.bind(globalThis), now = Date.now, random = randomID,
    schedule = (callback, delay) => globalThis.setTimeout(callback, delay), cancel = id => globalThis.clearTimeout(id), authorize = () => true,
    onChange = () => {}, onIdentity = () => {}, onPending = () => {}, restorePending = null, audio = null} = {}) {
    Object.assign(this, {fetchImpl, now, random, schedule, cancel, authorize, onChange, onIdentity, onPending, restorePending, audio});
    this.identity = null; this.state = null; this.stateAt = 0; this.epoch = 0;
    this.active = true; this.authInvalid = false; this.connected = false; this.mutationBusy = false; this.polling = false; this.uiBlocked = false;
    this.pendingSwitch = null; this.confirming = false; this.retryTimer = null;
    this.floorGeneration = 0; this.pressed = false; this.floorPhase = 'idle';
    this.floorRequest = null; this.lease = null; this.acquiring = false; this.releasing = 0;
    this.renewTimer = null; this.expiryTimer = null; this.share = null;
    this.notice = ''; this.noticeTone = 'info'; this.lastRefreshError = '';
  }
  emit() { this.onChange(this); }
  inform(text, tone = 'info') { this.notice = text; this.noticeTone = tone; this.emit(); }
  allowed() { return !!this.identity && this.active && this.authorize(); }
  setUIBlocked(blocked) {
    this.uiBlocked = !!blocked;
    // Synchronous cancellation also invalidates an acquire that resolves after
    // the user has opened a secondary task. Closing never presses or syncs audio.
    if (this.uiBlocked) this.cancelFloor();
    else this.emit();
  }
  load(identity, pending = null) { if (this.identity?.token !== identity?.token) { this.voice?.interrupt('身份已变化，语音采集已停止。'); this.cancelFloor(); this.audio?.suspend(); this.recommendation?.interrupt(); } this.identity = identity; this.pendingSwitch = pending; this.emit(); }
  async request(path, method = 'GET', body, {token = this.identity?.token, keepalive = false, timeoutMs = 8000} = {}) {
    const headers = {'Accept': 'application/json'};
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    if (token) headers.Authorization = `Bearer ${token}`;
    const abort = new AbortController();
    const timeout = setTimeout(() => abort.abort(), timeoutMs);
    try {
      const response = await this.fetchImpl(`${API}${path}`, {method, headers, cache: 'no-store',
        credentials: 'same-origin', signal: abort.signal, keepalive,
        ...(body === undefined ? {} : {body: JSON.stringify(body)})});
      const result = await response.json();
      if (!response.ok) {
        if (response.status === 401 && token && token === this.identity?.token) this.invalidateIdentity();
        throw new APIError(response.status, result?.error?.code || 'request_failed', result?.error?.message || '请求未完成，请稍后再试。');
      }
      return result;
    } finally { clearTimeout(timeout); }
  }
  async createIdentity(name) {
    if (!validName(name)) { this.inform('昵称请输入 1–32 个字符。', 'error'); return; }
    if (this.mutationBusy) return;
    this.mutationBusy = true; this.emit();
    try {
      const result = await this.request('/devices', 'POST', {name}, {token: null});
      this.voice?.interrupt(); this.audio?.disable(); this.identity = {device: result.device, token: result.token};
      this.active = true; this.authInvalid = false; this.epoch++; this.onIdentity(this.identity); this.emit();
      this.mutationBusy = false; await this.voice?.initialize(); await this.recommendation?.initialize(); await this.refresh();
      this.inform('准备好了。创建一个群，或用伙伴的口令加入。', 'success');
    } catch (error) { this.inform(errorText(error), 'error'); }
    finally { this.mutationBusy = false; this.emit(); }
  }
  applyState(next, epoch) {
    if (!this.allowed() || epoch !== this.epoch || !next || next.device?.id !== this.identity.device.id) return false;
    if (this.state && this.state.server_epoch === next.server_epoch && next.current_version < this.state.current_version) return false;
    const changed = this.state && (this.state.server_epoch !== next.server_epoch ||
      this.state.current_version !== next.current_version || this.state.current_group_id !== next.current_group_id);
    if (changed) {
      const restarted = this.state.server_epoch !== next.server_epoch;
      this.cancelFloor(); this.audio?.suspend(this.audio?.desired ? '群状态已变化，正在恢复音频。' : '');
      if (restarted) { this.notice = '连接已恢复，需要发言时请重新按住说话。'; this.noticeTone = 'success'; }
    }
    this.state = next; this.stateAt = this.now(); this.connected = true; this.recommendation?.observeState(); this.emit(); void this.audio?.sync(); return true;
  }
  async refresh() {
    if (!this.allowed() || this.polling || this.pendingSwitch || this.mutationBusy) return;
    const epoch = this.epoch; this.polling = true; this.emit();
    try {
      const next = await this.request('/state');
      if (this.applyState(next, epoch)) {
        if (this.lastRefreshError && this.noticeTone === 'error' && this.notice === this.lastRefreshError) this.inform('群组已更新。', 'success');
        this.lastRefreshError = '';
      }
    }
    catch (error) { if (epoch === this.epoch && this.active) { this.connected = false; this.cancelFloor(); this.audio?.suspend('控制连接未确认，音频已停止。'); this.lastRefreshError = errorText(error); this.inform(this.lastRefreshError, 'error'); } }
    finally { this.polling = false; this.emit(); }
  }
  async mutate(path, body) {
    if (!this.allowed() || this.mutationBusy || this.pendingSwitch || this.voice?.blocking || this.recommendation?.blocking) return;
    this.cancelFloor(); const epoch = ++this.epoch;
    this.mutationBusy = true; this.emit();
    try {
      const result = await this.request(path, 'POST', body);
      if (epoch !== this.epoch || !this.allowed()) return;
      this.applyState(result.state, epoch);
      if (result.code) this.share = {name: result.group.name, code: result.code};
      this.inform(path === '/groups' ? '群组已创建。口令只显示这一次，请保存并私下分享。' : '已加入群组。你可以从群列表手动进入。', 'success');
      return result;
    } catch (error) { if (epoch === this.epoch) this.inform(errorText(error), 'error'); }
    finally { this.mutationBusy = false; this.emit(); }
  }
  createGroup(name, purpose = '') {
    if (!validName(name)) { this.inform('群名请输入 1–32 个字符。', 'error'); return Promise.resolve(); }
    if (!validPurpose(purpose)) { this.inform('职责简介最多 256 个字符，不能包含控制字符。', 'error'); return Promise.resolve(); }
    const normalized = trimPurpose(purpose);
    return this.mutate('/groups', normalized ? {name, purpose: normalized} : {name});
  }
  joinGroup(code) { return this.mutate('/memberships', {code}); }
  async switchGroup(groupID) {
    if (!this.allowed() || this.uiBlocked || this.mutationBusy || this.pendingSwitch || this.recommendation?.blocking || !this.state) return false;
    this.cancelFloor(); this.audio?.suspend('正在确认切群结果。'); ++this.epoch;
    const pending = {operation_id: this.random(), group_id: groupID, expected_version: this.state.current_version};
    try { this.onPending(pending); }
    catch { this.inform('无法保存切群请求，尚未提交。请恢复浏览器本地存储后重试。', 'error'); void this.audio?.sync(); return false; }
    this.pendingSwitch = pending; this.inform('正在切换群组，完成前请稍候。');
    await this.confirmSwitch(true); return true;
  }
  async confirmRecommendation(recommendation, groupID) {
    if (!this.allowed() || !this.connected || this.uiBlocked || this.mutationBusy || this.pendingSwitch || !this.state) return false;
    const pending = {operation_id: this.random(), group_id: groupID, expected_version: recommendation.current_version,
      recommendation_id: recommendation.recommendation_id, server_epoch: recommendation.server_epoch};
    try { this.onPending(pending); }
    catch { this.inform('无法保存确认请求，尚未提交。请恢复本地存储后重试。', 'error'); return false; }
    this.pendingSwitch = pending;
    const target = this.recommendation?.mode === 'target';
    this.recommendation?.interrupt(target ? '目标确认请求已提交，正在等待后台结果。' : '确认请求已提交，正在等待后台结果。');
    this.cancelFloor(); this.audio?.suspend(target ? '正在确认语音目标切群结果。' : '正在确认推荐切群结果。');
    this.inform(target ? '正在确认并进入识别目标群，完成前请稍候。' : '正在确认并进入推荐群组，完成前请稍候。');
    await this.confirmSwitch(true); return true;
  }
  async confirmSwitch(first = false) {
    if (!this.allowed() || !this.pendingSwitch || this.confirming) return;
    this.cancel(this.retryTimer); this.retryTimer = null;
    const operation = {...this.pendingSwitch}; const epoch = this.epoch;
    this.confirming = true; this.emit();
    try {
      let result;
      if (!first) {
        try { result = await this.request(`/operations/${encodeURIComponent(operation.operation_id)}`); }
        catch (error) { if (!(error instanceof APIError && error.status === 404)) throw error; }
      }
      if (!result) result = await this.request(operation.recommendation_id ? '/recommendations/confirm' : '/current-group', 'PUT', operation);
      if (epoch !== this.epoch || !this.allowed()) return;
      if (!['applied', 'rejected'].includes(result.operation?.status) || result.operation.operation_id !== operation.operation_id ||
        result.operation.group_id !== operation.group_id || result.operation.expected_version !== operation.expected_version ||
        (operation.recommendation_id && (result.operation.recommendation_id !== operation.recommendation_id ||
          result.operation.recommendation_epoch !== operation.server_epoch)))
        throw new Error('operation_not_terminal');
      const state = result.state || await this.request('/state');
      if (epoch !== this.epoch || !this.allowed()) return;
      this.onPending(null); this.pendingSwitch = null; this.applyState(state, epoch);
      this.inform(result.operation.status === 'applied' ? '已切换群组，语音连接完成后就能说话。' :
        (messages[result.operation.error_code] || '切群被拒绝，已按服务端状态恢复。'), result.operation.status === 'applied' ? 'success' : 'error');
    } catch (error) {
      if (epoch === this.epoch && this.allowed()) {
        this.inform('切换还没完成，正在继续确认。请稍候，或点击“继续连接”。', 'error');
        this.retryTimer = this.schedule(() => { void this.confirmSwitch(); }, 1800);
      }
    } finally { this.confirming = false; this.emit(); }
  }
  serverNow() { return Date.parse(this.state?.server_time || '') + Math.max(0, this.now() - this.stateAt); }
  canPress() {
    return this.allowed() && !this.uiBlocked && this.connected && !this.voice?.blocking && !this.recommendation?.blocking && (!this.audio || this.audio.isReady()) && !!this.state?.current_group_id && !!this.state.server_epoch &&
      Number.isFinite(this.serverNow()) && !this.pendingSwitch && !this.mutationBusy && !this.acquiring && !this.releasing && !this.pressed;
  }
  stillPressed(generation, request) {
    return this.allowed() && !this.uiBlocked && this.pressed && generation === this.floorGeneration && !this.pendingSwitch &&
      (!this.audio || (this.audio.isReady() && request.peer_id === this.audio.peerID)) &&
      this.state?.server_epoch === request.server_epoch && this.state.current_group_id === request.group_id && this.state.current_version === request.current_version;
  }
  async release(request) {
    if (!request) return;
    this.releasing++; this.emit();
    const {valid_until, ...body} = request;
    try { await this.request('/floor/release', 'POST', body, {keepalive: true}); }
    catch { /* Local hold has already ended; the server lease also expires. */ }
    finally { this.releasing--; this.emit(); }
  }
  cancelFloor(message = '') {
    this.audio?.setSending(false);
    const request = this.floorRequest;
    this.pressed = false; this.floorPhase = 'idle'; this.floorRequest = null; this.lease = null;
    ++this.floorGeneration; ++this.epoch;
    this.cancel(this.renewTimer); this.cancel(this.expiryTimer); this.renewTimer = this.expiryTimer = null;
    if (message) { this.notice = message; this.noticeTone = 'info'; }
    this.emit();
    if (request) void this.release(request);
  }
  async pressFloor() {
    if (!this.canPress()) return;
    const request = {group_id: this.state.current_group_id, current_version: this.state.current_version,
      request_id: this.random(), server_epoch: this.state.server_epoch, ...(this.audio ? {peer_id: this.audio.peerID} : {}), valid_until: new Date(this.serverNow() + 5000).toISOString()};
    const generation = ++this.floorGeneration; ++this.epoch;
    this.floorRequest = request; this.pressed = true; this.floorPhase = 'acquiring'; this.acquiring = true; this.emit();
    try {
      const result = await this.request('/floor/acquire', 'POST', request);
      if (!this.stillPressed(generation, request)) { if (generation === this.floorGeneration) this.cancelFloor(); await this.release(request); return; }
      if (result.floor?.device_id !== this.identity.device.id || result.floor?.request_id !== request.request_id || !result.floor?.lease_id || (request.peer_id && result.floor.peer_id !== request.peer_id))
        throw new Error('invalid_floor');
      this.lease = result.floor; this.floorPhase = 'held';
      if (this.audio && !this.audio.setSending(true)) { this.cancelFloor('音频尚未就绪，请重新启用音频后再按下。'); return; }
      this.scheduleLease(generation, request); this.emit();
    } catch (error) {
      if (generation === this.floorGeneration) this.cancelFloor(errorText(error));
    } finally { this.acquiring = false; this.emit(); }
  }
  scheduleLease(generation, request) {
    this.cancel(this.renewTimer); this.cancel(this.expiryTimer);
    const remaining = Date.parse(this.lease?.expires_at || '') - this.serverNow();
    if (!Number.isFinite(remaining) || remaining <= 0) { this.cancelFloor('这次发言已结束，请重新按住说话。'); return; }
    this.expiryTimer = this.schedule(() => {
      if (this.stillPressed(generation, request)) this.cancelFloor('这次发言已结束，请重新按住说话。');
    }, remaining);
    this.renewTimer = this.schedule(() => { void this.renewFloor(generation, request); }, 1000);
  }
  async renewFloor(generation, request) {
    if (!this.stillPressed(generation, request) || !this.lease) return;
    const {valid_until, ...body} = request;
    const leaseID = this.lease.lease_id;
    try {
      const result = await this.request('/floor/renew', 'POST', {...body, lease_id: leaseID});
      if (!this.stillPressed(generation, request)) { if (generation === this.floorGeneration) this.cancelFloor(); await this.release(request); return; }
      if (result.floor?.lease_id !== leaseID || result.floor?.request_id !== request.request_id) throw new Error('invalid_renew');
      this.lease = result.floor; this.scheduleLease(generation, request); this.emit();
    } catch (error) { if (generation === this.floorGeneration) this.cancelFloor(errorText(error)); }
  }
  invalidateIdentity() {
    if (this.authInvalid) return;
    this.authInvalid = true; this.connected = false;
    this.suspend('本机身份已失效，请重新建立身份并重新入群。');
  }
  forgetIdentity() {
    this.suspend(''); this.identity = null; this.state = null; this.pendingSwitch = null; this.share = null;
    this.voice?.interrupt(); this.recommendation?.interrupt(); this.audio?.disable();
    this.authInvalid = false; this.active = true; this.connected = false; this.notice = ''; this.emit();
  }
  suspend(message) {
    this.active = false; this.cancel(this.retryTimer); this.retryTimer = null;
    this.voice?.interrupt(); this.recommendation?.interrupt('页面已暂停，请重新获取建议。'); this.cancelFloor(message); this.audio?.suspend(message); this.emit();
  }
  async activate() {
    this.cancelFloor(); this.audio?.suspend();
    if (this.restorePending && this.identity) this.pendingSwitch = this.restorePending(this.identity.device.id);
    this.active = true; ++this.epoch; this.state = null; this.connected = false; this.notice = ''; this.emit();
    await this.voice?.initialize();
    await this.recommendation?.initialize();
    if (this.pendingSwitch) await this.confirmSwitch(); else await this.refresh();
  }
}

// A visible speaking/ready state requires the actual media binding as well as
// control state. A stale floor snapshot alone never means audio is connected.
export function channelPresentation(client) {
  const audio = client.audio, state = client.state;
  const current = state?.groups?.find(group => group.id === state.current_group_id);
  const view = (phase, title, detail, hint = '按住说话，松开结束', button = '按住说话') => ({phase, title, detail, hint, button});
  if (!client.allowed()) return view('paused', '此页面已暂停', '返回正在使用的页面，或在这里继续。', '继续使用后再说话');
  if (client.pendingSwitch) return view('switching', '正在切换群组', '切换完成后将更新当前群，请稍候。', '正在切换，请稍候');
  if (client.voice?.blocking) return client.voice.current?.mode === 'need' ?
    view('voice', '正在语音找组', '语音会话结束后生成建议，确认前当前群保持不变。', '先完成或取消语音需求') :
    view('voice', client.voice.current ? '正在语音切群' : '正在准备', '语音操作结束后，会恢复群内对讲。', '结束语音操作后再说话');
  if (client.recommendation?.blocking) return client.recommendation.mode === 'target' ?
    view('reviewing_target', '请核对语音目标', '当前群保持不变，确认后才会切换。', '先确认或取消目标核对') :
    view('recommending', '正在选择协作群', '当前群保持不变。请先确认建议或取消。', '先完成或取消需求推荐');
  if (client.uiBlocked) return view('managing', '正在添加群组', '当前发言已停止。关闭面板后可返回对讲操作。', '先关闭添加群组面板');
  if (client.mutationBusy) return view('updating', '正在更新群组', '正在更新群组信息，请稍候。', '操作进行中，请稍候');
  if (!state) return !client.polling && client.noticeTone === 'error' ?
    view('error', '群组暂未加载', '请检查网络，然后点击刷新重试。', '获取群组后再连接语音') :
    view('loading', '正在获取群组', '获取完成后，会显示你已加入的群组。', '正在获取群组，请稍候');
  if (!current) return view('empty', '先加入一个群', '创建你的小组，或输入伙伴分享的口令。', '加入群组后即可连接');
  if (!client.connected) return view('error', '暂时无法连接', '检查网络后，点击刷新重试。', '连接恢复后再说话');
  if (!audio?.desired) return view('disconnected', '尚未连接语音', '连接后可以收听，也可以按住按钮向群里说话。', '先连接群语音');
  if (audio.phase === 'playback_blocked') return view('playback', '还差一步，就能收听', '点击“开始收听”，允许这个页面播放声音。', '先点击开始收听');
  if (audio.phase === 'error') return view('error', '语音连接没有完成', '请检查网络和麦克风权限，再试一次。', '重试连接后再说话');
  if (!audio.isReady()) return view('connecting', '正在连接语音', '连接完成后，就能收听并按住说话。', '正在连接，请稍候');
  if (client.floorPhase === 'held' && client.pressed) return view('speaking', '你正在说话', '声音正在发送给当前群。松开按钮就会结束。', '松开按钮，结束这次发言', '松开结束');
  if (client.floorPhase === 'acquiring' && client.pressed) return view('requesting', '正在准备发言', '请继续按住，看到“你正在说话”后再开口。', '请继续按住', '请继续按住');
  if (client.acquiring || client.releasing) return view('ending', '正在结束发言', '马上就好，结束后可再次按住说话。', '请稍候再按');
  const floor = current.floor;
  if (floor && floor.device_id !== client.identity?.device.id && Date.parse(floor.expires_at) > client.serverNow()) {
    return view('listening', `${floor.device_name || '伙伴'}正在发言`, audio.listeningMuted ? '收听目前已静音，可以取消静音听伙伴说话。' : '听伙伴说完，再按住按钮回应。', '伙伴正在发言，请稍候');
  }
  return view('ready', '可以说话了', audio.listeningMuted ? '收听已静音。按住按钮仍可向群里说话。' : '按住下方按钮，和当前群的伙伴说句话。');
}

function boot() {
  const $ = id => document.getElementById(id);
  const tab = randomID(); let identityChanged = false;
  const read = key => { try { return JSON.parse(localStorage.getItem(key)); } catch { return null; } };
  let client, addPanelOpen = false, addMode = 'join', addReturnFocus = null, formMessage = '', copiedCode = '';
  const owns = () => {
    try { return !identityChanged && localStorage.getItem(ownerKey(client.identity.device.id)) === tab && read(IDENTITY_KEY)?.token === client.identity.token; }
    catch { return false; }
  };
  const text = (id, value) => { if ($(id).textContent !== String(value)) $(id).textContent = value; };
  const element = (tag, className, value) => { const node = document.createElement(tag); node.className = className; if (value !== undefined) node.textContent = value; return node; };
  const canManage = () => client.allowed() && !client.mutationBusy && !client.pendingSwitch && !client.voice?.blocking && !client.recommendation?.blocking;
  function renderSuggestions(id, recommendation, target = false) {
    const list = $(id); list.replaceChildren();
    for (const item of recommendation?.result?.suggestions || []) {
      const row = element('li', 'recommendation-row');
      row.append(element('strong', '', target ? `识别目标群：${item.name}` : item.name));
      if (item.purpose) row.append(element('p', 'group-purpose', `群职责（填写内容）：${item.purpose}`));
      row.append(element('p', 'recommendation-reason', target ? item.reason : `推荐说明：${item.reason}`));
      const button = element('button', 'button secondary', `${target ? '确认切换到' : '确认进入'} ${item.name}`);
      button.type = 'button'; button.disabled = !client.allowed() || !!client.pendingSwitch || !client.connected;
      button.addEventListener('click', () => { void recommendation.confirm(item.group_id); });
      row.append(button); list.append(row);
    }
  }
  function openAddPanel(mode = 'join', trigger = $('add-group-open')) {
    if (!canManage()) return false;
    addPanelOpen = true; addMode = mode; addReturnFocus = trigger; formMessage = '';
    client.setUIBlocked(true); render();
    $(mode === 'create' ? 'create-name' : 'join-code').focus();
    return true;
  }
  function closeAddPanel() {
    if (!addPanelOpen) return;
    addPanelOpen = false; formMessage = ''; client.setUIBlocked(false); render();
    const emptyTrigger = addReturnFocus === $('empty-create') || addReturnFocus === $('empty-join');
    const target = emptyTrigger && client.state?.groups?.length ? $('add-group-open') : addReturnFocus || $('add-group-open');
    (target.disabled ? $('current-name') : target).focus();
  }
  function render() {
    const identity = client.identity;
    $('welcome').hidden = !!identity; $('workspace').hidden = !identity;
    $('identity-chip').hidden = !identity;
    if (identity) text('identity-name', identity.device.name);
    $('lock-panel').hidden = !identity || client.active;
    text('lock-title', client.authInvalid ? '本机身份需要重新建立' : '此标签页已暂停操作');
    text('lock-description', client.authInvalid ? '服务不再识别此浏览器保存的身份。重新建立本机身份后，需要重新加入原来的群。' : identityChanged ? '本浏览器保存的身份已被其他标签页更新。重新载入后才能继续。' : '同一身份一次只在一个标签页操作，避免抢麦与切群互相干扰。');
    text('takeover', client.authInvalid ? '重新建立本机身份' : identityChanged ? '重新载入此页面' : '改用当前标签页');
    const operable = canManage();
    for (const id of ['create-name', 'create-purpose', 'join-code', 'create-group', 'join-group']) $(id).disabled = !operable;
    for (const id of ['add-group-open', 'empty-create', 'empty-join', 'create-mode', 'join-mode']) $(id).disabled = !operable;
    $('add-group-panel').hidden = !addPanelOpen;
    $('add-group-open').setAttribute('aria-expanded', String(addPanelOpen));
    $('create-form').hidden = addMode !== 'create'; $('join-form').hidden = addMode !== 'join';
    $('create-mode').setAttribute('aria-pressed', String(addMode === 'create'));
    $('join-mode').setAttribute('aria-pressed', String(addMode === 'join'));
    $('add-group-status').hidden = !formMessage; text('add-group-status', formMessage);
    $('identity-submit').disabled = client.mutationBusy;
    $('refresh-state').disabled = !client.allowed() || !!client.pendingSwitch || client.mutationBusy;
    $('retry-switch').hidden = !client.pendingSwitch;
    $('retry-switch').disabled = client.confirming || !client.active;
    $('notice').hidden = !client.notice; text('notice', client.notice); $('notice').dataset.tone = client.noticeTone;
    $('share-card').hidden = !client.share;
    if (client.share) {
      text('share-name', client.share.name); text('share-code', client.share.code);
      text('copy-code', copiedCode === client.share.code ? '已复制' : '复制口令');
    }
    const state = client.state;
    const groups = state?.groups || [];
    const current = groups.find(group => group.id === state?.current_group_id);
    const loadFailed = !state && !client.polling && client.noticeTone === 'error';
    const visibleGroups = filterGroups(groups, $('group-filter').value);
    text('group-count', state ? `${groups.length} 个` : '—');
    $('group-filter').disabled = groups.length === 0;
    text('group-filter-status', !state ? '群组列表尚未加载' : groups.length ? `显示 ${visibleGroups.length} / ${groups.length} 个已加入群` : '仅筛选已加入群的列表');
    const list = $('group-list'); list.replaceChildren();
    for (const group of visibleGroups) {
      const row = element('li', `group-row${group.id === current?.id ? ' selected' : ''}`);
      const labels = element('div', 'group-labels'); labels.append(element('strong', '', group.name), element('span', '', `已加入成员 ${group.member_count} 人`));
      if (groupPurpose(group)) labels.append(element('span', 'group-purpose', group.purpose));
      const button = element('button', 'group-enter', group.id === current?.id ? '当前群' : '进入');
      button.type = 'button'; button.disabled = !operable || addPanelOpen || group.id === current?.id;
      button.addEventListener('click', () => { void client.switchGroup(group.id); });
      row.append(labels, button); list.append(row);
    }
    $('empty-groups').hidden = !state || groups.length > 0;
    $('empty-filter').hidden = groups.length === 0 || visibleGroups.length > 0;
    $('empty-actions').hidden = !state || groups.length > 0;
    text('current-name', !state ? (loadFailed ? '群组暂未加载' : '正在获取群组') : current?.name || '你的第一个群组');
    text('current-detail', !state ? (loadFailed ? '检查网络后可刷新重试' : '正在获取已加入的群组，请稍候') : client.pendingSwitch ? '正在切换，语音会稍作暂停' : current ? `已加入 ${current.member_count} 人` : '让要一起协作的伙伴，来到同一个频道');
    $('current-purpose').hidden = !groupPurpose(current || {}); text('current-purpose', groupPurpose(current || {}));
    text('sync-label', !client.active ? '此页面已暂停' : !state ? '正在获取群组' : !client.connected ? '连接待恢复' : '群组信息已更新');
    const presentation = channelPresentation(client);
    $('channel-panel').dataset.phase = presentation.phase;
    text('floor-label', presentation.title); text('floor-description', presentation.detail);
    text('ptt-title', presentation.button); text('ptt-hint', presentation.hint);
    $('ptt').dataset.phase = presentation.phase;
    $('ptt').disabled = !client.pressed && !client.canPress();
    $('ptt').setAttribute('aria-pressed', String(client.pressed));
    const audio = client.audio;
    const mediaReady = !!audio?.isReady();
    $('audio-status').dataset.phase = mediaReady ? 'ready' : audio?.phase || 'disabled';
    text('audio-status', mediaReady ? (audio.listeningMuted ? '已连接 · 收听静音' : '语音已连接') :
      ({disabled:'语音未连接',waiting:'等待连接',connecting:'正在连接',ready:'正在恢复连接',playback_blocked:'需要允许播放',error:'连接未完成'})[audio?.phase] || '语音未连接');
    text('audio-message', audio?.message || '建议戴耳机，避免同机多端播放造成回声。');
    $('audio-enable').hidden = !!audio?.desired && audio?.phase !== 'error';
    text('audio-enable', audio?.phase === 'error' ? '重新连接' : '连接群语音');
    $('audio-enable').disabled = !operable || addPanelOpen || !current || !client.connected;
    $('audio-disable').hidden = !audio?.desired;
    $('audio-play').hidden = audio?.phase !== 'playback_blocked';
    $('listen-muted').disabled = !audio?.desired; $('listen-muted').checked = !!audio?.listeningMuted;
    const voice=client.voice;
    $('voice-start').disabled=!operable || addPanelOpen || !current || !client.connected || !voice?.available;
    $('voice-cancel').hidden=!voice?.current; $('voice-cancel').disabled=!voice?.canCancel;
    text('voice-cancel', voice?.canCancel ? (voice.current.mode === 'need' ? '取消语音需求' : '取消语音切群') : '正在返回群组');
    $('voice-check').hidden=voice?.phase!=='checking' || !!voice?.initializing;
    const recommendation = client.recommendation;
    const targetReview = recommendation?.mode === 'target';
    text('voice-status', !voice?.current && targetReview ? recommendation.message : voice?.message || '正在检查语音切群。');
    $('voice-proposal-panel').hidden = !targetReview || !recommendation?.blocking;
    text('voice-proposal-status', targetReview ? recommendation.message : '识别后请核对目标群，确认后才会切换。');
    text('voice-proposal-source', targetReview && recommendation.result ?
      (recommendation.result.source === 'exact' ? '群名匹配结果' : '模型匹配结果，请核对群名') : '');
    $('voice-proposal-cancel').hidden = !targetReview || !recommendation?.blocking;
    renderSuggestions('voice-proposal-results', targetReview ? recommendation : null, true);
    $('recommendation-need').disabled = !recommendation?.canRequest();
    $('recommendation-request').disabled = !recommendation?.canRequest();
    $('recommendation-voice').disabled = !recommendation?.canRequest() || !voice?.available || !!voice?.blocking || !current;
    $('recommendation-cancel').hidden = targetReview || !recommendation?.blocking;
    text('recommendation-status', !targetReview ? recommendation?.message || '正在检查需求推荐。' :
      recommendation.available ? '描述需要的帮助，只向已加入的群推荐。' : '需求推荐暂不可用，可从群列表选择。');
    renderSuggestions('recommendation-results', targetReview ? null : recommendation);
  }
  client = new ControlClient({authorize: owns, onChange: render, restorePending: id => read(pendingKey(id)),
    onIdentity(identity) { localStorage.setItem(IDENTITY_KEY, JSON.stringify(identity)); claimOwner(localStorage, identity.device.id, tab, true); },
    onPending(pending) { if (!client.identity || !owns()) throw new Error('identity ownership changed'); const key = pendingKey(client.identity.device.id); if (pending) localStorage.setItem(key, JSON.stringify(pending)); else localStorage.removeItem(key); },
  });
  client.audio = new AudioClient({request: (...args) => client.request(...args),
    scopeRequest: () => { const token = client.identity?.token; return (path, method, body, options) => client.request(path, method, body, {...options, token}); }, serverNow: () => client.serverNow(),
    getContext: () => !document.hidden && client.allowed() && client.connected && !client.voice?.blocking && !client.pendingSwitch && client.state?.current_group_id ?
      {device_id: client.identity.device.id, group_id: client.state.current_group_id, current_version: client.state.current_version, server_epoch: client.state.server_epoch} : null,
    output: $('remote-audio'), onChange: render, onFailure: message => client.cancelFloor(message),
  });
  client.voice=new VoiceCommandClient({control:client,audio:client.audio,request:(...args)=>client.request(...args),
    scopeRequest:()=>{const token=client.identity?.token;return(path,method,body,options)=>client.request(path,method,body,{...options,token});},onChange:render});
  client.recommendation = new RecommendationClient({control: client, onChange: render});
  $('recommendation-form').addEventListener('submit', event => { event.preventDefault(); void client.recommendation.request($('recommendation-need').value); });
  $('recommendation-voice').addEventListener('click', () => { void client.voice.start({mode: 'need'}); });
  $('recommendation-cancel').addEventListener('click', () => { if (!client.pendingSwitch && client.recommendation.blocking) client.recommendation.interrupt('已取消建议，当前群保持不变。'); });
  $('voice-proposal-cancel').addEventListener('click', () => { if (!client.pendingSwitch && client.recommendation.mode === 'target' && client.recommendation.blocking) client.recommendation.interrupt('已取消目标核对，当前群保持不变。'); });
  $('voice-start').addEventListener('click',()=>{if (!client.uiBlocked) void client.voice.start();});
  $('voice-cancel').addEventListener('click',()=>client.voice.interrupt());
  $('voice-check').addEventListener('click',()=>{void client.voice.initialize();});
  $('audio-enable').addEventListener('click', () => { if (!client.uiBlocked) void client.audio.enable(); });
  bindAudioDisable($('audio-disable'), client);
  $('audio-play').addEventListener('click', () => { void client.audio.retryPlayback(); });
  $('listen-muted').addEventListener('change', () => client.audio.setListeningMuted($('listen-muted').checked));
  for (const [id, label] of [['nickname', '昵称'], ['create-name', '群名']]) {
    $(id).addEventListener('input', () => $(id).setCustomValidity(validName($(id).value) ? '' : `${label}请输入 1–32 个字符。`));
  }
  $('create-purpose').addEventListener('input', () => $('create-purpose').setCustomValidity(validPurpose($('create-purpose').value) ? '' : '职责简介最多 256 个字符，不能包含控制字符。'));
  $('group-filter').addEventListener('input', render);
  $('add-group-open').addEventListener('click', () => openAddPanel('join', $('add-group-open')));
  $('empty-create').addEventListener('click', () => openAddPanel('create', $('empty-create')));
  $('empty-join').addEventListener('click', () => openAddPanel('join', $('empty-join')));
  $('add-group-close').addEventListener('click', closeAddPanel);
  for (const mode of ['create', 'join']) $(mode + '-mode').addEventListener('click', () => {
    if (!addPanelOpen || !canManage()) return;
    addMode = mode; formMessage = ''; render(); $(mode === 'create' ? 'create-name' : 'join-code').focus();
  });
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape' && addPanelOpen) { event.preventDefault(); closeAddPanel(); }
  });
  $('identity-form').addEventListener('submit', event => { event.preventDefault(); void client.createIdentity($('nickname').value.trim()); });
  $('create-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (!addPanelOpen || addMode !== 'create' || !canManage()) return;
    formMessage = '';
    if (await client.createGroup($('create-name').value.trim(), $('create-purpose').value)) {
      $('create-name').value = ''; $('create-purpose').value = ''; $('create-purpose').setCustomValidity('');
      closeAddPanel();
    } else { formMessage = client.notice || '没有创建成功，请检查填写内容后重试。'; render(); }
  });
  $('join-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (!addPanelOpen || addMode !== 'join' || !canManage()) return;
    formMessage = '';
    if (await client.joinGroup($('join-code').value.trim())) { $('join-code').value = ''; closeAddPanel(); }
    else { formMessage = client.notice || '没有加入成功，请检查口令后重试。'; render(); }
  });
  $('retry-switch').addEventListener('click', () => { void client.confirmSwitch(); });
  $('refresh-state').addEventListener('click', () => { void client.refresh(); });
  $('copy-code').addEventListener('click', async () => {
    const code = client.share?.code; if (!code) return;
    try { await navigator.clipboard.writeText(code); copiedCode = code; client.inform('口令已复制，请只分享给要加入的伙伴。', 'success'); }
    catch { client.inform('暂时无法自动复制，请选中口令手动复制。', 'error'); }
  });
  $('takeover').addEventListener('click', () => {
    if (client.authInvalid) {
      const id = client.identity.device.id;
      localStorage.removeItem(IDENTITY_KEY); localStorage.removeItem(pendingKey(id));
      if (localStorage.getItem(ownerKey(id)) === tab) localStorage.removeItem(ownerKey(id));
      client.forgetIdentity(); $('nickname').focus(); return;
    }
    if (identityChanged) { location.reload(); return; }
    if (claimOwner(localStorage, client.identity.device.id, tab, true)) void client.activate();
  });
  const endPress = bindPTTPointer($('ptt'), client);
  $('ptt').addEventListener('keydown', event => { if ([' ', 'Enter'].includes(event.key)) { event.preventDefault(); if (!event.repeat) void client.pressFloor(); } });
  $('ptt').addEventListener('keyup', event => { if ([' ', 'Enter'].includes(event.key)) { event.preventDefault(); endPress(); } });
  $('ptt').addEventListener('blur', endPress);
  window.addEventListener('blur', endPress);
  window.addEventListener('offline', () => { client.voice.interrupt('网络已断开，语音切群已停止。'); client.recommendation.interrupt('网络已断开，请重新获取建议。'); client.connected = false; client.cancelFloor('网络已断开，发言已经停止。'); client.audio.suspend('网络已断开。'); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) { client.voice.interrupt('页面隐藏，语音指令已停止。'); client.recommendation.interrupt('页面已隐藏，请重新获取建议。'); endPress(); client.audio.suspend('页面隐藏，音频已停止。'); } else if (client.allowed()) void client.refresh(); });
  window.addEventListener('online', () => { if (client.allowed()) void client.refresh(); });
  window.addEventListener('pagehide', () => { client.voice.interrupt('页面已离开，语音指令已停止。'); client.recommendation.interrupt('页面已离开，请重新获取建议。'); endPress(); client.audio.suspend(); if (client.identity && owns()) localStorage.removeItem(ownerKey(client.identity.device.id)); });
  window.addEventListener('pageshow', () => { if (client.identity && !identityChanged && claimOwner(localStorage, client.identity.device.id, tab)) void client.activate(); });
  window.addEventListener('storage', event => {
    if (!client.identity) { if (event.key === IDENTITY_KEY) location.reload(); return; }
    if (event.key === IDENTITY_KEY) { identityChanged = true; client.suspend('身份已在其他标签页更新，此页面已暂停。'); }
    else if (event.key === pendingKey(client.identity.device.id)) {
      client.pendingSwitch = read(pendingKey(client.identity.device.id));
      if (client.active) client.suspend('另一页面更新了群组切换，请在这里继续确认。');
    }
    else if (event.key === ownerKey(client.identity.device.id) && event.newValue !== tab) client.suspend('已在另一页面继续使用，这里已停止发言。');
  });
  setInterval(() => {
    if (client.identity && client.active && !owns()) client.suspend('此身份已由另一标签页使用。');
    if (client.allowed()) void client.refresh();
  }, 1200);
  try {
    const identity = read(IDENTITY_KEY);
    if (identity?.token && identity?.device?.id && typeof identity.device.name === 'string') {
      const pending = read(pendingKey(identity.device.id));
      client.load(identity, pending);
      if (claimOwner(localStorage, identity.device.id, tab)) void client.activate();
      else client.suspend('同一身份正在另一标签页使用。');
    }
  } catch { client.inform('浏览器存储不可用；请允许本地存储后重新载入。', 'error'); }
  render();
}
if (typeof document !== 'undefined' && document.getElementById('identity-form')) boot();
