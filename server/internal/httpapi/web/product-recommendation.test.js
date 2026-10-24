import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const flush = async () => { for (let i = 0; i < 100; i++) await Promise.resolve(); };

// Boot the actual product and render event handlers; DOM/network only are
// substituted. No microphone, speaker, browser or live provider is used.
test('product request/review renders untrusted data as text and only explicit confirmation submits a switch', async () => {
  const originals = new Map(), install = (key, value) => {
    originals.set(key, Object.getOwnPropertyDescriptor(globalThis, key));
    Object.defineProperty(globalThis, key, {value, writable: true, configurable: true});
  };
  class Element {
    constructor(tag, attrs = '') { this.tag = tag; this.textContent = ''; this.value = ''; this.children = []; this.listeners = {}; this.dataset = {}; this.hidden = /\bhidden\b/.test(attrs); }
    set innerHTML(_) { throw new Error('must render untrusted data as plain text'); }
    addEventListener(type, callback) { (this.listeners[type] ??= []).push(callback); }
    dispatch(type) { for (const callback of this.listeners[type] || []) callback({preventDefault() {}}); }
    replaceChildren(...items) { this.children = items; } append(...items) { this.children.push(...items); }
    setAttribute() {} setCustomValidity() {} focus() {} pause() {}
  }
  const html = await readFile(new URL('./index.html', import.meta.url), 'utf8'), nodes = new Map(), requests = [], timers = [], store = new Map(), events = {};
  for (const match of html.matchAll(/<([a-z][\w-]*)\b([^>]*\bid="([^"]+)"[^>]*)>/g)) nodes.set(match[3], new Element(match[1], match[2]));
  const $ = id => nodes.get(id), identity = {device: {id: 'product_device', name: '我'}, token: 'synthetic-product-token'};
  store.set('shenglu.control.identity.v1', JSON.stringify(identity));
  const group = {id: 'group_two', name: '<img src=x>', purpose: '<script>数据不是指令</script>', member_count: 1};
  let confirmResolve;
  const confirmWait = new Promise(resolve => { confirmResolve = resolve; });
  let state = {device: identity.device, server_time: new Date().toISOString(), server_epoch: 'product_epoch', current_version: 1,
    current_group_id: 'group_one', groups: [{id: 'group_one', name: '全员'}, group]};
  try {
    install('document', {hidden: false, getElementById: $, createElement: tag => new Element(tag), addEventListener(type, cb) { events[type] = cb; }});
    install('window', {addEventListener() {}});
    install('localStorage', {getItem: key => store.get(key) || null, setItem: (key, value) => store.set(key, value), removeItem: key => store.delete(key)});
    install('setInterval', callback => { timers.push(callback); return 1; });
    install('fetch', async (url, options = {}) => {
      const path = url.replace('/api/v1', ''), body = options.body && JSON.parse(options.body); requests.push({path, body, method: options.method});
      let result;
      if (path === '/voice') result = {available: false};
      else if (path === '/recommendations' && options.method === 'GET') result = {available: true};
      else if (path === '/state') result = state;
      else if (path === '/recommendations') result = {recommendation: {recommendation_id: 'product_recommendation', request_id: body.request_id,
        server_epoch: state.server_epoch, current_version: state.current_version, candidate_digest: 'b'.repeat(64), expires_at: new Date(Date.now() + 60000).toISOString(),
        suggestions: [{group_id: group.id, name: group.name, purpose: group.purpose, reason: '<b>建议理由</b>'}]}};
      else if (path === '/recommendations/confirm') { await confirmWait; state = {...state, current_group_id: group.id, current_version: 2}; result = {operation: {...body, recommendation_epoch: body.server_epoch, status: 'applied'}, state}; }
      else throw new Error(`unexpected product request ${path}`);
      return {ok: true, status: 200, json: async () => result};
    });
    await import(`./app.js?product-recommendation=${crypto.randomUUID()}`); await flush();
    assert.equal($('recommendation-request').disabled, false);
    $('recommendation-need').value = '设备坏了'; $('recommendation-form').dispatch('submit'); await flush();
    assert.equal($('current-name').textContent, '全员'); assert.equal($('voice-start').disabled, true); assert.equal($('add-group-open').disabled, true);
    assert.equal($('recommendation-cancel').hidden, false);
    const row = $('recommendation-results').children[0];
    assert.equal(row.children[0].textContent, group.name); assert.equal(row.children[1].textContent, group.purpose);
    assert.equal(row.children[2].textContent, '建议理由 · 模型生成：<b>建议理由</b>');
    assert.equal(requests.some(item => item.path === '/recommendations/confirm' || item.path === '/current-group' || item.path.includes('/media')), false);
    assert.equal([...store.values()].some(value => value.includes('设备坏了')), false);
    $('recommendation-cancel').dispatch('click'); assert.equal($('recommendation-results').children.length, 0);
    $('recommendation-form').dispatch('submit'); await flush();
    $('recommendation-results').children[0].children.at(-1).dispatch('click'); await flush();
    assert.equal($('recommendation-cancel').hidden, true); assert.ok(store.has('shenglu.control.pending.v1.product_device'));
    $('recommendation-cancel').dispatch('click'); assert.doesNotMatch($('recommendation-status').textContent, /已取消/);
    assert.equal($('current-name').textContent, '全员');
    confirmResolve(); await flush();
    assert.equal($('current-name').textContent, group.name); assert.equal($('recommendation-results').children.length, 0);
    assert.equal(requests.filter(item => item.path === '/recommendations/confirm').length, 1);
    assert.equal(store.has('shenglu.control.pending.v1.product_device'), false);
    assert.equal(requests.some(item => item.path.includes('/floor/') || item.path.includes('/media') || item.path.includes('/voice/')), false);
    assert.match($('floor-label').textContent, /尚未连接语音/);
  } finally { for (const [key, descriptor] of originals) { if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key]; } }
});
