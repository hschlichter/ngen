// The watch page's script. It asks the engine for an offer, answers it, plays the video, reconnects when the connection goes, and tells
// the engine once a second what the browser decoded. With remote control on, the connection carries a control channel: JSON-RPC to the
// view's methods, the same ones its loopback endpoint serves. The panels are made from what the engine describes (rpc.describe,
// introspect.list, cvar.get), so a new method, record or cvar shows up here with no change to this file.
//
// One page for a phone and a desktop: the layout follows the window's size, and input comes through Pointer Events, so touch and mouse
// share one path and differ by pointerType.

'use strict';

const video = document.getElementById('video');
const pad = document.getElementById('pad');
const hint = document.getElementById('hint');
const sheet = document.getElementById('sheet');
const canvas = document.createElement('canvas');
const context = canvas.getContext('2d', { willReadFrequently: true });
// The numbers shared with the test pattern (videopattern.h): 8 colour bars over the top half, 32 time cells over the bottom eighth.
const timeCells = 32;
const barCount = 8;

let connection = null;
let session = 0;          // which watcher the engine counts this page as
let replaced = false;     // another page became the watcher: this one stops
let retryTimer = null;
let connectionState = 'starting';
let source = 'frame';
let clockOffset = 0;      // the engine's clock minus this device's, in milliseconds
let latencies = [];       // since the last report
let lastLatency = null;
let lastStats = { framesDecoded: 0, framesDropped: 0, framesPerSecond: 0 };
let testReport = null;    // the self-test's results, sent with the report
let hintTimer = null;
let controlOffered = false; // whether this connection's offer carried a control channel

// ---- small helpers ----

function h(tag, attributes, ...children) {
  const element = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes || {})) {
    if (key === 'class') {
      element.className = value;
    } else if (key.startsWith('on')) {
      element.addEventListener(key.slice(2), value);
    } else if (value === true) {
      element.setAttribute(key, '');
    } else if (value !== false && value !== null && value !== undefined) {
      element.setAttribute(key, value);
    }
  }
  for (const child of children) {
    if (child !== null && child !== undefined) {
      element.append(child);
    }
  }
  return element;
}

function clear(element) {
  while (element.firstChild) {
    element.removeChild(element.firstChild);
  }
}

function remember(key, fallback) {
  try {
    const value = localStorage.getItem('ngen.' + key);
    return value === null ? fallback : JSON.parse(value);
  } catch (error) {
    return fallback;
  }
}

function store(key, value) {
  try {
    localStorage.setItem('ngen.' + key, JSON.stringify(value));
  } catch (error) {
  }
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function short(value) {
  const text = typeof value === 'string' ? value : JSON.stringify(value);
  return text.length > 400 ? text.slice(0, 400) + '…' : text;
}

// ---- layout ----

function chooseLayout() {
  const width = window.innerWidth;
  const height = window.innerHeight;
  if (width >= 900 && height >= 500) {
    return 'wide';
  }
  if (width > height) {
    return 'landscape';
  }
  return 'narrow';
}

function pointerKind() {
  return window.matchMedia('(pointer: coarse)').matches ? 'coarse' : 'fine';
}

function applyLayout() {
  const layout = chooseLayout();
  if (document.body.dataset.layout !== layout) {
    document.body.dataset.layout = layout;
    document.body.classList.remove('sheetup');
  }
  document.documentElement.style.setProperty('--strip-height', document.getElementById('strip').offsetHeight + 'px');
  applyDock();
  showHint();
}

function panelShown(name) {
  if (document.body.classList.contains('nocontrol')) {
    return false;
  }
  if (document.body.dataset.layout === 'wide') {
    const place = dockPlace(name);
    return !document.body.classList.contains('fullvideo') && (place.float !== undefined || dock[place.area].active === name);
  }
  if (document.body.dataset.layout === 'landscape' && !document.body.classList.contains('sheetup')) {
    return false;
  }
  return panelElements[name].classList.contains('active');
}

function showHint() {
  if (!rpc.open()) {
    hint.textContent = '';
    return;
  }
  hint.textContent = pointerKind() === 'coarse'
    ? 'drag: look · two fingers: move · pinch: forward · tap: pick · double tap: frame'
    : 'right drag: look · wheel: speed · WASD QE: fly · click: pick · Alt: orbit · F: frame · Z R G · `: console';
  // It says how once, then gets out of the picture's way.
  clearTimeout(hintTimer);
  hintTimer = setTimeout(() => { hint.textContent = ''; }, 10000);
}

// ---- the connection ----

function showState() {
  const dot = document.getElementById('dot');
  dot.className = 'dot' + (connectionState === 'connected' ? ' connected' : (connectionState.startsWith('cannot') || connectionState === 'failed' ? ' failed' : ''));
  document.getElementById('state').textContent = connectionState + (rpc.open() ? ' · control' : '');
  document.getElementById('statFps').textContent = video.videoWidth > 0 ? Math.round(lastStats.framesPerSecond || 0) + ' fps' : '';
  document.getElementById('statSize').textContent = video.videoWidth > 0 ? video.videoWidth + '×' + video.videoHeight : '';
  document.getElementById('statLatency').textContent = lastLatency !== null ? Math.round(lastLatency) + ' ms' : '';
  if (video.videoWidth > 0) {
    document.documentElement.style.setProperty('--video-aspect', video.videoWidth + ' / ' + video.videoHeight);
  }
}

function setState(state) {
  connectionState = state;
  showState();
}

// The difference between the engine's clock and this device's, from the exchange with the shortest round trip of five.
async function measureClock() {
  let best = Infinity;
  for (let i = 0; i < 5; i++) {
    const sent = Date.now();
    const response = await fetch('/time', { cache: 'no-store' });
    const engine = Number(await response.text());
    const received = Date.now();
    if (received - sent < best) {
      best = received - sent;
      clockOffset = engine - (sent + received) / 2;
    }
  }
}

function gathered(peer) {
  return new Promise(resolve => {
    if (peer.iceGatheringState === 'complete') {
      resolve();
      return;
    }
    const timer = setTimeout(resolve, 2000);
    peer.addEventListener('icegatheringstatechange', () => {
      if (peer.iceGatheringState === 'complete') {
        clearTimeout(timer);
        resolve();
      }
    });
  });
}

function retryLater() {
  if (retryTimer !== null || replaced) {
    return;
  }
  retryTimer = setTimeout(() => {
    retryTimer = null;
    connect();
  }, 2000);
}

async function connect() {
  if (connection !== null) {
    connection.close();
  }
  rpc.detach();
  setState('connecting');
  const peer = new RTCPeerConnection();
  connection = peer;
  peer.ontrack = event => {
    video.srcObject = event.streams.length > 0 ? event.streams[0] : new MediaStream([event.track]);
    video.play().catch(() => {});
  };
  // The engine opens the control channel when remote control is on.
  peer.ondatachannel = event => {
    if (peer === connection && event.channel.label === 'control') {
      rpc.attach(event.channel);
    }
  };
  peer.onconnectionstatechange = () => {
    if (peer !== connection) {
      return;
    }
    setState(peer.connectionState);
    if (peer.connectionState === 'failed' || peer.connectionState === 'disconnected' || peer.connectionState === 'closed') {
      retryLater();
    }
  };
  try {
    const offered = await fetch('/offer', { method: 'POST' });
    if (!offered.ok) {
      throw new Error(await offered.text());
    }
    const offer = await offered.json();
    session = offer.session;
    controlOffered = offer.control === true;
    await peer.setRemoteDescription({ type: 'offer', sdp: offer.sdp });
    await peer.setLocalDescription(await peer.createAnswer());
    await gathered(peer);
    const answered = await fetch('/answer', { method: 'POST', body: peer.localDescription.sdp });
    if (!answered.ok) {
      throw new Error(await answered.text());
    }
  } catch (error) {
    if (peer === connection) {
      setState('cannot connect: ' + error.message);
      retryLater();
    }
  }
}

function drawFrame() {
  if (video.videoWidth === 0) {
    return false;
  }
  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  context.drawImage(video, 0, 0);
  return true;
}

// With the pattern as the source: the time drawn into the frame's cells against the time now.
function readLatency() {
  if (source !== 'pattern' || !drawFrame()) {
    return;
  }
  const now = Date.now() + clockOffset;
  const y = Math.floor(canvas.height * 15 / 16);
  const row = context.getImageData(0, y, canvas.width, 1).data;
  let stamp = 0;
  for (let cell = 0; cell < timeCells; cell++) {
    const x = Math.floor((cell + 0.5) * canvas.width / timeCells);
    const bit = row[x * 4 + 1] > 128 ? 1 : 0;
    stamp = stamp * 2 + bit;
  }
  let latency = (Math.floor(now) % 4294967296) - stamp;
  if (latency < -2147483648) {
    latency += 4294967296;
  }
  // A frame decoded while its cells were changing gives a time far off; those are left out.
  if (latency > -1000 && latency < 10000) {
    latencies.push(latency);
  }
}

function readBars() {
  if (source !== 'pattern' || !drawFrame()) {
    return null;
  }
  const y = Math.floor(canvas.height / 4);
  const row = context.getImageData(0, y, canvas.width, 1).data;
  const bars = [];
  for (let bar = 0; bar < barCount; bar++) {
    const x = Math.floor((bar + 0.5) * canvas.width / barCount);
    bars.push([row[x * 4], row[x * 4 + 1], row[x * 4 + 2]]);
  }
  return bars;
}

function everyVideoFrame() {
  readLatency();
  video.requestVideoFrameCallback(everyVideoFrame);
}

async function sendFrame() {
  if (!drawFrame()) {
    return;
  }
  const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/png'));
  await fetch('/frame', { method: 'POST', body: blob });
}

async function report() {
  if (connection !== null) {
    try {
      const stats = await connection.getStats();
      stats.forEach(entry => {
        if (entry.type === 'inbound-rtp' && entry.kind === 'video') {
          lastStats = entry;
        }
      });
    } catch (error) {
    }
  }
  if (!('requestVideoFrameCallback' in video)) {
    readLatency();
  }
  if (latencies.length > 0) {
    lastLatency = latencies.reduce((sum, value) => sum + value, 0) / latencies.length;
  } else {
    lastLatency = null;
  }
  latencies = [];
  showState();
  try {
    const response = await fetch('/report', {
      method: 'POST',
      body: JSON.stringify({
        session: session,
        agent: navigator.userAgent,
        state: connectionState,
        framesDecoded: lastStats.framesDecoded || 0,
        framesDropped: lastStats.framesDropped || 0,
        framesPerSecond: lastStats.framesPerSecond || 0,
        width: video.videoWidth,
        height: video.videoHeight,
        latencyMs: lastLatency,
        bars: readBars(),
        layout: document.body.dataset.layout,
        pointer: pointerKind(),
        test: testReport,
      }),
    });
    const answer = await response.json();
    source = answer.source;
    if (answer.watcher === 'another' && !replaced && session !== 0) {
      // One watcher at a time: the page opened last has it. This one stops, so the two don't take it from each other for ever.
      replaced = true;
      if (connection !== null) {
        connection.close();
      }
      rpc.detach();
      setState('another page is watching; reload this one to watch here');
    } else if (answer.watcher === 'none' && connectionState === 'connected') {
      // The engine dropped this connection (remoteview-disconnect, or it was restarted): the browser notices only seconds later.
      setState('dropped by the engine');
      retryLater();
    } else if (answer.watcher === 'you' && connectionState === 'connected' && answer.control !== controlOffered) {
      // Remote control was turned on or off: the channel comes and goes with a new connection.
      connect();
    }
    if (answer.grab) {
      await sendFrame();
    }
  } catch (error) {
  }
}

// ---- remote control: JSON-RPC on the control channel ----

const rpc = {
  channel: null,
  nextId: 1,
  pending: new Map(),
  listeners: new Map(),

  open() {
    return this.channel !== null && this.channel.readyState === 'open';
  },
  attach(channel) {
    this.channel = channel;
    channel.onopen = () => {
      if (channel === this.channel) {
        controlOpened();
      }
    };
    channel.onclose = () => {
      if (channel === this.channel) {
        this.detach();
      }
    };
    channel.onmessage = event => {
      let message = null;
      try {
        message = JSON.parse(event.data);
      } catch (error) {
        return;
      }
      if (message.method !== undefined) {
        const listener = this.listeners.get(message.method);
        if (listener) {
          listener(message.params || {});
        }
        return;
      }
      const waiting = this.pending.get(message.id);
      if (!waiting) {
        return;
      }
      this.pending.delete(message.id);
      if (message.error) {
        waiting.reject(new Error(message.error.message || 'failed'));
      } else {
        waiting.resolve(message.result);
      }
    };
    if (channel.readyState === 'open') {
      controlOpened();
    }
  },
  detach() {
    const had = this.channel !== null;
    this.channel = null;
    for (const waiting of this.pending.values()) {
      waiting.reject(new Error('the control channel closed'));
    }
    this.pending.clear();
    if (had) {
      controlClosed();
    }
  },
  call(method, params) {
    if (!this.open()) {
      return Promise.reject(new Error('no control channel'));
    }
    const id = this.nextId++;
    const promise = new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
    });
    this.channel.send(JSON.stringify({ jsonrpc: '2.0', id: id, method: method, params: params || {} }));
    return promise;
  },
  on(method, listener) {
    this.listeners.set(method, listener);
  },
};

