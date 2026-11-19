// ROI 編輯：在即時影像上以滑鼠或觸控拖拉建立 / 移動 / 調整 ROI，儲存到 PUT /api/roi，
// 或以 DELETE /api/roi 還原為規則檔。座標一律以畫面比例 (0–1) 保存，只在繪製與處理 pointer 時換算。
import * as store from './store.js';
import { canvas, render, setDraftRenderer } from './live.js';
import { drawDraft, handlePoints, HANDLES, HANDLE_CSS_PX } from './overlay.js';
import { fmtRoi } from './common.js';
import { refreshSituations } from './situations.js';

const MIN = 0.05;  // 寬高下限 (與後端相同)

const editBtn = document.getElementById('roi-edit');
const resetBtn = document.getElementById('roi-reset');
const toolbar = document.getElementById('roi-toolbar');
const saveBtn = document.getElementById('roi-save');
const cancelBtn = document.getElementById('roi-cancel');
const hintEl = document.getElementById('roi-hint');
const messageEl = document.getElementById('roi-message');

let editing = false;
let draft = null;      // [x, y, w, h]
let baseHash = '';     // 開始編輯時讀到的規則雜湊值 (If-Match)
let drag = null;       // { mode: 'create'|'move'|'resize', handle, anchor, start, startRoi, pointerId }
let saving = false;
let messageTimer = null;

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

function showMessage(text, kind = 'info', autoHideMs = 0) {
  clearTimeout(messageTimer);
  messageEl.textContent = text;
  messageEl.className = `roi-message ${kind}`;
  messageEl.hidden = !text;
  if (autoHideMs) messageTimer = setTimeout(() => { messageEl.hidden = true; }, autoHideMs);
}

// ---------------------------------------------------------------
// 座標換算與命中測試
// ---------------------------------------------------------------
function pointOf(ev) {
  const r = canvas.getBoundingClientRect();
  return { x: clamp((ev.clientX - r.left) / r.width, 0, 1), y: clamp((ev.clientY - r.top) / r.height, 0, 1) };
}

// 回傳 { mode, handle }：控制點 -> resize，矩形內 -> move，其他 -> create
function hitTest(ev) {
  if (!draft) return { mode: 'create' };
  const r = canvas.getBoundingClientRect();
  const px = ev.clientX - r.left;
  const py = ev.clientY - r.top;
  const [x, y, w, h] = draft;
  const pts = handlePoints([x * r.width, y * r.height, w * r.width, h * r.height]);
  // 觸控的命中範圍放大
  const radius = ev.pointerType === 'touch' ? HANDLE_CSS_PX * 2 : HANDLE_CSS_PX;
  let best = null;
  pts.forEach(([hx, hy], i) => {
    const d = Math.hypot(px - hx, py - hy);
    if (d <= radius && (!best || d < best.d)) best = { d, handle: HANDLES[i] };
  });
  if (best) return { mode: 'resize', handle: best.handle };
  const p = pointOf(ev);
  if (p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h) return { mode: 'move' };
  return { mode: 'create' };
}

const CURSOR = { nw: 'nwse-resize', se: 'nwse-resize', ne: 'nesw-resize', sw: 'nesw-resize',
  n: 'ns-resize', s: 'ns-resize', e: 'ew-resize', w: 'ew-resize' };

// 一維範圍：由錨點拖到 p，長度至少 MIN，且在 0–1 內
function span(a, p) {
  let lo = Math.min(a, p);
  let hi = Math.max(a, p);
  if (hi - lo < MIN) {
    if (p >= a) hi = lo + MIN; else lo = hi - MIN;
  }
  if (lo < 0) { hi -= lo; lo = 0; }
  if (hi > 1) { lo -= hi - 1; hi = 1; }
  return [lo, hi];
}

function applyDrag(p) {
  if (drag.mode === 'create') {
    const [x0, x1] = span(drag.anchor.x, p.x);
    const [y0, y1] = span(drag.anchor.y, p.y);
    draft = [x0, y0, x1 - x0, y1 - y0];
  } else if (drag.mode === 'move') {
    const [x, y, w, h] = drag.startRoi;
    draft = [clamp(x + p.x - drag.start.x, 0, 1 - w), clamp(y + p.y - drag.start.y, 0, 1 - h), w, h];
  } else {
    const [x, y, w, h] = drag.startRoi;
    let left = x;
    let right = x + w;
    let top = y;
    let bottom = y + h;
    if (drag.handle.includes('w')) left = clamp(p.x, 0, right - MIN);
    if (drag.handle.includes('e')) right = clamp(p.x, left + MIN, 1);
    if (drag.handle.includes('n')) top = clamp(p.y, 0, bottom - MIN);
    if (drag.handle.includes('s')) bottom = clamp(p.y, top + MIN, 1);
    draft = [left, top, right - left, bottom - top];
  }
}

// ---------------------------------------------------------------
// Pointer 事件 (滑鼠與觸控共用)
// ---------------------------------------------------------------
canvas.addEventListener('pointerdown', (ev) => {
  if (!editing || saving || (ev.pointerType === 'mouse' && ev.button !== 0)) return;
  ev.preventDefault();
  const hit = hitTest(ev);
  const p = pointOf(ev);
  drag = { ...hit, anchor: p, start: p, startRoi: draft, pointerId: ev.pointerId };
  canvas.setPointerCapture(ev.pointerId);
  if (hit.mode === 'create') applyDrag(p);
  updateToolbar();
  render();
});

canvas.addEventListener('pointermove', (ev) => {
  if (!editing) return;
  if (!drag) {
    const hit = hitTest(ev);
    canvas.style.cursor = hit.mode === 'resize' ? CURSOR[hit.handle] : hit.mode === 'move' ? 'move' : 'crosshair';
    return;
  }
  if (ev.pointerId !== drag.pointerId) return;
  ev.preventDefault();
  applyDrag(pointOf(ev));
  render();
});

