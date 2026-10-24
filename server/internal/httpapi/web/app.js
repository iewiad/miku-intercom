import {AudioClient} from './audio.js';
import {VoiceCommandClient} from './voice.js';
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
  floor_busy: '群内麦权正被占用，松手后可再次申请。',
  join_rate_limited: '口令尝试次数较多，请稍后再试。',
  request_ended: '本次麦权申请已结束，请重新按下。',
  request_capacity: '麦权申请较频繁，请稍后重新按下。',
  lease_not_held: '麦权授权已结束，请重新按下。',
  operation_capacity: '此身份的切群记录已达开发版上限，暂时无法发起新操作。',
  version_conflict: '当前群版本已变化，已刷新状态，请重新操作。',
  epoch_conflict: '服务已重新启动，请刷新后重新按下。',
  request_expired: '这次申请已过期，请松手后重新按下。',
  request_cancelled: '这次申请已取消，请重新按下。',
  not_member: '尚未加入这个群，无法切换。',
  invalid_code: '口令不正确，请检查后再试。',
  rate_limited: '尝试次数较多，请稍后再试。',
};
export const errorText = error => error.status === 401 ? '本机身份已失效，请重新建立身份并重新入群。' : messages[error.code] || (error instanceof APIError ? error.message : '网络请求未确认，请检查本机服务后重试。');