// What the engine last said, kept for the panels and the input.
const engine = {
  status: null,       // the status record: camera, selection, frame
  remoteview: null,   // the remoteview record: the frame's size, the video
  cvars: new Map(),   // name to its cvar.get entry
  methods: [],
  records: [],
};

function controlOpened() {
  document.body.classList.remove('nocontrol');
  applyLayout();
  showState();
  logLines.length = 0;
  rpc.call('log.subscribe', {}).catch(() => {});
  refreshDescriptions();
  poll(true);
  if (location.hash.startsWith('#test') && testReport === null) {
    runSelfTest();
  }
}

function controlClosed() {
  document.body.classList.add('nocontrol');
  document.body.classList.remove('sheetup');
  showState();
  showHint();
}

async function refreshDescriptions() {
  try {
    engine.methods = await rpc.call('rpc.describe');
    engine.records = (await rpc.call('introspect.list')).records || [];
    await refreshCvars();
    buildMethods();
    buildRecords();
    buildCvars();
    buildRender();
  } catch (error) {
    consoleLine('error', 'cannot read what the engine describes: ' + error.message);
  }
}

async function refreshCvars() {
  const answer = await rpc.call('cvar.get', {});
  for (const entry of answer.values || []) {
    engine.cvars.set(entry.name, entry);
    for (const row of cvarRows.get(entry.name) || []) {
      row.update(entry);
    }
  }
}

let pollCount = 0;
let polling = false;

// Twice a second while control is open: the small records every time, the larger ones for the panels that are shown.
async function poll(first) {
  if (!rpc.open() || polling) {
    return;
  }
  polling = true;
  pollCount++;
  try {
    engine.status = await rpc.call('introspect.get', { name: 'status' });
    if (first || pollCount % 4 === 0 || engine.remoteview === null) {
      engine.remoteview = await rpc.call('introspect.get', { name: 'remoteview' });
      showVideoHeight();
    }
    if (document.body.dataset.layout === 'wide' && pollCount % 2 === 0) {
      const profile = await rpc.call('introspect.get', { name: 'profile' });
      document.getElementById('statCpu').textContent = 'frame ' + (profile.cpuMsAverage || 0).toFixed(1) + ' ms';
      document.getElementById('statGpu').textContent = 'GPU ' + (profile.gpuMsAverage || 0).toFixed(1) + ' ms';
    }
    if (panelShown('cvars') || panelShown('render')) {
      await refreshCvars();
    }
    if (panelShown('scene')) {
      showScene();
    }
    if (panelShown('records') && openRecord !== null && pollCount % 2 === 0) {
      await showRecord(openRecord);
    }
  } catch (error) {
  }
  polling = false;
}

// ---- a JSON value as a tree ----

function tree(value, key, path, open) {
  if (value === null || typeof value !== 'object') {
    const kind = value === null ? 'null' : typeof value;
    let text = String(value);
    if (typeof value === 'number' && !Number.isInteger(value)) {
      text = String(Math.round(value * 1000) / 1000);
    } else if (value === '') {
      text = '""';
    }
    return h('div', { class: 'kv' }, key === null ? null : h('span', { class: 'k' }, key), h('span', { class: 'v ' + kind }, text));
  }
  const entries = Array.isArray(value) ? value.map((item, index) => [String(index), item]) : Object.entries(value);
  const details = h('details', { class: 'tree' });
  const label = Array.isArray(value) ? '[' + entries.length + ']' : '{' + entries.length + '}';
  details.append(h('summary', {}, key === null ? null : h('span', {}, key), h('span', { class: 'count' }, label)));
  const body = h('div', { class: 'body' });
  details.append(body);
  let built = false;
  const build = () => {
    if (built) {
      return;
    }
    built = true;
    const limit = 300;
    for (const [childKey, child] of entries.slice(0, limit)) {
      body.append(tree(child, childKey, path + '/' + childKey, open));
    }
    if (entries.length > limit) {
      body.append(h('div', { class: 'note' }, (entries.length - limit) + ' more not shown'));
    }
  };
  details.addEventListener('toggle', () => {
    if (details.open) {
      open.add(path);
      build();
    } else {
      open.delete(path);
    }
  });
  if (open.has(path)) {
    details.open = true;
    build();
  }
  return details;
}

// ---- tabs ----

const tabNames = ['scene', 'render', 'cvars', 'console', 'methods', 'records'];
// The panels themselves, kept by name: the dock moves them between its areas and windows, and one is out of the page for a moment
// while its window is rebuilt.
const panelElements = Object.fromEntries(tabNames.map(name => [name, document.getElementById('panel-' + name)]));

function showTab(name) {
  store('tab', name);
  for (const panel of document.querySelectorAll('.panel')) {
    panel.classList.toggle('active', panel.id === 'panel-' + name);
  }
  for (const button of document.querySelectorAll('#tabs button')) {
    button.classList.toggle('active', button.dataset.tab === name);
  }
  if (document.body.dataset.layout === 'wide') {
    // In the dock: the panel comes to the front of the area it is in, or its window to the front of the windows.
    const place = dockPlace(name);
    if (place.area !== undefined) {
      dock[place.area].active = name;
    } else {
      dock.floats.push(dock.floats.splice(place.float, 1)[0]);
    }
    saveDock();
    applyDock();
  }
  poll(false);
}

function buildTabs() {
  const tabs = document.getElementById('tabs');
  for (const name of tabNames) {
    tabs.append(h('button', { 'data-tab': name, onclick: () => showTab(name) }, panelTitle(name)));
  }
  const saved = remember('tab', 'scene');
  showTab(tabNames.includes(saved) ? saved : 'scene');
}

function panelTitle(name) {
  return panelElements[name].dataset.title;
}

// ---- the dock (the wide layout): panels in areas left, right and under the video, or floating; dragged by their tabs ----

const dockAreas = ['left', 'right', 'bottom'];
const dockMinSize = 180;

function dockDefault() {
  return {
    left: { panels: [], active: null, size: 340 },
    right: { panels: ['scene', 'render', 'cvars', 'methods', 'records'], active: 'scene', size: 460 },
    bottom: { panels: ['console'], active: 'console', size: 240 },
    floats: [], // {name, x, y, width, height}, the last in front
  };
}

