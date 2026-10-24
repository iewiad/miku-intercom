import test from 'node:test';
import assert from 'node:assert/strict';
import {filterGroups, validPurpose} from './app.js';

test('local filtering matches names and optional purposes case-insensitively without altering the catalog', () => {
  const groups = [{id: 'old', name: '后勤组'}, {id: 'repair', name: 'Repair', purpose: 'Equipment Support'}, {id: 'other', name: 'Other', purpose: null}];
  const before = structuredClone(groups);
  assert.equal(filterGroups(groups, '  '), groups);
  assert.deepEqual(filterGroups(groups, 'REPAIR').map(group => group.id), ['repair']);
  assert.deepEqual(filterGroups(groups, 'sUpPoRt').map(group => group.id), ['repair']);
  assert.deepEqual(filterGroups(groups, '后勤').map(group => group.id), ['old']);
  assert.deepEqual(filterGroups(groups, 'missing'), []);
  assert.deepEqual(groups, before);
  assert.equal(validPurpose(''), true); assert.equal(validPurpose('🛠'.repeat(256)), true);
  assert.equal(validPurpose('维\n修'), false);
});

test('product purpose rendering and filtering are text-only, retain the current channel and never activate media or switch', async () => {
  const originals = new Map();
  const install = (key, value) => {
    originals.set(key, Object.getOwnPropertyDescriptor(globalThis, key));
    Object.defineProperty(globalThis, key, {value, writable: true, configurable: true});
  };
  class Element {
    constructor(tag = 'div') { this.tag = tag; this.textContent = ''; this.value = ''; this.dataset = {}; this.children = []; this.listeners = {}; }
    set innerHTML(value) { throw new Error('Product content must use textContent'); }
    addEventListener(type, callback) { (this.listeners[type] ??= []).push(callback); }
    async dispatch(type) { await Promise.all((this.listeners[type] || []).map(callback => callback({preventDefault() {}}))); }
    append(...children) { this.children.push(...children); }
    replaceChildren(...children) { this.children = children; }
    setAttribute() {} setCustomValidity(value) { this.validityMessage = value; } focus() {} pause() {}
    play() { throw new Error('Purpose UI must not start playback'); }
  }
  const nodes = new Map(), $ = id => { if (!nodes.has(id)) nodes.set(id, new Element()); return nodes.get(id); };
  const identity = {device: {id: 'purpose_device', name: '测试'}, token: 'synthetic-purpose-token'};
  const store = new Map([['shenglu.control.identity.v1', JSON.stringify(identity)]]);
  const unsafeName = '<img src=x onerror=alert(1)>', unsafePurpose = '<script>alert(2)</script>';
  let state = {device: identity.device, server_epoch: 'purpose_epoch', server_time: new Date().toISOString(), current_group_id: 'current', current_version: 1,
    groups: [{id: 'current', name: '当前群', member_count: 1}, {id: 'repair', name: 'Repair Team', purpose: 'Equipment Support', member_count: 2},
      {id: 'unsafe', name: unsafeName, purpose: unsafePurpose, member_count: 1}]};
  const requests = [], intervals = [];
  let failCreate = true, microphoneCalls = 0, peerCalls = 0;
  const response = (status, value) => ({ok: status < 400, status, json: async () => value});
  const flush = async () => { for (let i = 0; i < 80; i++) await Promise.resolve(); };
  try {
    install('document', {hidden: false, getElementById: $, createElement: tag => new Element(tag), addEventListener() {}});
    install('window', {addEventListener() {}});
    install('localStorage', {getItem: key => store.get(key) || null, setItem: (key, value) => store.set(key, value), removeItem: key => store.delete(key)});
    install('navigator', {mediaDevices: {getUserMedia() { microphoneCalls++; throw new Error('No capture allowed'); }}});
    install('RTCPeerConnection', class { constructor() { peerCalls++; throw new Error('No Peer allowed'); } });
    install('setInterval', callback => { intervals.push(callback); return intervals.length; });
    install('fetch', async (url, options = {}) => {
      const path = url.replace('/api/v1', ''), body = options.body && JSON.parse(options.body);
      requests.push({path, method: options.method, body});
      if (path === '/voice') return response(200, {available: false, command_id: ''});
      if (path === '/state') return response(200, state);
      if (path === '/groups' && options.method === 'POST') {
        if (failCreate) return response(400, {error: {code: 'invalid_purpose', message: '职责未被接受'}});
        const group = {id: 'created', name: body.name, purpose: body.purpose, member_count: 1};
        state = {...state, groups: [...state.groups, group]};
        return response(200, {group, state, code: 'ABCDEFGH'});
      }
      throw new Error(`Unexpected request: ${path}`);
    });
    await import(`./app.js?purpose-product-test=${Date.now()}`); await flush();
    assert.equal($('group-list').children.length, 3);
    const unsafeLabels = $('group-list').children[2].children[0];
    assert.equal(unsafeLabels.children[0].textContent, unsafeName);
    assert.equal(unsafeLabels.children.at(-1).textContent, unsafePurpose);
    assert.ok(unsafeLabels.children.every(child => child.tag === 'strong' || child.tag === 'span'));
    assert.equal($('group-list').children[0].children[0].children.length, 2); // Old group has no purpose.
    const baselineRequests = requests.length;
    $('group-filter').value = 'sUpPoRt'; await $('group-filter').dispatch('input');
    assert.equal($('group-list').children.length, 1);
    assert.equal($('group-list').children[0].children[0].children[0].textContent, 'Repair Team');
    assert.equal($('current-name').textContent, '当前群');
    assert.equal($('group-filter-status').textContent, '显示 1 / 3 个已加入群');
    assert.equal(requests.length, baselineRequests);
    // A normal polling re-render must preserve the text and filtered results.
    intervals[0](); await flush();
    assert.equal($('group-filter').value, 'sUpPoRt'); assert.equal($('group-list').children.length, 1);
    assert.equal(state.current_group_id, 'current'); assert.equal(state.current_version, 1);
    $('group-filter').value = 'no match'; await $('group-filter').dispatch('input');
    assert.equal($('group-list').children.length, 0); assert.equal($('empty-filter').hidden, false); assert.equal($('empty-groups').hidden, true);
    assert.equal($('current-name').textContent, '当前群');
    state = {...state, groups: []}; intervals[0](); await flush();
    assert.equal($('empty-groups').hidden, false); assert.equal($('empty-filter').hidden, true);
    $('group-filter').value = ''; state = {...state, groups: [{id: 'current', name: '当前群', member_count: 1}]};
    intervals[0](); await flush();
    $('create-name').value = '新群'; $('create-purpose').value = '  Equipment Support  ';
    await $('create-form').dispatch('submit'); await flush();
    assert.equal($('create-name').value, '新群'); assert.equal($('create-purpose').value, '  Equipment Support  ');
    failCreate = false;
    await $('create-form').dispatch('submit'); await flush();
    assert.equal($('create-name').value, ''); assert.equal($('create-purpose').value, '');
    assert.deepEqual(requests.filter(request => request.path === '/groups').map(request => request.body), [
      {name: '新群', purpose: 'Equipment Support'}, {name: '新群', purpose: 'Equipment Support'},
    ]);
    assert.equal(microphoneCalls, 0); assert.equal(peerCalls, 0);
    assert.ok(requests.every(request => ['/voice', '/state', '/groups'].includes(request.path)));
    assert.equal(state.current_group_id, 'current'); assert.equal(state.current_version, 1);
  } finally {
    for (const [key, descriptor] of originals) {
      if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key];
    }
  }
});
