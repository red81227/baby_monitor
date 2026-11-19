// 組裝各模組：輪詢排程、背景分頁暫停、疊圖開關、只看影像模式
import * as store from './store.js';
import { prefs } from './common.js';
import { connect, disconnect, refreshStatus, render } from './live.js';
import { refreshSituations, refreshAlerts } from './situations.js';
import './roi-editor.js';
import './sound.js';

const STATUS_MS = 1000;
const POLL_MS = 2000;

// ---------------------------------------------------------------
// 疊圖開關 (記在瀏覽器)
// ---------------------------------------------------------------
store.set('overlays', { ...store.get('overlays'), ...prefs.get('overlays', {}) });
for (const input of document.querySelectorAll('[data-overlay]')) {
  const key = input.dataset.overlay;
  input.checked = store.get('overlays')[key] !== false;
  input.addEventListener('change', () => {
    const next = { ...store.get('overlays'), [key]: input.checked };
    prefs.set('overlays', next);
    store.set('overlays', next);
  });
}

// ---------------------------------------------------------------
// 只看影像模式：Fullscreen API；不支援時 (iPhone Safari) 以 CSS 填滿視窗
// ---------------------------------------------------------------
const viewer = document.getElementById('viewer');
const focusBtn = document.getElementById('focus-toggle');
const exitFocusBtn = document.getElementById('exit-focus');

function setFocus(on) {
  document.body.classList.toggle('focus-mode', on);
  exitFocusBtn.hidden = !on;
  // 版面改變後 canvas 的顯示大小也變了，重畫控制點
  requestAnimationFrame(render);
}

async function enterFocus() {
  setFocus(true);
  if (viewer.requestFullscreen) {
    try { await viewer.requestFullscreen({ navigationUI: 'hide' }); } catch (e) { /* 改用 CSS 填滿 */ }
  }
}

async function exitFocus() {
  if (document.fullscreenElement) {
    try { await document.exitFullscreen(); } catch (e) { /* ignore */ }
  }
  setFocus(false);
}

focusBtn.addEventListener('click', enterFocus);
exitFocusBtn.addEventListener('click', exitFocus);
document.addEventListener('fullscreenchange', () => {
  if (!document.fullscreenElement && document.body.classList.contains('focus-mode')) setFocus(false);
});
document.addEventListener('keydown', (ev) => {
  // 沒有全螢幕 (CSS 模式) 時以 Esc 離開；ROI 編輯中的 Esc 交給編輯器
  if (ev.key === 'Escape' && document.body.classList.contains('focus-mode') && !document.fullscreenElement &&
      !document.getElementById('view').classList.contains('editing')) {
    exitFocus();
  }
});

// ---------------------------------------------------------------
// 輪詢：頁面不可見時暫停即時影像、狀態與規則；警報輪詢不停 (提示音與分頁標題)
// ---------------------------------------------------------------
let statusTimer = null;
let situationsTimer = null;

function startForeground() {
  connect();
  refreshStatus();
  refreshSituations();
  clearInterval(statusTimer);
  clearInterval(situationsTimer);
  statusTimer = setInterval(refreshStatus, STATUS_MS);
  situationsTimer = setInterval(refreshSituations, POLL_MS);
}

function stopForeground() {
  disconnect();
  clearInterval(statusTimer);
  clearInterval(situationsTimer);
  statusTimer = null;
  situationsTimer = null;
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopForeground();
  else {
    startForeground();
    refreshAlerts();
  }
});

if (document.hidden) stopForeground();
else startForeground();
refreshAlerts();
setInterval(refreshAlerts, POLL_MS);