// The remembered layout, when it still places every panel exactly once; the default otherwise.
function loadDock() {
  const saved = remember('dock', null);
  try {
    const placed = [...dockAreas.flatMap(area => saved[area].panels), ...saved.floats.map(float => float.name)].sort();
    if (JSON.stringify(placed) === JSON.stringify([...tabNames].sort())) {
      return saved;
    }
  } catch (error) {
  }
  return dockDefault();
}

let dock = loadDock();

function saveDock() {
  store('dock', dock);
}

// Where a panel is: {area}, or {float} with its place among the windows.
function dockPlace(name) {
  for (const area of dockAreas) {
    if (dock[area].panels.includes(name)) {
      return { area: area };
    }
  }
  return { float: dock.floats.findIndex(float => float.name === name) };
}

function dockElement(area) {
  return document.querySelector('.dock[data-area="' + area + '"]');
}

// Puts every panel where the layout has it: in the wide layout into the dock's areas and windows, otherwise back into the sheet.
function applyDock() {
  const floats = document.getElementById('floats');
  if (document.body.dataset.layout !== 'wide') {
    const panels = document.getElementById('panels');
    for (const name of tabNames) {
      const panel = panelElements[name];
      panel.classList.remove('shown');
      panels.append(panel);
    }
    clear(floats);
    return;
  }
  const root = document.documentElement.style;
  for (const area of dockAreas) {
    const state = dock[area];
    const element = dockElement(area);
    const tabs = element.querySelector('.docktabs');
    const body = element.querySelector('.dockbody');
    const empty = state.panels.length === 0;
    if (!empty && !state.panels.includes(state.active)) {
      state.active = state.panels[0];
    }
    clear(tabs);
    for (const name of state.panels) {
      const tab = h('button', { 'data-tab': name, class: name === state.active ? 'active' : '' }, panelTitle(name));
      tab.addEventListener('pointerdown', event => beginPanelDrag(event, name, tab, null));
      tabs.append(tab);
      const panel = panelElements[name];
      panel.classList.toggle('shown', name === state.active);
      body.append(panel);
    }
    element.classList.toggle('empty', empty);
    document.querySelector('.split[data-area="' + area + '"]').classList.toggle('empty', empty);
    const most = (area === 'bottom' ? window.innerHeight : window.innerWidth) * (area === 'bottom' ? 0.6 : 0.4);
    const size = empty ? 0 : Math.round(Math.max(dockMinSize, Math.min(state.size, most)));
    root.setProperty('--dock-' + area, size + 'px');
    root.setProperty('--split-' + area, empty ? '0px' : '6px');
  }
  clear(floats);
  for (const float of dock.floats) {
    float.x = Math.max(0, Math.min(float.x, window.innerWidth - 120));
    float.y = Math.max(0, Math.min(float.y, window.innerHeight - 60));
    const title = h('div', { class: 'floattitle' }, panelTitle(float.name));
    const body = h('div', { class: 'floatbody' });
    const element = h('div', { class: 'float', 'data-float': float.name }, title, body);
    element.style.left = float.x + 'px';
    element.style.top = float.y + 'px';
    element.style.width = float.width + 'px';
    element.style.height = float.height + 'px';
    const panel = panelElements[float.name];
    panel.classList.add('shown');
    body.append(panel);
    floats.append(element);
    title.addEventListener('pointerdown', event => beginPanelDrag(event, float.name, title, element));
    // Its size is the browser's to change (the corner); the layout remembers it.
    new ResizeObserver(() => {
      if (element.isConnected && element.offsetWidth > 0 && (float.width !== element.offsetWidth || float.height !== element.offsetHeight)) {
        float.width = element.offsetWidth;
        float.height = element.offsetHeight;
        saveDock();
      }
    }).observe(element);
  }
}

// Moves a panel to an area, or makes it a window at a point of the page.
function dockMove(name, target, point) {
  const from = dockPlace(name);
  let size = { width: 440, height: 480 };
  if (from.area !== undefined) {
    const state = dock[from.area];
    state.panels.splice(state.panels.indexOf(name), 1);
    if (state.active === name) {
      state.active = state.panels.length > 0 ? state.panels[0] : null;
    }
  } else {
    const float = dock.floats.splice(from.float, 1)[0];
    size = { width: float.width, height: float.height };
  }
  if (target === 'float') {
    const at = point || { x: window.innerWidth / 2, y: window.innerHeight / 2 };
    dock.floats.push({ name: name, x: Math.round(at.x - size.width / 2), y: Math.round(at.y - 14), width: size.width, height: size.height });
  } else {
    dock[target].panels.push(name);
    dock[target].active = name;
  }
  saveDock();
  applyDock();
  poll(false);
}

// Where a panel dropped at a point goes: the area it is over, an edge of the page, or a window where it is.
function dropTarget(x, y) {
  for (const area of dockAreas) {
    const box = dockElement(area).getBoundingClientRect();
    if (box.width > 0 && x >= box.left && x < box.right && y >= box.top && y < box.bottom) {
      return area;
    }
  }
  if (x < 96) {
    return 'left';
  }
  if (x > window.innerWidth - 96) {
    return 'right';
  }
  if (y > window.innerHeight - 96) {
    return 'bottom';
  }
  return 'float';
}

// A tab or a window's title under the pointer: a press that doesn't move chooses the panel; one that does carries it, with the places
// it can be dropped lit, and a window follows the pointer.
function beginPanelDrag(event, name, handle, floatElement) {
  if (event.button !== undefined && event.button !== 0) {
    return;
  }
  event.preventDefault();
  try {
    handle.setPointerCapture(event.pointerId);
  } catch (error) {
  }
  const start = { x: event.clientX, y: event.clientY };
  const grab = floatElement ? { x: event.clientX - floatElement.offsetLeft, y: event.clientY - floatElement.offsetTop } : null;
  let dragging = false;
  let ghost = null;
  const zones = [...document.querySelectorAll('.zone')];
  const move = moveEvent => {
    if (!dragging && Math.hypot(moveEvent.clientX - start.x, moveEvent.clientY - start.y) < 6) {
      return;
    }
    if (!dragging) {
      dragging = true;
      document.body.classList.add('dragging');
      if (!floatElement) {
        ghost = h('div', { id: 'dragghost' }, panelTitle(name));
        document.body.append(ghost);
      }
    }
    const target = dropTarget(moveEvent.clientX, moveEvent.clientY);
    for (const zone of zones) {
      zone.classList.toggle('hot', zone.dataset.area === target);
    }
    if (floatElement) {
      floatElement.style.left = (moveEvent.clientX - grab.x) + 'px';
      floatElement.style.top = (moveEvent.clientY - grab.y) + 'px';
    } else {
      ghost.style.left = moveEvent.clientX + 'px';
      ghost.style.top = moveEvent.clientY + 'px';
    }
  };
  const up = upEvent => {
    handle.removeEventListener('pointermove', move);
    handle.removeEventListener('pointerup', up);
    handle.removeEventListener('pointercancel', up);
    document.body.classList.remove('dragging');
    if (ghost) {
      ghost.remove();
    }
    if (!dragging || upEvent.type === 'pointercancel') {
      if (!dragging) {
        showTab(name);
      } else {
        applyDock();
      }
      return;
    }
    const target = dropTarget(upEvent.clientX, upEvent.clientY);
    if (target === 'float' && floatElement) {
      // A window moved: it stays a window, where it was let go, in front.
      const float = dock.floats.splice(dockPlace(name).float, 1)[0];
      float.x = floatElement.offsetLeft;
      float.y = floatElement.offsetTop;
      dock.floats.push(float);
      saveDock();
      applyDock();
      return;
    }
    dockMove(name, target, { x: upEvent.clientX, y: upEvent.clientY });
  };
  handle.addEventListener('pointermove', move);
  handle.addEventListener('pointerup', up);
  handle.addEventListener('pointercancel', up);
}

function wireDock() {
  for (const split of document.querySelectorAll('.split')) {
    const area = split.dataset.area;
    split.addEventListener('pointerdown', event => {
      split.setPointerCapture(event.pointerId);
      const move = moveEvent => {
        if (area === 'left') {
          dock.left.size = moveEvent.clientX;
        } else if (area === 'right') {
          dock.right.size = window.innerWidth - moveEvent.clientX;
        } else {
          dock.bottom.size = window.innerHeight - moveEvent.clientY;
        }
        applyDock();
      };
      const up = () => {
        split.removeEventListener('pointermove', move);
        split.removeEventListener('pointerup', up);
        saveDock();
      };
      split.addEventListener('pointermove', move);
      split.addEventListener('pointerup', up);
    });
  }
  document.getElementById('layoutButton').addEventListener('click', () => {
    dock = dockDefault();
    saveDock();
    applyDock();
    poll(false);
  });
}

// ---- cvars: one editor per cvar, by its type ----

const cvarRows = new Map(); // name to the rows that show it

