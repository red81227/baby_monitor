// 警報提示音：以 Web Audio 產生，不需要音檔。瀏覽器要求使用者操作後才能播放聲音，
// 因此由「啟用提示音」按鈕開始；偏好記在瀏覽器，重新載入後需要再點一下才能恢復。
// (系統通知 Notification 在區網 http 下無法使用，見 design.md D8)
import * as store from './store.js';
import { prefs } from './common.js';

const button = document.getElementById('sound-toggle');
let ctx = null;
let seen = null;  // 已看過的已送出警報 id；第一次載入時的既有警報不響

function ensureContext() {
  if (!ctx) {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return null;
    ctx = new AC();
  }
  if (ctx.state === 'suspended') ctx.resume();
  return ctx;
}

function running() {
  return ctx && ctx.state === 'running';
}

function updateButton() {
  const enabled = prefs.get('sound', false);
  if (!enabled) {
    button.textContent = '啟用提示音';
    button.className = 'btn';
  } else if (!running()) {
    button.textContent = '點一下恢復提示音';
    button.className = 'btn attention';
  } else {
    button.textContent = '提示音：開';
    button.className = 'btn on';
  }
}

function tone(freq, start, duration, gain = 0.25) {
  const osc = ctx.createOscillator();
  const g = ctx.createGain();
  osc.type = 'sine';
  osc.frequency.value = freq;
  g.gain.setValueAtTime(0, start);
  g.gain.linearRampToValueAtTime(gain, start + 0.02);
  g.gain.setValueAtTime(gain, start + duration - 0.03);
  g.gain.linearRampToValueAtTime(0, start + duration);
  osc.connect(g).connect(ctx.destination);
  osc.start(start);
  osc.stop(start + duration);
}

// critical：高低交替 3 次；其他：短響一次
export function play(severity) {
  if (!prefs.get('sound', false) || !running()) return;
  const t = ctx.currentTime + 0.02;
  if (severity === 'critical') {
    for (let i = 0; i < 3; i++) {
      tone(988, t + i * 0.6, 0.28);
      tone(659, t + i * 0.6 + 0.3, 0.28);
    }
  } else {
    tone(784, t, 0.25, 0.2);
  }
}

button.addEventListener('click', () => {
  const enabled = prefs.get('sound', false);
  if (enabled && running()) {
    prefs.set('sound', false);
  } else {
    prefs.set('sound', true);
    if (ensureContext()) ctx.resume().then(() => { updateButton(); play('info'); });
  }
  updateButton();
});

// 已啟用時，頁面上任何一次點擊或按鍵都能恢復 AudioContext
function resumeOnGesture() {
  if (prefs.get('sound', false) && !running() && ensureContext()) ctx.resume().then(updateButton);
}
document.addEventListener('pointerdown', resumeOnGesture, true);
document.addEventListener('keydown', resumeOnGesture, true);

// 新出現的已送出警報：依最高嚴重程度響一次
store.subscribe('alerts', (alerts) => {
  const sent = alerts.filter((a) => a.delivery === 'sent');
  if (seen === null) {
    seen = new Set(sent.map((a) => a.id));
    return;
  }
  const fresh = sent.filter((a) => !seen.has(a.id));
  for (const a of fresh) seen.add(a.id);
  if (fresh.length === 0) return;
  play(fresh.some((a) => a.severity === 'critical') ? 'critical' : 'other');
});

updateButton();