export class ControlClient {
  constructor({fetchImpl = globalThis.fetch.bind(globalThis), now = Date.now, random = randomID,
    schedule = (callback, delay) => globalThis.setTimeout(callback, delay), cancel = id => globalThis.clearTimeout(id), authorize = () => true,
    onChange = () => {}, onIdentity = () => {}, onPending = () => {}, restorePending = null, audio = null} = {}) {
    Object.assign(this, {fetchImpl, now, random, schedule, cancel, authorize, onChange, onIdentity, onPending, restorePending, audio});
    this.identity = null; this.state = null; this.stateAt = 0; this.epoch = 0;
    this.active = true; this.authInvalid = false; this.connected = false; this.mutationBusy = false; this.polling = false;
    this.pendingSwitch = null; this.confirming = false; this.retryTimer = null;
    this.floorGeneration = 0; this.pressed = false; this.floorPhase = 'idle';
    this.floorRequest = null; this.lease = null; this.acquiring = false; this.releasing = 0;
    this.renewTimer = null; this.expiryTimer = null; this.share = null;
    this.notice = ''; this.noticeTone = 'info';
  }
  emit() { this.onChange(this); }
  inform(text, tone = 'info') { this.notice = text; this.noticeTone = tone; this.emit(); }
  allowed() { return !!this.identity && this.active && this.authorize(); }
  load(identity, pending = null) { if (this.identity?.token !== identity?.token) { this.cancelFloor(); this.audio?.suspend(); } this.identity = identity; this.pendingSwitch = pending; this.emit(); }
  async request(path, method = 'GET', body, {token = this.identity?.token, keepalive = false} = {}) {
    const headers = {'Accept': 'application/json'};
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    if (token) headers.Authorization = `Bearer ${token}`;
    const abort = new AbortController();
    const timeout = setTimeout(() => abort.abort(), 8000);
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
      this.mutationBusy = false; await this.voice?.initialize(); await this.refresh();
      this.inform('身份已建立。创建一个群，或用伙伴的口令加入。', 'success');
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
      this.cancelFloor(); this.audio?.suspend('群状态已变化，正在恢复音频。');
      if (restarted) this.notice = '服务已重新启动，麦权已清空；需要发言时请重新按下。';
    }
    this.state = next; this.stateAt = this.now(); this.connected = true; this.emit(); void this.audio?.sync(); return true;
  }
  async refresh() {
    if (!this.allowed() || this.polling || this.pendingSwitch || this.mutationBusy) return;
    const epoch = this.epoch; this.polling = true;
    try { const next = await this.request('/state'); this.applyState(next, epoch); }
    catch (error) { if (epoch === this.epoch && this.active) { this.connected = false; this.cancelFloor(); this.audio?.suspend('控制连接未确认，音频已停止。'); this.inform(errorText(error), 'error'); } }
    finally { this.polling = false; this.emit(); }
  }
  async mutate(path, body) {
    if (!this.allowed() || this.mutationBusy || this.pendingSwitch || this.voice?.blocking) return;
    this.cancelFloor(); const epoch = ++this.epoch;
    this.mutationBusy = true; this.emit();
    try {
      const result = await this.request(path, 'POST', body);
      if (epoch !== this.epoch || !this.allowed()) return;
      this.applyState(result.state, epoch);
      if (result.code) this.share = {name: result.group.name, code: result.code};
      this.inform(path === '/groups' ? '群已创建。请妥善保存口令，服务端不会再次展示。' : '已加入，第二个群不会自动打断当前群。', 'success');
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
    if (!this.allowed() || this.mutationBusy || this.pendingSwitch || !this.state) return false;
    this.cancelFloor(); this.audio?.suspend('正在确认切群结果。'); ++this.epoch;
    const pending = {operation_id: this.random(), group_id: groupID, expected_version: this.state.current_version};
    try { this.onPending(pending); }
    catch { this.inform('无法保存切群请求，尚未提交。请恢复浏览器本地存储后重试。', 'error'); void this.audio?.sync(); return false; }
    this.pendingSwitch = pending; this.inform('正在确认切群结果；确认前暂停麦权操作。');
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
      if (!result) result = await this.request('/current-group', 'PUT', operation);
      if (epoch !== this.epoch || !this.allowed()) return;
      if (!['applied', 'rejected'].includes(result.operation?.status) || result.operation.operation_id !== operation.operation_id ||
        result.operation.group_id !== operation.group_id || result.operation.expected_version !== operation.expected_version)
        throw new Error('operation_not_terminal');
      const state = result.state || await this.request('/state');
      if (epoch !== this.epoch || !this.allowed()) return;
      this.onPending(null); this.pendingSwitch = null; this.applyState(state, epoch);
      this.inform(result.operation.status === 'applied' ? '当前群已确认，请查看音频连接状态后再按住说话。' :
        (messages[result.operation.error_code] || '切群被拒绝，已按服务端状态恢复。'), result.operation.status === 'applied' ? 'success' : 'error');
    } catch (error) {
      if (epoch === this.epoch && this.allowed()) {
        this.inform('切群结果尚未确认。将查询同一操作；未知时仍用原请求重试，不推断当前群。', 'error');
        this.retryTimer = this.schedule(() => { void this.confirmSwitch(); }, 1800);
      }
    } finally { this.confirming = false; this.emit(); }
  }
  serverNow() { return Date.parse(this.state?.server_time || '') + Math.max(0, this.now() - this.stateAt); }
  canPress() {
    return this.allowed() && this.connected && !this.voice?.blocking && (!this.audio || this.audio.isReady()) && !!this.state?.current_group_id && !!this.state.server_epoch &&
      Number.isFinite(this.serverNow()) && !this.pendingSwitch && !this.mutationBusy && !this.acquiring && !this.releasing && !this.pressed;
  }
  stillPressed(generation, request) {
    return this.allowed() && this.pressed && generation === this.floorGeneration && !this.pendingSwitch &&
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
    if (!Number.isFinite(remaining) || remaining <= 0) { this.cancelFloor('麦权授权已到期，请重新按下。'); return; }
    this.expiryTimer = this.schedule(() => {
      if (this.stillPressed(generation, request)) this.cancelFloor('麦权授权已到期，请重新按下。');
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
    this.voice?.interrupt(); this.audio?.disable();
    this.authInvalid = false; this.active = true; this.connected = false; this.notice = ''; this.emit();
  }
  suspend(message) {
    this.active = false; this.cancel(this.retryTimer); this.retryTimer = null;
    this.voice?.interrupt(); this.cancelFloor(message); this.audio?.suspend(message); this.emit();
  }
  async activate() {
    this.cancelFloor(); this.audio?.suspend();
    if (this.restorePending && this.identity) this.pendingSwitch = this.restorePending(this.identity.device.id);
    this.active = true; ++this.epoch; this.state = null; this.connected = false; this.notice = ''; this.emit();
    await this.voice?.initialize();
    if (this.pendingSwitch) await this.confirmSwitch(); else await this.refresh();
  }
}

function boot() {
  const $ = id => document.getElementById(id);
  const tab = randomID(); let identityChanged = false;
  const read = key => { try { return JSON.parse(localStorage.getItem(key)); } catch { return null; } };
  let client;
  const owns = () => {
    try { return !identityChanged && localStorage.getItem(ownerKey(client.identity.device.id)) === tab && read(IDENTITY_KEY)?.token === client.identity.token; }
    catch { return false; }
  };
  const text = (id, value) => { if ($(id).textContent !== String(value)) $(id).textContent = value; };
  const element = (tag, className, value) => { const node = document.createElement(tag); node.className = className; if (value !== undefined) node.textContent = value; return node; };
  function render() {
    const identity = client.identity;
    $('welcome').hidden = !!identity; $('workspace').hidden = !identity;
    $('identity-chip').hidden = !identity;
    if (identity) text('identity-name', identity.device.name);
    $('lock-panel').hidden = !identity || client.active;
    text('lock-title', client.authInvalid ? '本机身份需要重新建立' : '此标签页已暂停操作');
    text('lock-description', client.authInvalid ? '服务不再识别此浏览器保存的身份。重新建立本机身份后，需要重新加入原来的群。' : identityChanged ? '本浏览器保存的身份已被其他标签页更新。重新载入后才能继续。' : '同一身份一次只在一个标签页操作，避免抢麦与切群互相干扰。');
    text('takeover', client.authInvalid ? '重新建立本机身份' : identityChanged ? '重新载入此页面' : '改用当前标签页');
    const operable = client.allowed() && !client.mutationBusy && !client.pendingSwitch && !client.voice?.blocking;
    for (const id of ['create-name', 'create-purpose', 'join-code', 'create-group', 'join-group']) $(id).disabled = !operable;
    $('identity-submit').disabled = client.mutationBusy;
    $('refresh-state').disabled = !client.allowed() || !!client.pendingSwitch || client.mutationBusy;
    $('retry-switch').hidden = !client.pendingSwitch;
    $('retry-switch').disabled = client.confirming || !client.active;
    $('notice').hidden = !client.notice; text('notice', client.notice); $('notice').dataset.tone = client.noticeTone;
    $('share-card').hidden = !client.share;
    if (client.share) { text('share-name', client.share.name); text('share-code', client.share.code); }
    const state = client.state;
    const groups = state?.groups || [];
    const current = groups.find(group => group.id === state?.current_group_id);
    const visibleGroups = filterGroups(groups, $('group-filter').value);
    text('group-count', `${groups.length} 个`);
    $('group-filter').disabled = groups.length === 0;
    text('group-filter-status', groups.length ? `显示 ${visibleGroups.length} / ${groups.length} 个已加入群` : '仅筛选已加入群的列表');
    const list = $('group-list'); list.replaceChildren();
    for (const group of visibleGroups) {
      const row = element('li', `group-row${group.id === current?.id ? ' selected' : ''}`);
      const labels = element('div', 'group-labels'); labels.append(element('strong', '', group.name), element('span', '', `已加入成员 ${group.member_count} 人`));
      if (groupPurpose(group)) labels.append(element('span', 'group-purpose', group.purpose));
      const button = element('button', 'group-enter', group.id === current?.id ? '当前群' : '进入');
      button.type = 'button'; button.disabled = !operable || group.id === current?.id;
      button.addEventListener('click', () => { void client.switchGroup(group.id); });
      row.append(labels, button); list.append(row);
    }
    $('empty-groups').hidden = groups.length > 0;
    $('empty-filter').hidden = groups.length === 0 || visibleGroups.length > 0;
    text('current-name', client.pendingSwitch ? '正在确认切群结果' : current?.name || (identity ? '还没有当前群' : '先建立你的身份'));
    text('current-detail', client.pendingSwitch ? '操作终态确认前，麦权按钮暂停使用' : current ? `已加入成员 ${current.member_count} 人 · 可手动切换当前群` : '创建群或输入口令，然后启用音频');
    text('sync-label', !identity ? '本机演示' : !client.active ? '此标签页已暂停' : !state ? '等待状态同步' : !client.connected ? '连接未确认' : '服务端状态已同步');
    text('floor-label', client.floorPhase === 'held' ? '你正在讲话' : client.floorPhase === 'acquiring' ? '正在申请麦权' : client.releasing || client.acquiring ? '正在结束上次申请' : '未按麦');
    text('floor-description', client.floorPhase === 'held' ? '麦克风正在向当前群发送声音，松手立即停发' : current?.floor && current.floor.device_id !== identity?.device.id ? `${current.floor.device_name} 持有麦权` : '松手、失焦或离开页面都会结束申请与持有');
    text('ptt-title', client.floorPhase === 'held' ? '你正在讲话' : client.floorPhase === 'acquiring' ? '申请中…' : '按住说话');
    $('ptt').dataset.phase = client.floorPhase;
    $('ptt').disabled = !client.pressed && !client.canPress();
    $('ptt').setAttribute('aria-pressed', String(client.pressed));
    const audio = client.audio;
    text('audio-status', ({disabled:'未启用音频',waiting:'等待群状态',connecting:'音频连接中',ready:audio?.listeningMuted ? '收听已静音' : '可收听',playback_blocked:'播放受阻',error:'音频连接失败'})[audio?.phase] || '未启用音频');
    text('audio-message', audio?.message || '建议戴耳机，避免同机多端播放造成回声。');
    $('audio-enable').hidden = !!audio?.desired && audio?.phase !== 'error';
    text('audio-enable', audio?.phase === 'error' ? '重试音频' : '启用音频');
    $('audio-enable').disabled = !operable || !current || !client.connected;
    $('audio-disable').hidden = !audio?.desired;
    $('audio-play').hidden = audio?.phase !== 'playback_blocked';
    $('listen-muted').disabled = !audio?.desired; $('listen-muted').checked = !!audio?.listeningMuted;
    const voice=client.voice;
    $('voice-start').disabled=!operable || !current || !client.connected || !voice?.available;
    $('voice-cancel').hidden=!voice?.current; $('voice-cancel').disabled=!voice?.canCancel;
    $('voice-check').hidden=voice?.phase!=='checking' || !!voice?.initializing;
    text('voice-status',voice?.message || '正在检查语音切群。');

    text('ptt-hint', client.pendingSwitch ? '等待切群终态' : !current ? '入群后可用' : !client.audio?.isReady() ? '先启用并连接音频' : client.floorPhase === 'held' ? '松手即释放' : '支持触屏、鼠标与空格键');
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
  $('voice-start').addEventListener('click',()=>{void client.voice.start();});
  $('voice-cancel').addEventListener('click',()=>client.voice.interrupt());
  $('voice-check').addEventListener('click',()=>{void client.voice.initialize();});
  $('audio-enable').addEventListener('click', () => { void client.audio.enable(); });
  bindAudioDisable($('audio-disable'), client);
  $('audio-play').addEventListener('click', () => { void client.audio.retryPlayback(); });
  $('listen-muted').addEventListener('change', () => client.audio.setListeningMuted($('listen-muted').checked));
  for (const [id, label] of [['nickname', '昵称'], ['create-name', '群名']]) {
    $(id).addEventListener('input', () => $(id).setCustomValidity(validName($(id).value) ? '' : `${label}请输入 1–32 个字符。`));
  }
  $('create-purpose').addEventListener('input', () => $('create-purpose').setCustomValidity(validPurpose($('create-purpose').value) ? '' : '职责简介最多 256 个字符，不能包含控制字符。'));
  $('group-filter').addEventListener('input', render);
  $('identity-form').addEventListener('submit', event => { event.preventDefault(); void client.createIdentity($('nickname').value.trim()); });
  $('create-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (await client.createGroup($('create-name').value.trim(), $('create-purpose').value)) {
      $('create-name').value = ''; $('create-purpose').value = ''; $('create-purpose').setCustomValidity('');
    }
  });
  $('join-form').addEventListener('submit', async event => { event.preventDefault(); if (await client.joinGroup($('join-code').value.trim())) $('join-code').value = ''; });
  $('retry-switch').addEventListener('click', () => { void client.confirmSwitch(); });
  $('refresh-state').addEventListener('click', () => { void client.refresh(); });
  $('copy-code').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(client.share.code); client.inform('口令已复制，请只分享给要加入的伙伴。', 'success'); }
    catch { client.inform('浏览器无法自动复制，请选择上面的口令手动复制。'); }
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
  window.addEventListener('offline', () => { client.voice.interrupt('浏览器离线，语音指令已停止。'); client.connected = false; client.cancelFloor('浏览器已离线，麦权已在本地结束。'); client.audio.suspend('浏览器已离线。'); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) { client.voice.interrupt('页面隐藏，语音指令已停止。'); endPress(); client.audio.suspend('页面隐藏，音频已停止。'); } else if (client.allowed()) void client.refresh(); });
  window.addEventListener('online', () => { if (client.allowed()) void client.refresh(); });
  window.addEventListener('pagehide', () => { client.voice.interrupt('页面已离开，语音指令已停止。'); endPress(); client.audio.suspend(); if (client.identity && owns()) localStorage.removeItem(ownerKey(client.identity.device.id)); });
  window.addEventListener('pageshow', () => { if (client.identity && !identityChanged && claimOwner(localStorage, client.identity.device.id, tab)) void client.activate(); });
  window.addEventListener('storage', event => {
    if (!client.identity) { if (event.key === IDENTITY_KEY) location.reload(); return; }
    if (event.key === IDENTITY_KEY) { identityChanged = true; client.suspend('身份已在其他标签页更新，此页面已暂停。'); }
    else if (event.key === pendingKey(client.identity.device.id)) {
      client.pendingSwitch = read(pendingKey(client.identity.device.id));
      if (client.active) client.suspend('另一标签页更新了切群操作，请重新接管并确认终态后继续。');
    }
    else if (event.key === ownerKey(client.identity.device.id) && event.newValue !== tab) client.suspend('另一标签页已接管此身份，本页已退出麦权操作。');
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
