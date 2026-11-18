'use strict';

// 即時影像：WebSocket /ws/live (訊息格式見 README「即時影像」)
const FRAME_MAGIC = 0x314d4246; // "FBM1"
const HEADER_SIZE = 20;
const BUFFER_US = 1_000_000;    // 保留約 1 秒的影格與偵測結果
const BOX_MAX_AGE_US = 1_000_000;
const KEYPOINT_MIN_VISIBILITY = 0.5;
// COCO 骨架連線 (關鍵點索引：0 鼻、1/2 眼、3/4 耳、5/6 肩、7/8 肘、9/10 腕、11/12 髖、13/14 膝、15/16 踝)
const SKELETON = [
  [5, 7], [7, 9], [6, 8], [8, 10], [5, 6], [5, 11], [6, 12], [11, 12],
  [11, 13], [13, 15], [12, 14], [14, 16], [0, 1], [0, 2], [1, 3], [2, 4],
];

const statusEl = document.getElementById('status');
const overlayEl = document.getElementById('overlay');
const metaEl = document.getElementById('meta');
const canvas = document.getElementById('view');
const ctx = canvas.getContext('2d');

const frames = new Map();     // seq -> { tCaptureUs, jpeg }（壓縮的原圖，省記憶體）
const detections = new Map(); // seq -> detection metadata
let displayed = null;         // { seq, tCaptureUs, bitmap }
let decoding = false;
let liveConnected = false;
let cameraConnected = true;
let backendReachable = true;
let framesShown = 0;

// ---------------------------------------------------------------
// 繪製：影格 + 「seq 小於等於該影格、且在 1 秒內」的最新偵測結果 (框與骨架來自同一筆)
// ---------------------------------------------------------------
function detectionFor(seq, tCaptureUs) {
  let best = null;
  for (const d of detections.values()) {
    if (d.seq <= seq && (!best || d.seq > best.seq)) best = d;
  }
  if (!best || tCaptureUs - best.tCaptureUs > BOX_MAX_AGE_US) return null;
  return best;
}

function render() {
  if (!displayed) return;
  const { bitmap, seq, tCaptureUs } = displayed;
  if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) {
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;
  }
  ctx.drawImage(bitmap, 0, 0);

  const det = detectionFor(seq, tCaptureUs);
  if (det && det.person && det.box) {
    const [x, y, w, h] = det.box;
    const color = det.motion ? '#e0321e' : '#1fae4b';
    const lw = Math.max(2, Math.round(canvas.width / 400));
    ctx.lineWidth = lw;
    ctx.strokeStyle = color;
    ctx.strokeRect(x, y, w, h);
    const label = `person ${det.confidence.toFixed(2)}`;
    const fontPx = Math.max(14, Math.round(canvas.width / 60));
    ctx.font = `${fontPx}px sans-serif`;
    const ty = Math.max(fontPx + 2, y - 6);
    ctx.fillStyle = color;
    ctx.fillRect(x - lw / 2, ty - fontPx - 2, ctx.measureText(label).width + 8, fontPx + 6);
    ctx.fillStyle = '#fff';
    ctx.fillText(label, x + 4 - lw / 2, ty);
    if (Array.isArray(det.keypoints)) drawSkeleton(det.keypoints, color, lw);
  }
}

