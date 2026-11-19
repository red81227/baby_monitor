// 狀況規則、巡檢、觸發紀錄與警報橫幅 (規則唯讀；ROI 的編輯在 roi-editor.js)
import * as store from './store.js';
import {
  el, fmtTime, fmtRoi, badge, prefs,
  SEVERITY_LABEL, ROUTE_LABEL, DELIVERY_LABEL, LLM_STATUS_LABEL, ROI_SOURCE_LABEL,
} from './common.js';

const BANNER_MAX_AGE_MS = 10 * 60 * 1000;  // 警報橫幅顯示最近 10 分鐘內送出的警報
const CLIP_FPS = 10;

const bannerEl = document.getElementById('alert-banner');
const rulesInfoEl = document.getElementById('rules-info');
const rulesErrorEl = document.getElementById('rules-error');
const rulesWarningsEl = document.getElementById('rules-warnings');
const roiInfoEl = document.getElementById('roi-info');
const situationsBody = document.getElementById('situations-body');
const patrolsBody = document.getElementById('patrols-body');
const patrolStalledEl = document.getElementById('patrol-stalled');
const alertListEl = document.getElementById('alert-list');
const alertFilterEl = document.getElementById('alert-filter');
const detailEl = document.getElementById('detail');
const baseTitle = document.title;

let dismissed = new Set(prefs.get('dismissedAlerts', []));

// 「未經 LLM 判定」已用標籤表示，說明文字只留原因
function causeText(a) {
  const c = a.cause || '';
  return c.startsWith('未經 LLM 判定 ') ? c.slice('未經 LLM 判定 '.length) : c;
}

function llmTag(a) {
  if (a.route === 'patrol') return badge('LLM 判定', 'llm');
  if (a.route !== 'llm' || a.delivery === 'pending') return null;
  return a.viaLlm ? badge('LLM 判定', 'llm') : badge('未經 LLM 判定', 'no-llm');
}

// 窄螢幕時表格改成卡片式，每格以 data-label 顯示欄名
function td(label, attrs, ...children) {
  return el('td', { ...attrs, 'data-label': label }, ...children);
}

// ---------------------------------------------------------------
// 規則與 ROI
// ---------------------------------------------------------------
function renderSituations(s) {
  const loaded = s.loadedAtEpochMs ? `，載入於 ${fmtTime(s.loadedAtEpochMs)}` : '';
  let vlm = 'VLM 停用';
  if (s.vlm && s.vlm.enabled) {
    vlm = s.vlm.available ? `VLM 可用${s.vlm.model ? `（${s.vlm.model}）` : ''}` : `VLM 無法連線（${s.vlm.error}）`;
  }
  const roiSource = ROI_SOURCE_LABEL[s.bedRoiSource] || s.bedRoiSource;
  rulesInfoEl.textContent = `規則 version ${s.version}（${s.hash}）${loaded} · ROI：${roiSource} · ${vlm}`;

  rulesErrorEl.hidden = !s.error;
  rulesErrorEl.textContent = s.error ? `規則載入失敗，仍使用 version ${s.version}：${s.error}` : '';

  rulesWarningsEl.replaceChildren(...(s.warnings || []).map((w) => el('li', { text: w })));

  roiInfoEl.replaceChildren(...[
    el('span', { text: 'ROI：' }),
    badge(roiSource, `roi-source ${s.bedRoiSource}`),
    s.bedRoi ? el('span', { text: fmtRoi(s.bedRoi) }) : null,
    s.bedRoiSource === 'override' && s.fileBedRoi ? el('span', { class: 'muted', text: `（規則檔：${fmtRoi(s.fileBedRoi)}）` }) : null,
    s.bedRoi ? null : el('strong', { class: 'roi-missing', text: 'near_bed_side（靠近床邊）不會觸發，請設定 ROI' }),
  ].filter(Boolean));

  situationsBody.replaceChildren(...s.situations.map((x) => {
    const dangerous = x.severity === 'critical' && (x.shadow || !x.enabled);
    const name = td('狀況', {}, el('code', { text: x.id }));
    if (x.shadow) name.append(' ', badge(dangerous ? '影子模式：不會提醒' : '影子模式', dangerous ? 'danger' : 'shadow'));
    if (!x.enabled) name.append(' ', badge(dangerous ? '已停用：不會提醒' : '已停用', dangerous ? 'danger' : 'shadow'));
    let route = ROUTE_LABEL[x.route] || x.route;
    if (x.route === 'suppress') route += `（低於 ${SEVERITY_LABEL[x.suppressBelow] || x.suppressBelow}，延續 ${x.lingerS}s）`;
    return el('tr', { class: dangerous ? 'danger-row' : '' },
      name,
      td('嚴重程度', {}, badge(SEVERITY_LABEL[x.severity] || x.severity, x.severity)),
      td('路由', { text: route }),
      td('條件', { class: 'conds', text: x.when.join(' 且 ') }),
      td('目前', {}, x.active ? badge('成立', 'active') : el('span', { class: 'muted', text: '—' })));
  }));
}