function cvarRow(entry, label) {
  const name = entry.name;
  const row = h('div', { class: 'row' });
  const nameCell = h('span', { class: 'name', title: name + '\n' + (entry.description || '') }, label || name);
  const edit = h('div', { class: 'edit' });
  const set = value => {
    rpc.call('cvar.set', { name: name, value: value }).then(refreshCvars).catch(error => consoleLine('error', name + ': ' + error.message));
  };
  let input = null;
  let valueText = null;
  let read = null;
  if (entry.type === 'bool') {
    input = h('input', { type: 'checkbox', onchange: () => set(input.checked) });
    read = value => { input.checked = value === true; };
  } else if (entry.type === 'enum') {
    input = h('select', { onchange: () => set(input.value) });
    for (const choice of entry.choices || []) {
      input.append(h('option', { value: choice }, choice));
    }
    read = value => { input.value = value; };
  } else if ((entry.type === 'int' || entry.type === 'float') && entry.min !== undefined && entry.max !== undefined) {
    const step = entry.type === 'int' ? 1 : (entry.max - entry.min) / 200;
    valueText = h('span', { class: 'value num' });
    input = h('input', { type: 'range', min: entry.min, max: entry.max, step: step, oninput: () => { valueText.textContent = input.value; }, onchange: () => set(Number(input.value)) });
    read = value => {
      input.value = value;
      valueText.textContent = entry.type === 'int' ? String(value) : String(Math.round(value * 1000) / 1000);
    };
  } else if (entry.type === 'int' || entry.type === 'float') {
    input = h('input', { type: 'number', step: entry.type === 'int' ? 1 : 'any', onchange: () => set(Number(input.value)) });
    read = value => { input.value = value; };
  } else {
    // A vec3 as "x,y,z", a string as it is.
    input = h('input', { type: 'text', onchange: () => set(input.value) });
    read = value => { input.value = Array.isArray(value) ? value.join(',') : value; };
  }
  const reset = h('button', { class: 'reset', title: 'Back to ' + entry.default, onclick: () => {
    rpc.call('cvar.reset', { name: name }).then(refreshCvars).catch(error => consoleLine('error', name + ': ' + error.message));
  } }, '↺');
  edit.append(input);
  if (valueText) {
    edit.append(valueText);
  }
  edit.append(reset);
  row.append(nameCell, edit);
  const item = {
    element: row,
    update(next) {
      if (document.activeElement !== input) {
        read(next.value);
      }
      row.classList.toggle('changed', !next.isDefault);
      reset.style.visibility = next.isDefault ? 'hidden' : 'visible';
    },
  };
  item.update(entry);
  if (!cvarRows.has(name)) {
    cvarRows.set(name, []);
  }
  cvarRows.get(name).push(item);
  return row;
}

function forgetRows(panel) {
  for (const [name, rows] of cvarRows) {
    cvarRows.set(name, rows.filter(row => !panel.contains(row.element)));
  }
}

function buildCvars() {
  const panel = panelElements.cvars;
  forgetRows(panel);
  clear(panel);
  const search = h('input', { class: 'search', type: 'search', placeholder: 'Search ' + engine.cvars.size + ' cvars', value: remember('cvarSearch', '') });
  const list = h('div', {});
  panel.append(search, list);
  const groups = new Map();
  for (const entry of engine.cvars.values()) {
    const group = entry.name.split('.')[0];
    if (!groups.has(group)) {
      groups.set(group, []);
    }
    groups.get(group).push(entry);
  }
  const openGroups = new Set(remember('cvarGroups', []));
  const sections = [];
  for (const [group, entries] of [...groups].sort((a, b) => a[0].localeCompare(b[0]))) {
    const details = h('details', {});
    const count = h('span', { class: 'count' }, String(entries.length));
    details.append(h('summary', {}, h('span', {}, group), count));
    const body = h('div', { class: 'body' });
    details.append(body);
    const rows = entries.map(entry => ({ entry: entry, element: cvarRow(entry, entry.name.slice(group.length + 1) || entry.name) }));
    for (const row of rows) {
      body.append(row.element);
    }
    details.open = openGroups.has(group);
    details.addEventListener('toggle', () => {
      if (details.open) {
        openGroups.add(group);
      } else {
        openGroups.delete(group);
      }
      store('cvarGroups', [...openGroups]);
    });
    list.append(details);
    sections.push({ group, details, rows, count });
  }
  const filter = () => {
    const words = search.value.toLowerCase().split(/\s+/).filter(word => word.length > 0);
    store('cvarSearch', search.value);
    for (const section of sections) {
      let shown = 0;
      for (const row of section.rows) {
        const text = (row.entry.name + ' ' + (row.entry.description || '')).toLowerCase();
        const match = words.every(word => text.includes(word));
        row.element.style.display = match ? '' : 'none';
        shown += match ? 1 : 0;
      }
      section.details.style.display = shown > 0 ? '' : 'none';
      section.count.textContent = String(shown);
      if (words.length > 0) {
        section.details.open = shown > 0;
      }
    }
  };
  search.addEventListener('input', filter);
  filter();
}

// The Render panel: the cvars someone watching reaches for first, by name; whichever of them this engine has.
const renderGroups = [
  ['View', ['render.view.mode', 'render.view.debug', 'render.view.hzblevel', 'render.antialiasing', 'render.prepass']],
  ['Shadows', ['render.shadow', 'render.shadow.cascades', 'render.shadow.distance', 'render.shadow.pcf']],
  ['Culling', ['render.culling.enabled', 'render.culling.frozen', 'render.culling.occlusion']],
  ['Overlays', 'tools.overlay.'],
  ['Video', ['remoteview.source', 'remoteview.height', 'remoteview.fps', 'remoteview.bitrateKbps', 'view.frameLimit']],
];

function buildRender() {
  const panel = panelElements.render;
  forgetRows(panel);
  clear(panel);
  for (const [title, names] of renderGroups) {
    const entries = typeof names === 'string'
      ? [...engine.cvars.values()].filter(entry => entry.name.startsWith(names))
      : names.map(name => engine.cvars.get(name)).filter(entry => entry !== undefined);
    if (entries.length === 0) {
      continue;
    }
    panel.append(h('h3', {}, title));
    for (const entry of entries) {
      panel.append(cvarRow(entry, entry.name.split('.').slice(1).join('.')));
    }
  }
}

// ---- the Scene panel ----

const sceneOpen = new Set();
let sceneFor = null;       // the selection the details below were asked for
let sceneDetails = null;

function buildScene() {
  const panel = panelElements.scene;
  clear(panel);
  panel.append(
    h('h3', {}, 'Selection'),
    h('div', { id: 'sceneSelected', class: 'mono note' }, 'nothing selected'),
    h('div', { class: 'buttons' },
      h('button', { onclick: () => frameSelection() }, 'Frame selection'),
      h('button', { onclick: () => callAndSay('view.camera.frame', { target: 'scene' }) }, 'Frame scene'),
      h('button', { onclick: () => callAndSay('view.select', { prim: '' }) }, 'Deselect')),
    h('div', { id: 'sceneDetails' }),
    h('h3', {}, 'Camera'),
    h('div', { id: 'scenePose', class: 'mono note num' }),
    h('div', { id: 'sceneCameras' }));
}

function callAndSay(method, params) {
  return rpc.call(method, params).then(result => {
    poll(false);
    return result;
  }).catch(error => {
    consoleLine('error', method + ': ' + error.message);
  });
}

function frameSelection() {
  const selected = engine.status ? engine.status.selected : '';
  return callAndSay('view.camera.frame', { target: selected || 'scene' });
}

async function showScene() {
  const status = engine.status;
  if (status === null || document.getElementById('sceneSelected') === null) {
    return;
  }
  const selected = status.selected || '';
  document.getElementById('sceneSelected').textContent = selected || 'nothing selected';
  if (status.camera) {
    const position = status.camera.position || [0, 0, 0];
    document.getElementById('scenePose').textContent =
      position.map(value => value.toFixed(2)).join(', ') + '  yaw ' + status.camera.yaw.toFixed(1) + '  pitch ' + status.camera.pitch.toFixed(1) +
      '  speed ' + status.camera.speed.toFixed(1) + ' m/s';
  }
  if (selected !== sceneFor) {
    sceneFor = selected;
    const details = document.getElementById('sceneDetails');
    clear(details);
    if (selected) {
      try {
        const assets = await rpc.call('view.assetsOf', { prim: selected });
        if (sceneFor === selected) {
          details.append(tree(assets.assets !== undefined ? assets.assets : assets, 'assets', 'assets', sceneOpen));
        }
      } catch (error) {
        details.append(h('div', { class: 'note' }, error.message));
      }
    }
  }
  if (sceneDetails === null) {
    sceneDetails = true;
    try {
      const cameras = (await rpc.call('view.camera.list')).cameras || [];
      const list = document.getElementById('sceneCameras');
      clear(list);
      if (cameras.length === 0) {
        list.append(h('div', { class: 'note' }, 'The scene authors no cameras.'));
      }
      for (const camera of cameras) {
        list.append(h('div', { class: 'row' },
          h('span', { class: 'name mono', title: camera.path }, camera.path),
          h('button', { onclick: () => callAndSay('view.camera.use', { camera: camera.path }) }, 'Use')));
      }
    } catch (error) {
    }
  }
}

// ---- the console: a line that runs a console command, and the log under it ----

const logLines = [];   // {severity, category, text}, the newest last
let logDirty = false;
let logFilter = remember('logFilter', '');
let logSeverity = remember('logSeverity', 'all');
const lineHistory = remember('history', []);
let historyAt = lineHistory.length;

function consoleLine(severity, text, category) {
  logLines.push({ severity: severity, category: category || '', text: text });
  if (logLines.length > 2000) {
    logLines.splice(0, logLines.length - 2000);
  }
  logDirty = true;
}

function buildConsole() {
  const panel = panelElements.console;
  clear(panel);
  const filter = h('input', { type: 'search', placeholder: 'Filter the log', value: logFilter, oninput: () => {
    logFilter = filter.value;
    store('logFilter', logFilter);
    logDirty = true;
  } });
  const severity = h('select', { onchange: () => {
    logSeverity = severity.value;
    store('logSeverity', logSeverity);
    logDirty = true;
  } }, h('option', { value: 'all' }, 'all'), h('option', { value: 'warning' }, 'warnings'), h('option', { value: 'error' }, 'errors'));
  severity.value = logSeverity;
  const line = h('input', { id: 'consoleInput', type: 'text', placeholder: 'A console command, such as: cvar render.shadow off', autocapitalize: 'off', autocomplete: 'off', spellcheck: 'false' });
  const run = () => {
    const text = line.value.trim();
    if (text.length === 0) {
      return;
    }
    runConsole(text);
    if (lineHistory[lineHistory.length - 1] !== text) {
      lineHistory.push(text);
      store('history', lineHistory.slice(-100));
    }
    historyAt = lineHistory.length;
    line.value = '';
  };
  line.addEventListener('keydown', event => {
    if (event.key === 'Enter') {
      run();
    } else if (event.key === 'ArrowUp' && historyAt > 0) {
      historyAt--;
      line.value = lineHistory[historyAt];
      event.preventDefault();
    } else if (event.key === 'ArrowDown' && historyAt < lineHistory.length) {
      historyAt++;
      line.value = historyAt < lineHistory.length ? lineHistory[historyAt] : '';
      event.preventDefault();
    }
  });
  panel.append(
    h('div', { class: 'consoleFilter' }, filter, severity),
    h('div', { id: 'log', class: 'mono' }),
    h('div', { class: 'consoleLine' }, line, h('button', { onclick: run }, 'Run')));
}

