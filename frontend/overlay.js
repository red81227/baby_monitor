// 在即時影像上畫疊圖：人物框、骨架、ROI 與床邊判定範圍、ROI 編輯中的草稿
import * as store from './store.js';

const KEYPOINT_MIN_VISIBILITY = 0.5;
// COCO 骨架連線 (關鍵點索引：0 鼻、1/2 眼、3/4 耳、5/6 肩、7/8 肘、9/10 腕、11/12 髖、13/14 膝、15/16 踝)
const SKELETON = [
  [5, 7], [7, 9], [6, 8], [8, 10], [5, 6], [5, 11], [6, 12], [11, 12],
  [11, 13], [13, 15], [12, 14], [14, 16], [0, 1], [0, 2], [1, 3], [2, 4],
];
const ROI_COLOR = '#ffd34d';
const DRAFT_COLOR = '#4dc3ff';
export const HANDLE_CSS_PX = 12;  // 控制點命中半徑 (CSS px)

// 床邊判定範圍：與後端相同，四邊都內縮「區域寬度 x margin」(以畫面比例計)
export function innerRect([x, y, w, h], margin) {
  const d = w * margin;
  return [x + d, y + d, Math.max(0, w - 2 * d), Math.max(0, h - 2 * d)];
}

export function drawDetection(ctx, det) {
  const { box: showBox, skeleton: showSkeleton } = store.get('overlays');
  if (!det || !det.person || !det.box || (!showBox && !showSkeleton)) return;
  const { width } = ctx.canvas;
  const [x, y, w, h] = det.box;
  const color = det.motion ? '#e0321e' : '#1fae4b';
  const lw = Math.max(2, Math.round(width / 400));
  if (showBox) {
    ctx.lineWidth = lw;
    ctx.strokeStyle = color;
    ctx.strokeRect(x, y, w, h);
    const label = `person ${det.confidence.toFixed(2)}`;
    const fontPx = Math.max(14, Math.round(width / 60));
    ctx.font = `${fontPx}px sans-serif`;
    const ty = Math.max(fontPx + 2, y - 6);
    ctx.fillStyle = color;
    ctx.fillRect(x - lw / 2, ty - fontPx - 2, ctx.measureText(label).width + 8, fontPx + 6);
    ctx.fillStyle = '#fff';
    ctx.fillText(label, x + 4 - lw / 2, ty);
  }
  if (showSkeleton && Array.isArray(det.keypoints)) drawSkeleton(ctx, det.keypoints, color, lw);
}

// 只畫可見度夠高的點；連線兩端都夠高才畫
function drawSkeleton(ctx, kps, color, lw) {
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

  const r = Math.max(3, Math.round(ctx.canvas.width / 250));
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

function toPx(ctx, [x, y, w, h]) {
  const { width: W, height: H } = ctx.canvas;
  return [x * W, y * H, w * W, h * H];
}

// 生效的 ROI (實線) 與床邊判定範圍 (虛線)
export function drawRoi(ctx, roi, margin) {
  if (!roi) return;
  const lw = Math.max(2, Math.round(ctx.canvas.width / 500));
  ctx.save();
  ctx.lineWidth = lw;
  ctx.strokeStyle = ROI_COLOR;
  ctx.strokeRect(...toPx(ctx, roi));
  if (margin > 0) {
    ctx.setLineDash([lw * 4, lw * 3]);
    ctx.globalAlpha = 0.8;
    ctx.strokeRect(...toPx(ctx, innerRect(roi, margin)));
  }
  ctx.restore();
}

// 編輯中的草稿：ROI 外調暗、八個控制點；cssScale = canvas px / CSS px
export function drawDraft(ctx, roi, margin, cssScale) {
  const { width: W, height: H } = ctx.canvas;
  const [x, y, w, h] = toPx(ctx, roi);
  ctx.save();
  ctx.fillStyle = 'rgba(0, 0, 0, 0.45)';
  ctx.beginPath();
  ctx.rect(0, 0, W, H);
  ctx.rect(x, y, w, h);
  ctx.fill('evenodd');

  const lw = Math.max(2, Math.round(W / 500));
  ctx.lineWidth = lw;
  ctx.strokeStyle = DRAFT_COLOR;
  ctx.strokeRect(x, y, w, h);
  if (margin > 0) {
    ctx.setLineDash([lw * 4, lw * 3]);
    ctx.strokeRect(...toPx(ctx, innerRect(roi, margin)));
    ctx.setLineDash([]);
  }
  const r = (HANDLE_CSS_PX * cssScale) / 1.6;
  ctx.fillStyle = '#fff';
  for (const [hx, hy] of handlePoints([x, y, w, h])) {
    ctx.beginPath();
    ctx.arc(hx, hy, r, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
  }
  ctx.restore();
}

// 八個控制點，順序：nw n ne e se s sw w
export const HANDLES = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'];
export function handlePoints([x, y, w, h]) {
  const cx = x + w / 2;
  const cy = y + h / 2;
  return [[x, y], [cx, y], [x + w, y], [x + w, cy], [x + w, y + h], [cx, y + h], [x, y + h], [x, cy]];
}