function renderPatrols(list) {
  const patrols = list || [];
  // 巡檢停止：與 critical 影子模式同等醒目
  const stalled = patrols.filter((p) => p.enabled && !p.shadow && p.stalled);
  patrolStalledEl.hidden = stalled.length === 0;
  patrolStalledEl.textContent = stalled.map((p) =>
    `巡檢停止：${p.id}（${p.stallReason}）。最後成功 ${p.lastOkEpochMs ? fmtTime(p.lastOkEpochMs) : '尚未成功'}。` +
    `${p.severity === 'critical' ? '這個狀況目前無法偵測。' : ''}`).join('\n');

  if (patrols.length === 0) {
    patrolsBody.replaceChildren(el('tr', {}, el('td', { class: 'muted', colspan: '5', text: '沒有巡檢' })));
    return;
  }
  patrolsBody.replaceChildren(...patrols.map((p) => {
    const dangerous = p.severity === 'critical' && (p.shadow || !p.enabled || p.stalled);
    const name = td('巡檢', {}, el('code', { text: p.id }));
    if (p.stalled) name.append(' ', badge('巡檢停止', p.shadow ? 'shadow' : 'danger'));
    if (p.shadow) name.append(' ', badge(p.severity === 'critical' ? '影子模式：不會提醒' : '影子模式', p.severity === 'critical' ? 'danger' : 'shadow'));
    if (!p.enabled) name.append(' ', badge(p.severity === 'critical' ? '已停用：不會提醒' : '已停用', p.severity === 'critical' ? 'danger' : 'shadow'));
    if (p.paused) name.append(' ', badge('暫停（前提條件不成立）', 'shadow'));
    if (p.while.length) name.append(el('div', { class: 'conds muted', text: `前提：${p.while.join(' 且 ')}` }));

    let result;
    if (p.lastError && !p.stalled) result = el('span', { class: 'muted', text: `失敗：${p.lastError}` });
    else if (p.lastAlert === null) result = el('span', { class: 'muted', text: p.inFlight ? '判定中' : '—' });
    else {
      result = el('span', {}, p.stalled ? el('span', { class: 'muted', text: '停止前最後一次：' }) : null,
        p.lastAlert ? badge(`需要提醒 ${p.positives}/${p.confirm}`, 'active') : badge('正常', 'llm'), ' ', p.lastCause);
    }
    return el('tr', { class: dangerous ? 'danger-row' : '' },
      name,
      td('嚴重程度', {}, badge(SEVERITY_LABEL[p.severity] || p.severity, p.severity)),
      td('間隔', { text: `${p.everyS}s` + (p.confirm > 1 ? `，連續 ${p.confirm} 次` : '') }),
      td('最後成功', { text: p.lastOkEpochMs ? fmtTime(p.lastOkEpochMs) : '尚未成功' }),
      td('最近結果', {}, result));
  }));
}

