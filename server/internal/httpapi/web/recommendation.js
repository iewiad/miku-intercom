// Recommendations never execute commands. Only confirm() creates a durable
// control operation; ordinary PTT and explicit group-name voice stay separate.
export const validNeed = value => typeof value === 'string' && [...value.trim()].length >= 1 &&
  [...value.trim()].length <= 512 && !/\p{Cc}/u.test(value.trim());
const directory = state => JSON.stringify((state?.groups || []).map(({id, name, purpose = ''}) => ({id, name, purpose})).sort((a, b) => a.id.localeCompare(b.id)));
const binding = control => ({identity: control.identity?.token, epoch: control.state?.server_epoch,
  version: control.state?.current_version, directory: directory(control.state)});
const sameBinding = (a, b) => a && b && a.identity === b.identity && a.epoch === b.epoch && a.version === b.version && a.directory === b.directory;

export class RecommendationClient {
  constructor({control, onChange = () => {}}) {
    Object.assign(this, {control, onChange});
    this.available = false; this.phase = 'unavailable'; this.generation = 0; this.result = null; this.snapshot = null;
    this.message = '正在检查需求推荐。';
  }
  get blocking() { return this.phase === 'requesting' || this.phase === 'review'; }
  emit() { this.onChange(this); }
  async initialize() {
    this.interrupt();
    const generation = this.generation;
    try {
      const status = await this.control.request('/recommendations');
      if (generation !== this.generation || !this.control.allowed()) return;
      this.available = status.available === true;
    } catch { if (generation !== this.generation) return; this.available = false; }
    this.phase = this.available ? 'idle' : 'unavailable';
    this.message = this.available ? '描述需要的帮助，只向已加入的群推荐。' : '需求推荐暂不可用，可从群列表选择或按群名语音切换。';
    this.emit();
  }
  canRequest() {
    const c = this.control;
    return this.available && c.allowed() && c.connected && !!c.state?.groups?.length &&
      !c.uiBlocked && !c.voice?.blocking && !c.mutationBusy && !c.pendingSwitch && !this.blocking;
  }
  interrupt(message = '') {
    ++this.generation; this.result = null; this.snapshot = null;
    this.phase = this.available ? 'idle' : 'unavailable';
    if (message) this.message = message;
    this.emit();
  }
  observeState() {
    if (this.blocking && !sameBinding(this.snapshot, binding(this.control))) this.interrupt('群组或连接状态已变化，请重新获取建议。');
    else if (this.result && Date.parse(this.result.expires_at) <= this.control.serverNow()) this.interrupt('建议已过期，请重新描述需求。');
  }
  async request(need) {
    if (!this.canRequest()) return false;
    if (!validNeed(need)) { this.message = '请输入 1–512 个字符的需求，不能包含控制字符。'; this.emit(); return false; }
    const c = this.control;
    c.cancelFloor();
    this.snapshot = binding(c); this.result = null; this.phase = 'requesting';
    this.message = '正在向已加入的群获取建议，当前群保持不变。';
    const generation = ++this.generation, requestID = c.random(), groups = c.state.groups.map(group => ({...group}));
    this.emit();
    try {
      const {recommendation: result} = await c.request('/recommendations', 'POST', {
        request_id: requestID, need: need.trim(), expected_version: this.snapshot.version, server_epoch: this.snapshot.epoch,
      }, {timeoutMs: 35000});
      if (generation !== this.generation || !c.allowed()) return false;
      if (!sameBinding(this.snapshot, binding(c))) throw new Error('binding_changed');
      if (!result || result.request_id !== requestID || result.server_epoch !== this.snapshot.epoch || result.current_version !== this.snapshot.version ||
        typeof result.recommendation_id !== 'string' || !/^[A-Za-z0-9_-]{8,80}$/.test(result.recommendation_id) ||
        typeof result.candidate_digest !== 'string' || !/^[a-f0-9]{64}$/.test(result.candidate_digest) ||
        !Number.isFinite(Date.parse(result.expires_at)) || Date.parse(result.expires_at) <= c.serverNow() ||
        !Array.isArray(result.suggestions) || result.suggestions.length > 3) throw new Error('invalid_recommendation');
      const seen = new Set();
      for (const item of result.suggestions) {
        const group = groups.find(group => group.id === item.group_id);
        if (!group || seen.has(item.group_id) || item.name !== group.name || (item.purpose || '') !== (group.purpose || '') ||
          typeof item.reason !== 'string' || !item.reason.trim() || [...item.reason].length > 256 || /\p{Cc}/u.test(item.reason)) throw new Error('invalid_suggestion');
        seen.add(item.group_id);
      }
      if (!result.suggestions.length) {
        this.interrupt('没有找到合适的已加入群。可修改需求，或从群列表选择。'); return true;
      }
      this.result = result; this.phase = 'review';
      this.message = '请核对职责和建议理由，选择后明确确认进入。建议不代表有人在线或已接听。'; this.emit(); return true;
    } catch (error) {
      if (generation !== this.generation) return false;
      const messages = {recommendation_unavailable: '需求推荐暂不可用，请从群列表选择。',
        recommendation_busy: '推荐正在处理，请稍后重试。', recommendation_rate_limited: '请求较频繁，请稍后重试。',
        version_conflict: '当前群已变化，请重新获取建议。', epoch_conflict: '服务已重新连接，请重新获取建议。'};
      this.interrupt(messages[error.code] || '未取得可验证的建议。当前群保持不变，可重新尝试或从群列表选择。'); return false;
    }
  }
  async confirm(groupID) {
    const c = this.control, result = this.result;
    if (this.phase !== 'review' || !result || !c.allowed() || !c.connected || c.uiBlocked || c.voice?.blocking || c.mutationBusy || c.pendingSwitch) return false;
    if (!sameBinding(this.snapshot, binding(c)) || Date.parse(result.expires_at) <= c.serverNow()) {
      this.interrupt('建议已失效，请重新获取。'); return false;
    }
    if (!result.suggestions.some(item => item.group_id === groupID)) return false;
    const accepted = await c.confirmRecommendation(result, groupID);
    if (accepted) this.interrupt('确认请求已提交，结果与语音连接状态见当前群。');
    return accepted;
  }
}