function runConsole(text) {
  consoleLine('echo', '> ' + text);
  return rpc.call('console.run', { line: text }).then(result => {
    const answer = result === null || (typeof result === 'object' && Object.keys(result).length === 0) ? 'ok' : short(result);
    consoleLine('answer', answer);
    poll(false);
  }).catch(error => {
    consoleLine('error', error.message);
  });
}

function drawLog() {
  if (!logDirty) {
    return;
  }
  logDirty = false;
  const log = document.getElementById('log');
  if (log === null || !panelShown('console')) {
    logDirty = true;
    return;
  }
  const atEnd = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
  const words = logFilter.toLowerCase().split(/\s+/).filter(word => word.length > 0);
  const shown = [];
  for (let index = logLines.length - 1; index >= 0 && shown.length < 300; index--) {
    const entry = logLines[index];
    const own = entry.severity === 'echo' || entry.severity === 'answer';
    if (!own && logSeverity === 'warning' && entry.severity !== 'warning' && entry.severity !== 'error') {
      continue;
    }
    if (!own && logSeverity === 'error' && entry.severity !== 'error') {
      continue;
    }
    const text = (entry.category + ' ' + entry.text).toLowerCase();
    if (!words.every(word => text.includes(word))) {
      continue;
    }
    shown.push(entry);
  }
  clear(log);
  for (const entry of shown.reverse()) {
    log.append(h('div', { class: 'line ' + entry.severity }, entry.category ? h('span', { class: 'category' }, entry.category + ' ') : null, entry.text));
  }
  if (atEnd) {
    log.scrollTop = log.scrollHeight;
  }
}

rpc.on('log.events', params => {
  if (params.dropped > 0) {
    consoleLine('warning', params.dropped + ' log events were dropped', 'log');
  }
  for (const event of params.events || []) {
    consoleLine(event.severity || 'info', event.text || event.name || event.type, event.category + '/' + event.type);
    logTypes.add(event.type);
  }
});
const logTypes = new Set(); // the event types seen, for the self-test

// ---- methods: a form for each, from its description ----

const methodOpen = new Set();

function buildMethods() {
  const panel = panelElements.methods;
  clear(panel);
  const search = h('input', { class: 'search', type: 'search', placeholder: 'Search ' + engine.methods.length + ' methods' });
  const list = h('div', {});
  panel.append(search, list);
  const items = [];
  for (const method of [...engine.methods].sort((a, b) => a.name.localeCompare(b.name))) {
    const details = h('details', {});
    details.append(h('summary', {}, h('span', { class: 'mono' }, method.name), h('span', { class: 'summary' }, method.summary || '')));
    const body = h('div', { class: 'body' });
    details.append(body);
    let built = false;
    details.addEventListener('toggle', () => {
      if (details.open && !built) {
        built = true;
        buildMethodForm(method, body);
      }
    });
    list.append(details);
    items.push({ method, details });
  }
  search.addEventListener('input', () => {
    const words = search.value.toLowerCase().split(/\s+/).filter(word => word.length > 0);
    for (const item of items) {
      const text = (item.method.name + ' ' + (item.method.summary || '')).toLowerCase();
      item.details.style.display = words.every(word => text.includes(word)) ? '' : 'none';
    }
  });
}

function buildMethodForm(method, body) {
  body.append(h('div', { class: 'note' }, method.summary || ''));
  const readers = [];
  for (const param of method.params || []) {
    let input = null;
    let read = null;
    if (param.type === 'bool') {
      input = h('select', {}, h('option', { value: '' }, param.required ? 'false' : '—'), h('option', { value: 'true' }, 'true'), h('option', { value: 'false' }, 'false'));
      read = () => (input.value === '' ? (param.required ? false : undefined) : input.value === 'true');
    } else if (param.type === 'enum') {
      input = h('select', {}, param.required ? null : h('option', { value: '' }, '—'));
      for (const value of param.enumValues || []) {
        input.append(h('option', { value: value }, value));
      }
      read = () => (input.value === '' ? undefined : input.value);
    } else if (param.type === 'int' || param.type === 'float') {
      input = h('input', { type: 'number', step: param.type === 'int' ? 1 : 'any' });
      read = () => (input.value === '' ? undefined : Number(input.value));
    } else if (param.type === 'string') {
      input = h('input', { type: 'text', autocapitalize: 'off', spellcheck: 'false' });
      read = () => (input.value === '' && !param.required ? undefined : input.value);
    } else if (param.type === 'vec3') {
      input = h('input', { type: 'text', placeholder: 'x, y, z' });
      read = () => (input.value === '' ? undefined : input.value.split(',').map(Number));
    } else {
      input = h('input', { type: 'text', placeholder: 'JSON', class: 'mono' });
      read = () => (input.value === '' ? undefined : JSON.parse(input.value));
    }
    input.title = param.description || '';
    readers.push({ name: param.name, read: read });
    body.append(h('div', { class: 'row' },
      h('span', { class: 'name', title: param.description || '' }, param.name + (param.required ? ' *' : '')),
      h('div', { class: 'edit' }, input)));
    if (param.description) {
      body.append(h('div', { class: 'note' }, param.description));
    }
  }
  const result = h('div', {});
  const call = h('button', { onclick: async () => {
    clear(result);
    const params = {};
    try {
      for (const reader of readers) {
        const value = reader.read();
        if (value !== undefined) {
          params[reader.name] = value;
        }
      }
      const answer = await rpc.call(method.name, params);
      result.append(tree(answer, 'result', method.name, methodOpen));
      const first = result.querySelector('details');
      if (first) {
        first.open = true;
      }
      poll(false);
    } catch (error) {
      result.append(h('div', { class: 'note bad' }, error.message));
    }
  } }, 'Call');
  body.append(h('div', { class: 'buttons' }, call), h('div', { class: 'note' }, 'Answers: ' + (method.result || '')), result);
}

// ---- records: each as a tree, refreshed while open ----

const recordOpen = new Set();
let openRecord = null;

function buildRecords() {
  const panel = panelElements.records;
  clear(panel);
  for (const record of [...engine.records].sort((a, b) => a.name.localeCompare(b.name))) {
    const details = h('details', { 'data-record': record.name });
    details.append(h('summary', {}, h('span', { class: 'mono' }, record.name), h('span', { class: 'summary' }, record.description || '')));
    details.append(h('div', { class: 'body' }));
    details.addEventListener('toggle', () => {
      if (details.open) {
        for (const other of panel.querySelectorAll('details[data-record]')) {
          if (other !== details) {
            other.open = false;
          }
        }
        openRecord = record.name;
        showRecord(record.name);
      } else if (openRecord === record.name) {
        openRecord = null;
      }
    });
    panel.append(details);
  }
}

async function showRecord(name) {
  const details = document.querySelector('details[data-record="' + name + '"]');
  if (details === null) {
    return;
  }
  const body = details.querySelector('.body');
  try {
    const value = await rpc.call('introspect.get', { name: name });
    const scroll = body.scrollTop;
    clear(body);
    // The record's own fields at the top, not under one more thing to open.
    if (value !== null && typeof value === 'object' && !Array.isArray(value)) {
      for (const [key, child] of Object.entries(value)) {
        body.append(tree(child, key, name + '/' + key, recordOpen));
      }
    } else {
      body.append(tree(value, null, name, recordOpen));
    }
    body.scrollTop = scroll;
  } catch (error) {
    clear(body);
    body.append(h('div', { class: 'note bad' }, error.message));
  }
}

// ---- the strip's controls ----

function showVideoHeight() {
  const select = document.getElementById('heightSelect');
  const height = engine.cvars.get('remoteview.height');
  if (height === undefined || document.activeElement === select) {
    return;
  }
  const known = ['360', '720', '1080'].includes(String(height.value));
  select.value = known ? String(height.value) : 'view';
}

function wireStrip() {
  document.getElementById('heightSelect').addEventListener('change', event => {
    let height = Number(event.target.value);
    if (event.target.value === 'view') {
      height = engine.remoteview && engine.remoteview.frame.height > 0 ? Math.min(engine.remoteview.frame.height, 2160) : 1440;
    }
    rpc.call('cvar.set', { name: 'remoteview.height', value: height }).then(refreshCvars).catch(() => {});
  });
  document.getElementById('shotButton').addEventListener('click', async () => {
    try {
      const shot = await rpc.call('remoteview.screenshot');
      const name = shot.url.split('/').pop();
      const link = h('a', { href: shot.url, download: name });
      document.body.append(link);
      link.click();
      link.remove();
      consoleLine('answer', 'screenshot ' + shot.width + '×' + shot.height + ': ' + shot.path);
    } catch (error) {
      consoleLine('error', 'screenshot: ' + error.message);
    }
  });
  document.getElementById('fullButton').addEventListener('click', toggleFull);
  document.getElementById('sheetButton').addEventListener('click', () => {
    document.body.classList.toggle('sheetup');
    poll(false);
  });
}