// ---------------------------------------------------------------
// 警報橫幅與觸發紀錄
// ---------------------------------------------------------------
function renderBanner(alerts) {
  const now = Date.now();
  const recent = alerts.filter((a) => a.delivery === 'sent' && now - a.deliveredEpochMs < BANNER_MAX_AGE_MS &&
    !dismissed.has(a.id));
  if (recent.length === 0) {
    bannerEl.hidden = true;
    document.title = baseTitle;
    return;
  }
  // 優先顯示 critical，其次最新的
  const order = { critical: 2, warning: 1, info: 0 };
  recent.sort((x, y) => (order[y.severity] - order[x.severity]) || (y.deliveredEpochMs - x.deliveredEpochMs));
  const a = recent[0];
  bannerEl.className = `alert-banner ${a.severity}`;
  const dismiss = el('button', { class: 'dismiss', type: 'button', text: '知道了' });
  dismiss.onclick = () => {
    for (const r of recent) dismissed.add(r.id);
    prefs.set('dismissedAlerts', [...dismissed].slice(-200));
    renderBanner(alerts);
  };
  const more = recent.length > 1 ? el('span', { class: 'more', text: `另有 ${recent.length - 1} 筆` }) : null;
  bannerEl.replaceChildren(
    el('div', { class: 'banner-main' },
      el('strong', { text: `${SEVERITY_LABEL[a.severity] || a.severity}：${a.situation}` }),
      ' ', llmTag(a),
      el('div', { class: 'banner-cause', text: causeText(a) }),
      el('div', { class: 'meta', text: `${fmtTime(a.deliveredEpochMs)}` }, more)),
    dismiss);
  bannerEl.onclick = (ev) => { if (ev.target !== dismiss) openDetail(a); };
  bannerEl.hidden = false;
  document.title = `${a.severity === 'critical' ? '‼ ' : '! '}${a.situation} — ${baseTitle}`;
}

function renderAlertList(alerts) {
  const filter = alertFilterEl.value;
  const shown = filter === 'all' ? alerts : alerts.filter((a) => a.severity === filter);
  if (shown.length === 0) {
    const text = alerts.length === 0 ? '還沒有任何觸發' : `沒有${SEVERITY_LABEL[filter] || ''}等級的紀錄`;
    alertListEl.replaceChildren(el('li', { class: 'muted', text }));
    return;
  }
  alertListEl.replaceChildren(...shown.map((a) => {
    const li = el('li', { class: `alert-row ${a.delivery}`, tabindex: '0' },
      el('span', { class: 'time', text: fmtTime(a.epochMs) }),
      badge(SEVERITY_LABEL[a.severity] || a.severity, a.severity),
      el('code', { text: a.situation }),
      badge(DELIVERY_LABEL[a.delivery] || a.delivery, `delivery ${a.delivery}`),
      a.shadow ? badge('影子模式', 'shadow') : null,
      llmTag(a),
      el('span', { class: 'cause', text: causeText(a) || (a.suppressedBy ? `被 ${a.suppressedBy} 壓制` : '') }));
    li.onclick = () => openDetail(a);
    li.onkeydown = (ev) => { if (ev.key === 'Enter') openDetail(a); };
    return li;
  }));
}

alertFilterEl.value = prefs.get('alertFilter', 'all');
if (!alertFilterEl.value) alertFilterEl.value = 'all';
alertFilterEl.addEventListener('change', () => {
  prefs.set('alertFilter', alertFilterEl.value);
  renderAlertList(store.get('alerts'));
});

// ---------------------------------------------------------------
// 詳細資料與事件片段播放
// ---------------------------------------------------------------
let clipTimer = null;

function stopClip() {
  if (clipTimer) clearInterval(clipTimer);
  clipTimer = null;
}

