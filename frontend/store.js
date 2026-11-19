// 極小的共用狀態：各模組以 set() 更新、以 subscribe() 接收變化。
// situations：最近一次 /api/situations 的內容 (生效的 ROI 只有這一個來源)
// alerts：最近一次 /api/alerts 的清單
// overlays：疊圖開關 { box, skeleton, roi }
const state = {
  situations: null,
  alerts: [],
  overlays: { box: true, skeleton: true, roi: true },
};
const listeners = new Map();

export function get(key) {
  return state[key];
}

export function set(key, value) {
  state[key] = value;
  for (const fn of listeners.get(key) || []) fn(value);
}

export function subscribe(key, fn) {
  if (!listeners.has(key)) listeners.set(key, new Set());
  listeners.get(key).add(fn);
  return () => listeners.get(key).delete(fn);
}