function toggleFull() {
  if (document.body.dataset.layout === 'wide') {
    document.body.classList.toggle('fullvideo');
  }
  const stage = document.getElementById('stage');
  if (document.fullscreenElement) {
    document.exitFullscreen().catch(() => {});
  } else if (stage.requestFullscreen && document.body.dataset.layout !== 'wide') {
    stage.requestFullscreen().catch(() => {});
  }
}

// ---- the camera and picking: touch, mouse and keys on the pad over the video ----
//
// The mouse and the keys are the view's own controls (src/apps/view.cpp, pollInput): the right button held looks, the wheel sets the fly
// speed, a left press picks, W A S D and Q E fly with Shift for speed, Left Alt held orbits around what is under the pointer (the wheel
// then moves toward it), Z deselects, R selects the parent, F frames the selection, G follows the ground. Touch has no such keys, so it
// has gestures of its own.

const lookDegreesPerPixel = { touch: 0.2, mouse: 0.15, pen: 0.15 };
const tapMaxPixels = 8;
const tapMaxMs = 500;
const doubleTapMs = 350;

const input = {
  pointers: new Map(),   // pointerId to {x, y, type, button}
  gesture: null,         // {moved, startMs, count, x, y} while any pointer is down
  pinch: 0,              // the last distance between two fingers
  keys: new Set(),       // the camera keys held
  move: { forward: 0, right: 0, up: 0, yaw: 0, pitch: 0, wheel: 0 }, // what has added up since the last call
  inFlight: false,
  lastTap: null,         // {ms, x, y}
  lastPick: Promise.resolve(),
  lastFrameMs: 0,
  mouse: null,           // where the mouse last was over the pad, in page coordinates
  orbiting: false,       // Left Alt is held
  orbitTimer: null,
};

function cameraSpeed() {
  return engine.status && engine.status.camera ? engine.status.camera.speed : 5;
}

function onPointerDown(event) {
  if (!rpc.open()) {
    return;
  }
  pad.focus({ preventScroll: true });
  try {
    pad.setPointerCapture(event.pointerId);
  } catch (error) {
  }
  const type = event.pointerType || 'mouse';
  input.pointers.set(event.pointerId, { x: event.clientX, y: event.clientY, type: type, button: event.button });
  if (type === 'mouse' && event.button === 0) {
    // The left button picks as it goes down, as the view's does.
    input.lastPick = pickAt(event.clientX, event.clientY);
  }
  if (input.pointers.size === 1) {
    input.gesture = { moved: 0, startMs: performance.now(), count: 1, x: event.clientX, y: event.clientY, type: type, button: event.button };
  } else if (input.gesture) {
    input.gesture.count = Math.max(input.gesture.count, input.pointers.size);
    input.pinch = fingerDistance();
  }
  event.preventDefault();
}

function fingerDistance() {
  const points = [...input.pointers.values()];
  if (points.length < 2) {
    return 0;
  }
  return Math.hypot(points[0].x - points[1].x, points[0].y - points[1].y);
}

function onPointerMove(event) {
  if ((event.pointerType || 'mouse') === 'mouse') {
    input.mouse = { x: event.clientX, y: event.clientY };
  }
  const pointer = input.pointers.get(event.pointerId);
  if (!pointer || !input.gesture) {
    return;
  }
  const dx = event.clientX - pointer.x;
  const dy = event.clientY - pointer.y;
  pointer.x = event.clientX;
  pointer.y = event.clientY;
  input.gesture.moved += Math.abs(dx) + Math.abs(dy);
  const speed = cameraSpeed();
  if (input.pointers.size >= 2) {
    // Two fingers: their common movement moves the camera sideways and up (the world follows the fingers); their spread moves it
    // forward. Each finger's event carries half the pair's movement.
    input.move.right -= dx / 2 * speed / 200;
    input.move.up += dy / 2 * speed / 200;
    const distance = fingerDistance();
    input.move.forward += (distance - input.pinch) * speed / 100;
    input.pinch = distance;
  } else if (pointer.type === 'mouse' && pointer.button !== 2) {
    // Only the right button looks; the left one picked when it went down.
  } else {
    const degrees = lookDegreesPerPixel[pointer.type] || lookDegreesPerPixel.mouse;
    input.move.yaw += dx * degrees;
    input.move.pitch -= dy * degrees;
  }
  event.preventDefault();
}

function onPointerUp(event) {
  const pointer = input.pointers.get(event.pointerId);
  if (!pointer) {
    return;
  }
  input.pointers.delete(event.pointerId);
  input.pinch = fingerDistance();
  const gesture = input.gesture;
  if (input.pointers.size > 0 || !gesture) {
    return;
  }
  input.gesture = null;
  const now = performance.now();
  // A tap is touch's way to pick; the mouse picked with its left button down.
  const tap = event.type === 'pointerup' && gesture.type !== 'mouse' && gesture.count === 1 && gesture.moved <= tapMaxPixels && now - gesture.startMs <= tapMaxMs;
  if (!tap) {
    return;
  }
  const last = input.lastTap;
  if (last && now - last.ms <= doubleTapMs && Math.hypot(last.x - event.clientX, last.y - event.clientY) < 30) {
    // The second tap of two: frame what the first picked.
    input.lastTap = null;
    input.lastPick.then(() => frameSelection());
    return;
  }
  input.lastTap = { ms: now, x: event.clientX, y: event.clientY };
  input.lastPick = pickAt(event.clientX, event.clientY);
}

// A point of the page as the frame's pixel: the video is shown whole inside the pad, with bars where their shapes differ.
function framePixel(clientX, clientY) {
  const frame = engine.remoteview ? engine.remoteview.frame : null;
  if (!frame || frame.width === 0 || video.videoWidth === 0) {
    return null;
  }
  const box = pad.getBoundingClientRect();
  const scale = Math.min(box.width / video.videoWidth, box.height / video.videoHeight);
  const shownWidth = video.videoWidth * scale;
  const shownHeight = video.videoHeight * scale;
  const x = (clientX - box.left - (box.width - shownWidth) / 2) / shownWidth;
  const y = (clientY - box.top - (box.height - shownHeight) / 2) / shownHeight;
  if (x < 0 || x >= 1 || y < 0 || y >= 1) {
    return null;
  }
  return { x: Math.floor(x * frame.width), y: Math.floor(y * frame.height) };
}

async function pickAt(clientX, clientY) {
  const pixel = framePixel(clientX, clientY);
  if (pixel === null) {
    return null;
  }
  try {
    const picked = await rpc.call('view.pick', pixel);
    await poll(false);
    return picked;
  } catch (error) {
    consoleLine('error', 'pick: ' + error.message);
    return null;
  }
}

const cameraKeys = { KeyW: ['forward', 1], KeyS: ['forward', -1], KeyD: ['right', 1], KeyA: ['right', -1], KeyE: ['up', 1], KeyQ: ['up', -1] };

function onKeyDown(event) {
  if (!rpc.open() || event.metaKey) {
    return;
  }
  if (event.code === 'AltLeft') {
    // Held: orbit around what is under the pointer. The browser must not take it for its menu.
    event.preventDefault();
    if (!event.repeat) {
      startOrbit();
    }
    return;
  }
  if (event.code === 'ShiftLeft' || event.code === 'ShiftRight') {
    input.keys.add('Shift');
    return;
  }
  if (event.ctrlKey) {
    return;
  }
  if (event.code in cameraKeys) {
    input.keys.add(event.code);
  } else if (event.repeat) {
    return;
  } else if (event.code === 'KeyZ') {
    callAndSay('view.select', { prim: '' });
  } else if (event.code === 'KeyR') {
    selectParent();
  } else if (event.code === 'KeyF') {
    frameSelection();
  } else if (event.code === 'KeyG') {
    toggleFollow();
  } else if (event.code === 'Backquote') {
    focusConsole();
  } else {
    return;
  }
  event.preventDefault();
}

function onKeyUp(event) {
  input.keys.delete(event.code);
  if (event.code === 'ShiftLeft' || event.code === 'ShiftRight') {
    input.keys.delete('Shift');
  }
  if (event.code === 'AltLeft') {
    event.preventDefault();
    stopOrbit();
  }
}

// The selection's parent, as the view's R: the path without its last part; nothing above the root.
function selectParent() {
  const selected = engine.status ? engine.status.selected || '' : '';
  const slash = selected.lastIndexOf('/');
  return callAndSay('view.select', { prim: slash <= 0 ? '' : selected.slice(0, slash) });
}

async function toggleFollow() {
  try {
    const follow = (await rpc.call('cvar.get', { name: 'view.camera.follow' })).values[0].value;
    await rpc.call('cvar.set', { name: 'view.camera.follow', value: !follow });
    await refreshCvars();
  } catch (error) {
    consoleLine('error', 'follow: ' + error.message);
  }
}

// Left Alt went down: the engine picks the pivot under the pointer and orbits while it is told to, which is while the key is held.
function startOrbit() {
  const at = input.mouse || padPoint(0.5, 0.5);
  const pixel = framePixel(at.x, at.y) || framePixel(padPoint(0.5, 0.5).x, padPoint(0.5, 0.5).y);
  if (pixel === null) {
    return;
  }
  input.orbiting = true;
  rpc.call('view.camera.orbit.start', pixel).catch(() => {});
  clearInterval(input.orbitTimer);
  input.orbitTimer = setInterval(() => {
    rpc.call('view.camera.orbit.start', {}).catch(() => {});
  }, 500);
}

function stopOrbit() {
  if (!input.orbiting) {
    return;
  }
  input.orbiting = false;
  clearInterval(input.orbitTimer);
  rpc.call('view.camera.orbit.stop').catch(() => {});
}

function focusConsole() {
  showTab('console');
  if (document.body.dataset.layout !== 'wide') {
    document.body.classList.add('sheetup');
  }
  const line = document.getElementById('consoleInput');
  if (line) {
    line.focus();
  }
}