function endDrag(ev) {
  if (!drag || ev.pointerId !== drag.pointerId) return;
  drag = null;
  updateToolbar();
  render();
}
canvas.addEventListener('pointerup', endDrag);
canvas.addEventListener('pointercancel', endDrag);

// ---------------------------------------------------------------
// 編輯模式
// ---------------------------------------------------------------
function drawEditor(ctx) {
  const s = store.get('situations');
  const scale = canvas.width / (canvas.getBoundingClientRect().width || canvas.width);
  if (draft) {
    drawDraft(ctx, draft, s ? s.bedEdgeMargin : 0, scale);
  } else {
    ctx.fillStyle = 'rgba(0, 0, 0, 0.45)';
    ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);
  }
}

function updateToolbar() {
  toolbar.hidden = !editing;
  saveBtn.disabled = !draft || saving;
  cancelBtn.disabled = saving;
  hintEl.textContent = draft
    ? `${fmtRoi(draft)} · 拖拉內部移動、拖拉控制點調整大小；Enter 儲存、Esc 取消`
    : '在畫面上拖拉，框出嬰兒床';
  const s = store.get('situations');
  editBtn.hidden = editing;
  editBtn.disabled = !s;
  resetBtn.hidden = editing || !s || s.bedRoiSource !== 'override';
}

function startEditing() {
  const s = store.get('situations');
  if (!s) return;
  editing = true;
  draft = s.bedRoi ? [...s.bedRoi] : null;
  baseHash = s.hash;
  showMessage('');
  canvas.classList.add('editing');
  canvas.style.touchAction = 'none';  // 只在編輯時阻止手機捲動
  setDraftRenderer(drawEditor);
  updateToolbar();
}

function stopEditing() {
  editing = false;
  draft = null;
  drag = null;
  canvas.classList.remove('editing');
  canvas.style.touchAction = '';
  canvas.style.cursor = '';
  setDraftRenderer(null);
  updateToolbar();
}

// 以寫入端點的回應更新生效的 ROI (不等下一次輪詢)
function applyResponse(body) {
  const s = store.get('situations');
  if (!s || !body || !('bedRoiSource' in body)) return;
  store.set('situations', {
    ...s,
    bedRoi: body.bedRoi,
    bedRoiSource: body.bedRoiSource,
    fileBedRoi: body.fileBedRoi,
    bedEdgeMargin: body.bedEdgeMargin,
    hash: body.hash,
    version: body.version,
  });
}

async function send(method, ifMatch, payload) {
  const res = await fetch('/api/roi', {
    method,
    headers: { 'Content-Type': 'application/json', 'If-Match': ifMatch },
    body: payload ? JSON.stringify(payload) : undefined,
    cache: 'no-store',
  });
  let body = null;
  try { body = await res.json(); } catch (e) { /* 非 JSON 回應 */ }
  return { status: res.status, body };
}

async function save() {
  if (!draft || saving) return;
  saving = true;
  updateToolbar();
  showMessage('儲存中…');
  try {
    const { status, body } = await send('PUT', baseHash, { roi: draft.map((v) => Math.round(v * 1000) / 1000) });
    if (status === 200) {
      applyResponse(body);
      stopEditing();
      showMessage(`ROI 已儲存並生效：${fmtRoi(body.bedRoi)}`, 'ok', 6000);
    } else if (status === 409) {
      // 別人先改了 (或規則載入失敗)：顯示目前的 ROI，草稿不套用
      applyResponse(body);
      stopEditing();
      showMessage(`未儲存：${body && body.error ? body.error : 'ROI 已被其他人修改'}。畫面上是目前的 ROI，請重新編輯。`, 'error');
    } else {
      showMessage(`未儲存：${body && body.error ? body.error : `HTTP ${status}`}`, 'error');
    }
  } catch (e) {
    showMessage('未儲存：後端無法連線', 'error');
  } finally {
    saving = false;
    updateToolbar();
    refreshSituations();
  }
}

async function reset() {
  const s = store.get('situations');
  if (!s || s.bedRoiSource !== 'override') return;
  const after = s.fileBedRoi
    ? `還原後 ROI 會是規則檔的 ${fmtRoi(s.fileBedRoi)}。`
    : '規則檔沒有設定 bed_roi，還原後沒有 ROI，near_bed_side（靠近床邊）不會觸發。';
  if (!confirm(`還原為規則檔？\n${after}`)) return;
  try {
    const { status, body } = await send('DELETE', s.hash);
    if (status === 200) {
      applyResponse(body);
      showMessage(`已還原為規則檔：${fmtRoi(body.bedRoi)}`, 'ok', 6000);
    } else {
      applyResponse(body);
      showMessage(`未還原：${body && body.error ? body.error : `HTTP ${status}`}`, 'error');
    }
  } catch (e) {
    showMessage('未還原：後端無法連線', 'error');
  }
  refreshSituations();
}

editBtn.addEventListener('click', startEditing);
cancelBtn.addEventListener('click', () => { stopEditing(); showMessage(''); });
saveBtn.addEventListener('click', save);
resetBtn.addEventListener('click', reset);

document.addEventListener('keydown', (ev) => {
  if (!editing || document.querySelector('dialog[open]')) return;
  if (ev.key === 'Escape') {
    ev.preventDefault();
    stopEditing();
    showMessage('');
  } else if (ev.key === 'Enter' && !(ev.target instanceof HTMLButtonElement)) {
    ev.preventDefault();
    save();
  }
});

store.subscribe('situations', updateToolbar);
updateToolbar();
