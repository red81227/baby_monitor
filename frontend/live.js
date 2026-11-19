// 即時影像：WebSocket /ws/live (訊息格式見 README「即時影像」) 與 /api/status 狀態列
import * as store from './store.js';
import { drawDetection, drawRoi } from './overlay.js';

const FRAME_MAGIC = 0x314d4246; // "FBM1"
const HEADER_SIZE = 20;
const BUFFER_US = 1_000_000;    // 保留約 1 秒的影格與偵測結果
const BOX_MAX_AGE_US = 1_000_000;

const statusEl = document.getElementById('status');
const overlayEl = document.getElementById('overlay');
const metaEl = document.getElementById('meta');
export const canvas = document.getElementById('view');
const ctx = canvas.getContext('2d');

const frames = new Map();     // seq -> { tCaptureUs, jpeg }（壓縮的原圖，省記憶體）
const detections = new Map(); // seq -> detection metadata
let displayed = null;         // { seq, tCaptureUs, bitmap }
let decoding = false;
let liveConnected = false;
let cameraConnected = true;
let backendReachable = true;
let framesShown = 0;
let fps = 0;

// ROI 編輯器在影像上畫草稿：設定後取代生效 ROI 的繪製
let draftRenderer = null;
export function setDraftRenderer(fn) {
  draftRenderer = fn;
  render();
}

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

export function render() {
  if (displayed) {
    const { bitmap, seq, tCaptureUs } = displayed;
    if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) {
      canvas.width = bitmap.width;
      canvas.height = bitmap.height;
      // 顯示比例跟著攝影機 (CSS 用 --aspect 保持比例，pointer 換算才不會偏移)
      canvas.parentElement.style.setProperty('--aspect', `${bitmap.width} / ${bitmap.height}`);
      canvas.parentElement.style.setProperty('--aspect-num', String(bitmap.width / bitmap.height));
    }
    ctx.drawImage(bitmap, 0, 0);
    drawDetection(ctx, detectionFor(seq, tCaptureUs));
  } else if (draftRenderer) {
    // 還沒有畫面時仍可編輯 ROI
    ctx.fillStyle = '#2b2a27';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  } else {
    return;
  }

  if (draftRenderer) {
    draftRenderer(ctx);
    return;
  }
  const s = store.get('situations');
  if (s && store.get('overlays').roi) drawRoi(ctx, s.bedRoi, s.bedEdgeMargin);
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
let retryTimer = null;
let wanted = false;   // 頁面可見時才連線

export function connect() {
  wanted = true;
  if (ws || retryTimer) return;
  open();
}

export function disconnect() {
  wanted = false;
  clearTimeout(retryTimer);
  retryTimer = null;
  if (ws) ws.close();
}

function clearBuffers() {
  // 服務重啟後 seq 會從 1 開始：清掉舊的緩衝
  frames.clear();
  detections.clear();
  if (displayed) { displayed.bitmap.close(); displayed = null; }
}

function open() {
  retryTimer = null;
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const sock = new WebSocket(`${proto}://${location.host}/ws/live`);
  ws = sock;
  sock.binaryType = 'arraybuffer';

  sock.onopen = () => {
    liveConnected = true;
    retryMs = 1000;
    updateOverlay();
  };

  sock.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      let d;
      try { d = JSON.parse(ev.data); } catch (e) { return; }
      if (d.type !== 'detection') return;
      detections.set(d.seq, d);
      if (displayed && d.seq <= displayed.seq) render();
      return;
    }

    sock.send('ack');
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

  sock.onclose = () => {
    const wasConnected = liveConnected;
    liveConnected = false;
    ws = null;
    if (wasConnected) clearBuffers();
    updateOverlay();
    if (!wanted) return;
    retryTimer = setTimeout(open, retryMs);
    retryMs = Math.min(retryMs * 2, 5000);
  };

  sock.onerror = () => { /* onclose 會接著處理重連 */ };
}

// ---------------------------------------------------------------
// 狀態：/api/status
// ---------------------------------------------------------------
function updateOverlay() {
  let msg = '';
  if (!backendReachable) msg = '後端無法連線，重試中…';
  else if (!cameraConnected) msg = '攝影機中斷，重新連線中…';
  else if (wanted && !liveConnected) msg = '即時影像中斷，重新連線中…';
  overlayEl.hidden = msg === '';
  overlayEl.textContent = msg;
  canvas.classList.toggle('stale', msg !== '');
}

export async function refreshStatus() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) throw new Error('status fetch failed');
    const status = await response.json();
    backendReachable = true;
    cameraConnected = status.cameraConnected !== false;

    if (!cameraConnected) {
      statusEl.textContent = '攝影機中斷';
      statusEl.className = 'alert';
    } else if (status.motionDetected) {
      statusEl.textContent = '有移動';
      statusEl.className = 'alert';
    } else {
      statusEl.textContent = '沒有移動';
      statusEl.className = 'ok';
    }
    const hz = typeof status.detectHz === 'number' ? status.detectHz.toFixed(1) : '-';
    metaEl.textContent = `seq ${status.seq ?? '-'} · 偵測 ${hz} 次/秒 · 顯示 ${fps.toFixed(0)} fps`;
  } catch (error) {
    backendReachable = false;
    statusEl.textContent = '後端無法連線';
    statusEl.className = 'alert';
  }
  updateOverlay();
}

let lastShown = 0;
setInterval(() => {
  fps = framesShown - lastShown;
  lastShown = framesShown;
}, 1000);

// 生效的 ROI 或疊圖開關改變：重畫目前的影格
store.subscribe('situations', () => render());
store.subscribe('overlays', () => render());