async function playClip(clipId) {
  stopClip();
  const img = document.getElementById('clip-img');
  const info = document.getElementById('clip-info');
  img.removeAttribute('src');
  if (!clipId) {
    info.textContent = '沒有事件片段';
    return;
  }
  info.textContent = '載入片段中...';
  try {
    const res = await fetch(`/api/events/${encodeURIComponent(clipId)}`, { cache: 'no-store' });
    if (!res.ok) throw new Error(res.status === 404 ? '片段不存在（可能尚未產生或已被刪除）' : `HTTP ${res.status}`);
    const meta = await res.json();
    if (!meta.frames.length) {
      info.textContent = '片段中沒有影格（攝影機可能中斷）';
      return;
    }
    // 依擷取時間以約 CLIP_FPS 張/秒播放
    const frames = [];
    let lastUs = -Infinity;
    for (const f of meta.frames) {
      if (f.tCaptureUs - lastUs >= 1e6 / CLIP_FPS) { frames.push(f); lastUs = f.tCaptureUs; }
    }
    const t0 = meta.frames[0].tCaptureUs;
    let i = 0;
    const show = () => {
      const f = frames[i];
      img.src = `/api/events/${encodeURIComponent(clipId)}/${f.file}`;
      const rel = ((f.tCaptureUs - meta.triggerUs) / 1e6).toFixed(1);
      info.textContent = `${clipId} · 觸發 ${rel >= 0 ? '+' : ''}${rel}s · ${((f.tCaptureUs - t0) / 1e6).toFixed(1)}s / ${((meta.frames.at(-1).tCaptureUs - t0) / 1e6).toFixed(1)}s`;
      i = (i + 1) % frames.length;
    };
    show();
    clipTimer = setInterval(show, 1000 / CLIP_FPS);
  } catch (e) {
    info.textContent = `無法載入片段：${e.message}`;
  }
}

function openDetail(a) {
  document.getElementById('detail-title').textContent =
    `${SEVERITY_LABEL[a.severity] || a.severity}：${a.situation}（${DELIVERY_LABEL[a.delivery] || a.delivery}）`;
  document.getElementById('detail-cause').textContent = a.cause || '';

  const fields = [
    ['觸發時間', fmtTime(a.epochMs)],
    ['送出時間', a.deliveredEpochMs ? fmtTime(a.deliveredEpochMs) : '未送出'],
    ['路由', ROUTE_LABEL[a.route] || a.route],
    ['規則', `version ${a.rulesVersion}（${a.rulesHash}）`],
  ];
  if (a.suppressedBy) fields.push(['壓制來源', a.suppressedBy]);
  if (a.llm) {
    fields.push(['LLM 判定', `${LLM_STATUS_LABEL[a.llm.status] || a.llm.status}` +
      (a.llm.status === 'ok' ? `：${a.llm.alert ? '需要提醒' : '不需要提醒'}` : '') +
      (a.llm.latencyMs ? `（${(a.llm.latencyMs / 1000).toFixed(1)}s）` : '')]);
    if (a.llm.cause) fields.push(['LLM 原因', a.llm.cause]);
    if (a.llm.model) fields.push(['模型', a.llm.model]);
    if (a.llm.error) fields.push(['錯誤', a.llm.error]);
    if (a.llm.raw) fields.push(['原始回覆', a.llm.raw]);
  }
  const sig = Object.entries(a.signals || {})
    .map(([k, v]) => `${k}=${v === null ? '未知' : (typeof v === 'number' ? +v.toFixed(4) : v)}`).join(', ');
  if (a.route !== 'system') fields.push(['訊號', sig]);
  document.getElementById('detail-fields').replaceChildren(
    ...fields.flatMap(([k, v]) => [el('dt', { text: k }), el('dd', { text: v })]));

  // 片段區塊只在有片段時顯示 (系統紀錄沒有片段)
  document.querySelector('#detail .clip').hidden = a.route === 'system' && !a.clipId;
  if (!detailEl.open) detailEl.showModal();
  playClip(a.clipId);
}

detailEl.addEventListener('close', stopClip);

// ---------------------------------------------------------------
// 輪詢 (由 main.js 排程)
// ---------------------------------------------------------------
export async function refreshSituations() {
  try {
    const res = await fetch('/api/situations', { cache: 'no-store' });
    if (!res.ok) return;
    const s = await res.json();
    renderSituations(s);
    renderPatrols(s.patrols);
    store.set('situations', s);
  } catch (e) {
    // 後端無法連線：狀態列會顯示
  }
}

export async function refreshAlerts() {
  try {
    const res = await fetch('/api/alerts', { cache: 'no-store' });
    if (!res.ok) return;
    const { alerts } = await res.json();
    store.set('alerts', alerts);
    renderBanner(alerts);
    renderAlertList(alerts);
  } catch (e) {
    // 後端無法連線：狀態列會顯示
  }
}
