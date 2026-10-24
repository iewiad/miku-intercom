// Passive diagnostics only: retain lifecycle facts, never media or identity data.
const EVENTS = new Set(['audio-state', 'visibility-hidden', 'hidden-closed', 'visibility-visible', 'window-blur', 'blur-cancelled', 'window-focus', 'disabled-closed']);
const PHASES = new Set(['disabled', 'waiting', 'connecting', 'playback_blocked', 'ready', 'error', 'closed']);
const FLOOR_PHASES = new Set(['idle', 'acquiring', 'held']);
const VISIBILITY = new Set(['visible', 'hidden', 'prerender']);
const PEER_STATES = new Set(['new', 'connecting', 'connected', 'disconnected', 'failed', 'closed']);
const TRACK_STATES = new Set(['live', 'ended']);
const choice = (value, allowed) => allowed.has(value) ? value : null;
const boolean = value => typeof value === 'boolean' ? value : null;
const count = value => Number.isSafeInteger(value) && value >= 0 ? value : null;
const object = value => value !== null && typeof value === 'object';
const read = (value, field) => { try { return value[field]; } catch { return undefined; } };
const copy = event => ({...event, peer: event.peer && {...event.peer}, capture: event.capture && {...event.capture},
  previous_peer: event.previous_peer && {...event.previous_peer}, previous_capture: event.previous_capture && {...event.previous_capture}});

export class LifecycleLog {
  #events = [];
  #dropped = 0;
  #sequence = 0;
  #peerSequence = 0;
  #peers = new WeakMap();
  #startedAt;
  #now;
  #wallNow;
  #limit;

  constructor({startedAt = performance.now(), now = () => performance.now(), wallNow = () => new Date().toISOString(), limit = 128} = {}) {
    this.#startedAt = Number.isFinite(startedAt) ? startedAt : 0;
    this.#now = now;
    this.#wallNow = wallNow;
    this.#limit = Number.isSafeInteger(limit) && limit > 0 ? limit : 128;
  }

  #peer(value) {
    if (!object(value)) return null;
    if (!this.#peers.has(value)) this.#peers.set(value, ++this.#peerSequence);
    return {seq: this.#peers.get(value), state: choice(read(value, 'connectionState'), PEER_STATES)};
  }

  #capture(value) {
    if (!object(value)) return null;
    return {track_state: choice(read(value, 'readyState'), TRACK_STATES), enabled: boolean(read(value, 'enabled')), muted: boolean(read(value, 'muted'))};
  }

  record(event, state = {}) {
    if (!EVENTS.has(event)) return null;
    if (!object(state)) state = {};
    const elapsed = this.#now() - this.#startedAt;
    const wallTime = this.#wallNow();
    const entry = {
      seq: ++this.#sequence,
      at_ms: Number.isFinite(elapsed) ? Math.max(0, Math.round(elapsed * 1000) / 1000) : null,
      at: typeof wallTime === 'string' && /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/.test(wallTime) && Number.isFinite(Date.parse(wallTime)) ? wallTime : null,
      event_type: event,
      is_trusted: boolean(state.isTrusted),
      visibility_state: choice(state.visibility, VISIBILITY),
      document_hidden: boolean(state.hidden),
      has_focus: boolean(state.focused),
      phase: choice(state.phase, PHASES),
      desired: boolean(state.desired),
      pressed: boolean(state.pressed),
      floor_phase: choice(state.floorPhase, FLOOR_PHASES),
      current_version: count(state.currentVersion),
      peer: this.#peer(state.peer),
      capture: this.#capture(state.track),
      previous_peer: this.#peer(state.previousPeer),
      previous_capture: this.#capture(state.previousTrack),
      open_peers: count(state.openPeers),
      output_attached: boolean(state.outputAttached),
    };
    this.#events.push(entry);
    if (this.#events.length > this.#limit) { this.#events.shift(); this.#dropped++; }
    return copy(entry);
  }

  snapshot() { return {events: this.#events.map(copy), dropped_events: this.#dropped}; }
}