function onWheel(event) {
  if (!rpc.open()) {
    return;
  }
  // A notch of the wheel is a step: the fly speed, or toward the pivot while orbiting. Away from the user is positive, as the view's.
  const pixels = event.deltaMode === 1 ? event.deltaY * 33 : event.deltaY;
  input.move.wheel -= pixels / 100;
  event.preventDefault();
}

// Every animation frame: the held keys add to what the pointers moved, and what has added up goes out as one view.camera.move, with at
// most one call waiting for its answer.
function everyAnimationFrame(now) {
  const dt = Math.min(0.1, input.lastFrameMs > 0 ? (now - input.lastFrameMs) / 1000 : 0);
  input.lastFrameMs = now;
  if (input.keys.size > 0 && rpc.open()) {
    const metres = cameraSpeed() * dt * (input.keys.has('Shift') ? 5 : 1);
    for (const code of input.keys) {
      const key = cameraKeys[code];
      if (key) {
        input.move[key[0]] += key[1] * metres;
      }
    }
  }
  flushMove();
  drawLog();
  requestAnimationFrame(everyAnimationFrame);
}

function moveWaiting() {
  const move = input.move;
  return move.forward !== 0 || move.right !== 0 || move.up !== 0 || move.yaw !== 0 || move.pitch !== 0 || move.wheel !== 0;
}

function flushMove() {
  if (input.inFlight || !moveWaiting() || !rpc.open()) {
    return;
  }
  const move = input.move;
  input.move = { forward: 0, right: 0, up: 0, yaw: 0, pitch: 0, wheel: 0 };
  input.inFlight = true;
  // The camera the view's own keys and mouse move: the main one, unless the debug camera has the controls.
  move.camera = 'controlled';
  rpc.call('view.camera.move', move).then(pose => {
    if (engine.status && engine.status.camera && pose) {
      engine.status.camera.position = [pose.x, pose.y, pose.z];
      engine.status.camera.yaw = pose.yaw;
      engine.status.camera.pitch = pose.pitch;
      engine.status.camera.speed = pose.speed;
    }
  }).catch(() => {}).finally(() => {
    input.inFlight = false;
  });
}

function wirePad() {
  pad.addEventListener('pointerdown', onPointerDown);
  pad.addEventListener('pointermove', onPointerMove);
  pad.addEventListener('pointerup', onPointerUp);
  pad.addEventListener('pointercancel', onPointerUp);
  pad.addEventListener('contextmenu', event => event.preventDefault());
  pad.addEventListener('wheel', onWheel, { passive: false });
  pad.addEventListener('keydown', onKeyDown);
  pad.addEventListener('keyup', onKeyUp);
  pad.addEventListener('blur', () => {
    input.keys.clear();
    stopOrbit();
  });
}

// ---- the self-test (#test in the address): what an agent reads back through the remoteview record ----

async function settled() {
  for (let i = 0; i < 200; i++) {
    if (!input.inFlight && !moveWaiting()) {
      return;
    }
    await sleep(10);
  }
}

function padPoint(fx, fy) {
  const box = pad.getBoundingClientRect();
  return { x: box.left + box.width * fx, y: box.top + box.height * fy };
}

function pointer(type, pointerType, id, point, button) {
  pad.dispatchEvent(new PointerEvent(type, {
    pointerId: id, pointerType: pointerType, clientX: point.x, clientY: point.y, button: button || 0,
    buttons: type === 'pointerup' ? 0 : 1, bubbles: true, cancelable: true, isPrimary: id === 1,
  }));
}

async function drag(pointerType, from, to, button) {
  const id = pointerType === 'touch' ? 11 : 1;
  pointer('pointerdown', pointerType, id, from, button);
  for (let step = 1; step <= 5; step++) {
    pointer('pointermove', pointerType, id, { x: from.x + (to.x - from.x) * step / 5, y: from.y + (to.y - from.y) * step / 5 }, button);
  }
  pointer('pointerup', pointerType, id, to, button);
  await settled();
}

async function pose() {
  return rpc.call('view.camera.get');
}

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