// 只畫可見度夠高的點；連線兩端都夠高才畫
function drawSkeleton(kps, color, lw) {
  const visible = (k) => Array.isArray(k) && k[2] >= KEYPOINT_MIN_VISIBILITY;
  ctx.lineWidth = lw;
  ctx.strokeStyle = color;
  ctx.beginPath();
  for (const [a, b] of SKELETON) {
    if (!visible(kps[a]) || !visible(kps[b])) continue;
    ctx.moveTo(kps[a][0], kps[a][1]);
    ctx.lineTo(kps[b][0], kps[b][1]);
  }
  ctx.stroke();

  const r = Math.max(3, Math.round(canvas.width / 250));
  ctx.fillStyle = color;
  ctx.strokeStyle = '#fff';
  ctx.lineWidth = Math.max(1, lw / 2);
  for (const k of kps) {
    if (!visible(k)) continue;
    ctx.beginPath();
    ctx.arc(k[0], k[1], r, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
  }
}

function prune(newestUs) {
  for (const [seq, f] of frames) if (newestUs - f.tCaptureUs > BUFFER_US) frames.delete(seq);
  for (const [seq, d] of detections) if (newestUs - d.tCaptureUs > BUFFER_US) detections.delete(seq);
}

async function showFrame(seq) {
  const f = frames.get(seq);
  if (!f) return;
  decoding = true;
  try {
    const bitmap = await createImageBitmap(new Blob([f.jpeg], { type: 'image/jpeg' }));
    if (displayed && displayed.seq >= seq) {
      bitmap.close();
      return;
    }
    if (displayed) displayed.bitmap.close();
    displayed = { seq, tCaptureUs: f.tCaptureUs, bitmap };
    framesShown++;
    render();
  } catch (e) {
    // 單張影格解碼失敗：略過
  } finally {
    decoding = false;
  }
}

// ---------------------------------------------------------------
// WebSocket：收到影格立刻回 ack；解碼中收到的影格只存進緩衝，不排隊解碼
// ---------------------------------------------------------------
let ws = null;
let retryMs = 1000;

function connectLive() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws/live`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    liveConnected = true;
    retryMs = 1000;
    updateBanner();
  };

  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      let d;
      try { d = JSON.parse(ev.data); } catch (e) { return; }
      if (d.type !== 'detection') return;
      detections.set(d.seq, d);
      if (displayed && d.seq <= displayed.seq) render();
      return;
    }

    ws.send('ack');
    const buf = ev.data;
    if (buf.byteLength <= HEADER_SIZE) return;
    const view = new DataView(buf);
    if (view.getUint32(0, true) !== FRAME_MAGIC) return;
    const seq = Number(view.getBigUint64(4, true));
    const tCaptureUs = Number(view.getBigUint64(12, true));
    frames.set(seq, { tCaptureUs, jpeg: buf.slice(HEADER_SIZE) });
    prune(tCaptureUs);
    if (!decoding) showFrame(seq);
  };

  ws.onclose = () => {
    const wasConnected = liveConnected;
    liveConnected = false;
    ws = null;
    if (wasConnected) {
      // 服務重啟後 seq 會從 1 開始：清掉舊的緩衝
      frames.clear();
      detections.clear();
      if (displayed) { displayed.bitmap.close(); displayed = null; }
    }
    updateBanner();
    setTimeout(connectLive, retryMs);
    retryMs = Math.min(retryMs * 2, 5000);
  };

  ws.onerror = () => { /* onclose 會接著處理重連 */ };
}

// ---------------------------------------------------------------
// 狀態：/api/status 每秒輪詢
// ---------------------------------------------------------------
function updateBanner() {
  let msg = '';
  if (!backendReachable) msg = 'Backend unavailable — retrying...';
  else if (!cameraConnected) msg = 'Camera disconnected — reconnecting...';
  else if (!liveConnected) msg = 'Live view disconnected — reconnecting...';
  overlayEl.hidden = msg === '';
  overlayEl.textContent = msg;
  canvas.classList.toggle('stale', msg !== '');
}

async function refreshStatus() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) throw new Error('status fetch failed');
    const status = await response.json();
    backendReachable = true;
    cameraConnected = status.cameraConnected !== false;

    if (!cameraConnected) {
      statusEl.textContent = 'Camera disconnected';
      statusEl.className = 'alert';
    } else if (status.motionDetected) {
      statusEl.textContent = 'Motion detected';
      statusEl.className = 'alert';
    } else {
      statusEl.textContent = 'No motion';
      statusEl.className = 'ok';
    }
    const hz = typeof status.detectHz === 'number' ? status.detectHz.toFixed(1) : '-';
    metaEl.textContent = `seq ${status.seq ?? '-'} · detect ${hz} Hz · view ${fps.toFixed(0)} fps`;
  } catch (error) {
    backendReachable = false;
    statusEl.textContent = 'Backend unavailable';
    statusEl.className = 'alert';
  }
  updateBanner();
}

let fps = 0;
let lastShown = 0;
setInterval(() => {
  fps = framesShown - lastShown;
  lastShown = framesShown;
}, 1000);

connectLive();
setInterval(refreshStatus, 1000);
refreshStatus();
