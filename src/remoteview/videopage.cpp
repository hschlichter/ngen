#include "videopage.h"

namespace {

// The numbers the page shares with the test pattern (videopattern.h): 8 colour bars over the top half, 32 time cells over the bottom
// eighth.
constexpr std::string_view page = R"html(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ngen remote view</title>
<style>
html, body { margin: 0; height: 100%; background: #111; color: #ccc; font: 13px system-ui, sans-serif; }
video { display: block; width: 100%; height: calc(100% - 28px); object-fit: contain; background: #000; }
#status { height: 28px; line-height: 28px; padding: 0 10px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
</style>
</head>
<body>
<video id="video" muted playsinline autoplay></video>
<div id="status">starting</div>
<script>
const video = document.getElementById('video');
const statusLine = document.getElementById('status');
const canvas = document.createElement('canvas');
const context = canvas.getContext('2d', { willReadFrequently: true });
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

function showStatus() {
  let text = connectionState;
  if (video.videoWidth > 0) {
    text += ' · ' + video.videoWidth + '×' + video.videoHeight;
    text += ' · ' + Math.round(lastStats.framesPerSecond || 0) + ' fps';
  }
  if (lastLatency !== null) {
    text += ' · latency ' + Math.round(lastLatency) + ' ms';
  }
  statusLine.textContent = text;
}

function setState(state) {
  connectionState = state;
  showStatus();
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
  setState('connecting');
  const peer = new RTCPeerConnection();
  connection = peer;
  peer.ontrack = event => {
    video.srcObject = event.streams.length > 0 ? event.streams[0] : new MediaStream([event.track]);
    video.play().catch(() => {});
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

function everyFrame() {
  readLatency();
  video.requestVideoFrameCallback(everyFrame);
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
  showStatus();
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
      }),
    });
    const answer = await response.json();
    source = answer.source;
    // The engine dropped this connection (remoteview-disconnect, or it was restarted): the browser notices only seconds later.
    if (answer.watcher === 'another' && !replaced && session !== 0) {
      // One watcher at a time: the page opened last has it. This one stops, so the two don't take it from each other for ever.
      replaced = true;
      if (connection !== null) {
        connection.close();
      }
      setState('another page is watching; reload this one to watch here');
    } else if (answer.watcher === 'none' && connectionState === 'connected') {
      setState('dropped by the engine');
      retryLater();
    }
    if (answer.grab) {
      await sendFrame();
    }
  } catch (error) {
  }
}

async function start() {
  try {
    await measureClock();
  } catch (error) {
  }
  if ('requestVideoFrameCallback' in video) {
    video.requestVideoFrameCallback(everyFrame);
  }
  setInterval(report, 1000);
  setInterval(() => { measureClock().catch(() => {}); }, 60000);
  connect();
}

start();
</script>
</body>
</html>
)html";

} // namespace

auto videoWatchPage() -> std::string_view {
    return page;
}