async function runSelfTest() {
  const results = [];
  let testShot = null;
  testReport = { done: false, passed: 0, failed: 0, results: results };
  const check = async (name, body) => {
    try {
      const detail = await body();
      results.push({ name: name, ok: true, detail: detail === undefined ? '' : short(detail) });
      testReport.passed++;
    } catch (error) {
      results.push({ name: name, ok: false, detail: error.message });
      testReport.failed++;
    }
  };
  const expect = (condition, message) => {
    if (!condition) {
      throw new Error(message);
    }
  };
  // The first frames, and the first poll's records, before anything is measured.
  for (let i = 0; i < 100 && (video.videoWidth === 0 || engine.remoteview === null || engine.remoteview.frame.width === 0 || engine.status === null); i++) {
    await sleep(100);
    engine.remoteview = await rpc.call('introspect.get', { name: 'remoteview' }).catch(() => engine.remoteview);
  }

  await check('ping', () => rpc.call('rpc.ping'));
  await check('layout', async () => {
    const overflowX = document.documentElement.scrollWidth - window.innerWidth;
    const overflowY = document.documentElement.scrollHeight - window.innerHeight;
    expect(overflowX <= 0 && overflowY <= 0, 'the page overflows the window by ' + overflowX + ' × ' + overflowY);
    const box = pad.getBoundingClientRect();
    expect(box.width > 100 && box.height > 60, 'the video is ' + Math.round(box.width) + ' × ' + Math.round(box.height));
    return document.body.dataset.layout + ', ' + window.innerWidth + '×' + window.innerHeight + ', video ' + Math.round(box.width) + '×' + Math.round(box.height);
  });
  await check('describe', async () => {
    expect(engine.methods.length > 10 && engine.records.length > 5 && engine.cvars.size > 10, 'methods, records or cvars are missing');
    expect(document.querySelectorAll('#panel-cvars .row').length === engine.cvars.size, 'the Cvars panel does not hold every cvar');
    expect(document.querySelectorAll('#panel-methods details').length === engine.methods.length, 'the Methods panel does not hold every method');
    return engine.methods.length + ' methods, ' + engine.records.length + ' records, ' + engine.cvars.size + ' cvars';
  });
  await check('cvar set and read back', async () => {
    const before = (await rpc.call('cvar.get', { name: 'remoteview.bitrateKbps' })).values[0].value;
    await rpc.call('cvar.set', { name: 'remoteview.bitrateKbps', value: 3210 });
    const set = (await rpc.call('cvar.get', { name: 'remoteview.bitrateKbps' })).values[0].value;
    await rpc.call('cvar.set', { name: 'remoteview.bitrateKbps', value: before });
    expect(set === 3210, 'read back ' + set);
    return before + ' → 3210 → ' + before;
  });
  await check('console.run', () => rpc.call('console.run', { line: 'camera-frame scene' }));
  await check('a failing call fails', async () => {
    let message = null;
    try {
      await rpc.call('console.run', { line: 'no-such-command' });
    } catch (error) {
      message = error.message;
    }
    expect(message !== null, 'it answered');
    return message;
  });
  await check('the log arrives', async () => {
    for (let i = 0; i < 40 && !logTypes.has('RpcCallFailed'); i++) {
      await sleep(100);
    }
    expect(logTypes.has('RpcCallFailed'), 'no RpcCallFailed event among ' + logLines.length + ' lines');
    return logLines.length + ' lines';
  });
  const centre = padPoint(0.5, 0.5);
  const stepPixels = 100;
  for (const kind of ['touch', 'mouse']) {
    // Touch looks with one finger, the mouse with its right button, as the view does.
    const button = kind === 'mouse' ? 2 : 0;
    await check(kind + (kind === 'mouse' ? ' right drag turns' : ' drag turns'), async () => {
      const before = await pose();
      await drag(kind, centre, { x: centre.x + stepPixels, y: centre.y }, button);
      const after = await pose();
      const expected = stepPixels * lookDegreesPerPixel[kind];
      expect(Math.abs(after.yaw - before.yaw - expected) < 0.01, 'yaw changed by ' + (after.yaw - before.yaw) + ', expected ' + expected);
      await drag(kind, centre, { x: centre.x - stepPixels, y: centre.y }, button);
      return 'yaw +' + expected + '°';
    });
  }
  await check('mouse left drag turns nothing', async () => {
    const before = await pose();
    await drag('mouse', centre, { x: centre.x + stepPixels, y: centre.y }, 0);
    await input.lastPick;
    const after = await pose();
    expect(after.yaw === before.yaw && distance(before, after) === 0, 'the camera moved');
  });
  await check('touch pinch moves forward', async () => {
    const before = await pose();
    const a = padPoint(0.4, 0.5);
    const b = padPoint(0.6, 0.5);
    pointer('pointerdown', 'touch', 11, a);
    pointer('pointerdown', 'touch', 12, b);
    pointer('pointermove', 'touch', 11, { x: a.x - 40, y: a.y });
    pointer('pointermove', 'touch', 12, { x: b.x + 40, y: b.y });
    pointer('pointerup', 'touch', 11, { x: a.x - 40, y: a.y });
    pointer('pointerup', 'touch', 12, { x: b.x + 40, y: b.y });
    await settled();
    const after = await pose();
    const moved = distance(before, after);
    const expected = 80 * cameraSpeed() / 100;
    expect(Math.abs(moved - expected) < 0.01 && Math.abs(after.yaw - before.yaw) < 0.01, 'moved ' + moved + ' m, expected ' + expected);
    return moved.toFixed(2) + ' m';
  });
  const speedNow = async () => (await rpc.call('introspect.get', { name: 'status' })).camera.speed;
  await check('the wheel sets the fly speed', async () => {
    const before = await speedNow();
    pad.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
    await settled();
    const faster = await speedNow();
    pad.dispatchEvent(new WheelEvent('wheel', { deltaY: 100, bubbles: true, cancelable: true }));
    await settled();
    const back = await speedNow();
    expect(Math.abs(faster - before * 1.25) < 0.001 && Math.abs(back - before) < 0.001, 'speed ' + before + ' → ' + faster + ' → ' + back);
    return before + ' → ' + faster + ' → ' + back + ' m/s';
  });
  await check('Left Alt orbits', async () => {
    await rpc.call('view.camera.frame', { target: 'scene' });
    const key = type => pad.dispatchEvent(new KeyboardEvent(type, { code: 'AltLeft', key: 'Alt', altKey: type === 'keydown', bubbles: true, cancelable: true }));
    const orbit = async () => (await rpc.call('introspect.get', { name: 'status' })).cameraModes.orbit;
    pad.focus();
    pad.dispatchEvent(new PointerEvent('pointermove', { pointerId: 1, pointerType: 'mouse', clientX: centre.x, clientY: centre.y, bubbles: true }));
    key('keydown');
    await sleep(700);
    const held = await orbit();
    expect(held !== null && held.picked, 'not orbiting around a picked pivot: ' + JSON.stringify(held));
    const pivot = { x: held.pivot[0], y: held.pivot[1], z: held.pivot[2] };
    const before = await pose();
    await drag('mouse', centre, { x: centre.x + stepPixels, y: centre.y }, 2);
    const after = await pose();
    const turned = distance(before, after);
    expect(turned > 0.5 && Math.abs(distance(after, pivot) - distance(before, pivot)) < 0.01, 'moved ' + turned + ' m, pivot distance ' + distance(before, pivot) + ' → ' + distance(after, pivot));
    pad.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
    await settled();
    const nearer = await pose();
    expect(Math.abs(distance(nearer, pivot) - distance(after, pivot) * 0.85) < 0.01, 'the wheel did not move toward the pivot');
    await sleep(2500);
    expect((await orbit()) !== null, 'the orbit ended while the key was held');
    key('keyup');
    await sleep(400);
    expect((await orbit()) === null, 'still orbiting after the key came up');
    return 'around ' + held.pivot.map(value => value.toFixed(2)).join(', ') + ', ' + distance(before, pivot).toFixed(2) + ' m away';
  });
  await check('a held W flies', async () => {
    const before = await pose();
    pad.focus();
    pad.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyW', key: 'w', bubbles: true, cancelable: true }));
    await sleep(300);
    pad.dispatchEvent(new KeyboardEvent('keyup', { code: 'KeyW', key: 'w', bubbles: true }));
    await settled();
    const moved = distance(before, await pose());
    expect(moved > 0.2 * cameraSpeed() * 0.3 && moved < 3 * cameraSpeed() * 0.3, 'moved ' + moved + ' m in 0.3 s at ' + cameraSpeed() + ' m/s');
    return moved.toFixed(2) + ' m';
  });
  await check('a key in a field flies nothing', async () => {
    const before = await pose();
    showTab('console');
    const line = document.getElementById('consoleInput');
    line.focus();
    line.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyW', key: 'w', bubbles: true, cancelable: true }));
    await sleep(200);
    line.dispatchEvent(new KeyboardEvent('keyup', { code: 'KeyW', key: 'w', bubbles: true }));
    await settled();
    const moved = distance(before, await pose());
    line.blur();
    expect(moved === 0, 'moved ' + moved + ' m');
  });
  if (document.body.dataset.layout === 'wide') {
    await check('panels dock, float and are remembered', async () => {
      const before = JSON.stringify(dock);
      const panel = panelElements.cvars;
      const fits = () => document.documentElement.scrollWidth <= window.innerWidth && document.documentElement.scrollHeight <= window.innerHeight;
      for (const area of ['left', 'bottom', 'right']) {
        dockMove('cvars', area);
        expect(panel.parentElement === dockElement(area).querySelector('.dockbody'), 'cvars is not in the ' + area + ' area');
        expect(panelShown('cvars') && panel.offsetWidth > 100 && panel.offsetHeight > 60, 'cvars is not shown in the ' + area + ' area');
        expect(fits(), 'the page overflows with cvars docked ' + area);
      }
      dockMove('cvars', 'float', { x: 500, y: 300 });
      expect(panel.closest('.float') !== null && panelShown('cvars') && panel.offsetWidth > 100, 'cvars does not float');
      // A tab carried by the pointer to the left edge, and the window carried to the bottom one.
      const tab = document.querySelector('.docktabs button[data-tab="records"]');
      const from = tab.getBoundingClientRect();
      const press = (target, type, x, y) => target.dispatchEvent(new PointerEvent(type, { pointerId: 1, pointerType: 'mouse', clientX: x, clientY: y, button: 0, bubbles: true, cancelable: true }));
      press(tab, 'pointerdown', from.left + 5, from.top + 5);
      press(tab, 'pointermove', 300, 400);
      expect(document.body.classList.contains('dragging'), 'the drop zones did not show');
      press(tab, 'pointermove', 20, 400);
      press(tab, 'pointerup', 20, 400);
      expect(dock.left.panels.includes('records') && panelShown('records'), 'records did not dock left by its tab');
      const title = document.querySelector('.float[data-float="cvars"] .floattitle');
      const at = title.getBoundingClientRect();
      press(title, 'pointerdown', at.left + 20, at.top + 5);
      press(title, 'pointermove', 700, 500);
      press(title, 'pointermove', 700, window.innerHeight - 20);
      press(title, 'pointerup', 700, window.innerHeight - 20);
      expect(dock.bottom.panels.includes('cvars') && dock.floats.length === 0, 'the cvars window did not dock under the video');
      expect(JSON.stringify(remember('dock', null)) === JSON.stringify(dock), 'the layout was not remembered');
      const moved = JSON.stringify(dock);
      dock = loadDock();
      applyDock();
      expect(JSON.stringify(dock) === moved && panel.parentElement === dockElement('bottom').querySelector('.dockbody'), 'the remembered layout does not come back');
      dock = JSON.parse(before);
      saveDock();
      applyDock();
      expect(fits(), 'the page overflows');
      return 'left, bottom, right, float, by tab, by title';
    });
  }
  for (const kind of ['touch', 'mouse']) {
    await check(kind + ' tap picks', async () => {
      await rpc.call('view.camera.frame', { target: 'scene' });
      await rpc.call('view.select', { prim: '' });
      await sleep(300);
      showTab('scene');
      const id = kind === 'touch' ? 11 : 1;
      const middle = padPoint(0.5, 0.5);
      pointer('pointerdown', kind, id, middle);
      pointer('pointerup', kind, id, middle);
      const picked = await input.lastPick;
      input.lastTap = null;
      expect(picked && picked.prim, 'nothing was picked at the centre');
      // On a phone on its side the panels are a drawer, closed while the video is tapped: opened to read the selection.
      const drawer = document.body.dataset.layout === 'landscape';
      document.body.classList.toggle('sheetup', drawer);
      await poll(false);
      await sleep(600);
      const shown = document.getElementById('sceneSelected').textContent;
      document.body.classList.remove('sheetup');
      expect(shown === picked.prim, 'the Scene panel shows "' + shown + '", picked ' + picked.prim);
      return picked.prim;
    });
  }
  await check('R selects the parent, Z deselects', async () => {
    const selected = async () => (await rpc.call('introspect.get', { name: 'status' })).selected;
    await rpc.call('view.select', { prim: '/World/Cube_1' });
    await poll(false);
    await sleep(600);
    pad.focus();
    const press = code => pad.dispatchEvent(new KeyboardEvent('keydown', { code: code, bubbles: true, cancelable: true }));
    press('KeyR');
    await sleep(300);
    const parent = await selected();
    expect(parent === '/World', 'R selected "' + parent + '"');
    press('KeyZ');
    await sleep(300);
    const none = await selected();
    expect(none === '', 'Z left "' + none + '" selected');
    return '/World/Cube_1 → /World → nothing';
  });
  await check('screenshot', async () => {
    const shot = await rpc.call('remoteview.screenshot');
    const response = await fetch(shot.url);
    const bytes = new Uint8Array(await response.arrayBuffer());
    expect(response.ok && bytes.length > 1000 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47, 'not a PNG: ' + response.status + ', ' + bytes.length + ' bytes');
    testShot = shot;
    return shot.width + '×' + shot.height + ', ' + bytes.length + ' bytes';
  });
  await check('a result over the limit fails and the channel stays', async () => {
    // The same picture inside the answer, as base64: larger than a message on the channel may be.
    const limit = engine.remoteview.control.messageLimit;
    let message = null;
    try {
      const inline = await rpc.call('view.screenshot', { path: testShot.path, inline: true });
      message = 'it fitted: ' + JSON.stringify(inline).length + ' bytes under ' + limit;
    } catch (error) {
      expect(error.message.includes('over the control channel'), error.message);
      message = error.message;
    }
    await rpc.call('rpc.ping');
    return message;
  });
  testReport.layout = document.body.dataset.layout;
  testReport.pointer = pointerKind();
  testReport.done = true;
}

// ---- start ----

async function start() {
  buildTabs();
  wireDock();
  buildScene();
  buildConsole();
  wireStrip();
  wirePad();
  applyLayout();
  window.addEventListener('resize', applyLayout);
  document.addEventListener('keydown', event => {
    // Escape leaves the full video; a backtick anywhere but in a field goes to the console.
    if (event.key === 'Escape') {
      document.body.classList.remove('fullvideo');
    }
  });
  try {
    await measureClock();
  } catch (error) {
  }
  if ('requestVideoFrameCallback' in video) {
    video.requestVideoFrameCallback(everyVideoFrame);
  }
  setInterval(report, 1000);
  setInterval(() => poll(false), 500);
  setInterval(() => { measureClock().catch(() => {}); }, 60000);
  requestAnimationFrame(everyAnimationFrame);
  connect();
}

start();
