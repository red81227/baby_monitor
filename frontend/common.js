// 共用的 DOM 與格式化工具、標籤文字、瀏覽器偏好設定

export const SEVERITY_LABEL = { info: '資訊', warning: '警告', critical: '緊急' };
export const ROUTE_LABEL = { direct: '直接提醒', llm: 'LLM 判定', suppress: '壓制', patrol: 'VLM 巡檢', system: '系統' };
export const DELIVERY_LABEL = {
  sent: '已送出', suppressed: '被壓制', shadow: '影子模式', no_alert: '判定不需提醒',
  dropped: '未送出', pending: '判定中', suppressor: '壓制來源', logged: '紀錄',
};
export const LLM_STATUS_LABEL = {
  ok: '完成', timeout: '逾時', unavailable: '無法使用', bad_reply: '回覆格式錯誤', queue_full: '佇列已滿',
};
export const ROI_SOURCE_LABEL = { override: '網頁設定', rules: '規則檔', none: '未設定' };

export function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null) continue;
    if (k === 'class') node.className = v;
    else if (k === 'text') node.textContent = v;
    else node.setAttribute(k, v);
  }
  for (const c of children) if (c != null) node.append(c);
  return node;
}

export function fmtTime(ms) {
  if (!ms) return '-';
  const d = new Date(ms);
  const sameDay = d.toDateString() === new Date().toDateString();
  const time = d.toLocaleTimeString('zh-TW', { hour12: false });
  return sameDay ? time : `${d.toLocaleDateString('zh-TW')} ${time}`;
}

export function badge(text, cls) {
  return el('span', { class: `badge ${cls || ''}`, text });
}

export function fmtRoi(r) {
  return r ? `[${r.map((v) => v.toFixed(3)).join(', ')}]` : '未設定';
}

// 偏好設定：存在 localStorage；瀏覽器不允許儲存時只在本次頁面有效
const memory = new Map();
export const prefs = {
  get(key, fallback) {
    try {
      const v = localStorage.getItem(`bm.${key}`);
      if (v !== null) return JSON.parse(v);
    } catch (e) { /* 改用記憶體中的值 */ }
    return memory.has(key) ? memory.get(key) : fallback;
  },
  set(key, value) {
    memory.set(key, value);
    try { localStorage.setItem(`bm.${key}`, JSON.stringify(value)); } catch (e) { /* 只在本次頁面有效 */ }
  },
};
