# Proposal

## Why

整體藍圖見 `redesign-vision-architecture`。本變更實作其中的 Phase 1「影像骨架」：目前擷取、推論、畫框、編碼串在同一條執行緒，顯示 FPS 被推論綁住、處理跟不上時延遲會累積，攝影機一斷線整個程式就結束。後續的狀態估計、事件片段、音訊都需要一個「來源可替換、各段速率獨立、每張影格帶時間戳」的骨架，所以先做這一期。

## What Changes

- 影像來源改為 USB 攝影機 MJPG 擷取，並在本機模擬雙碼流：
  - 主碼流：攝影機輸出的 JPEG 原樣使用，不解碼、不重新編碼
  - 子碼流：解碼時直接縮小，只供分析使用
  - 每張影格帶遞增的 `seq` 與擷取當下的 monotonic 時間戳
- 擷取、分析、輸出拆開，各段只取最新影格（latest-wins），不累積延遲
- 新增移動分數（子碼流影格差分），用來動態調整人物偵測頻率：靜止時以下限頻率（預設 1 Hz）執行，有動靜時提高到上限頻率
- 新增本機即時影像端點：推送主碼流原圖與帶 `seq` 的偵測 metadata；沒有觀看者時不送
- 監視頁面改為顯示串流，人物框由前端依 metadata 畫上
- **BREAKING** `GET /api/frame.jpg` 改為回傳沒有畫框的主碼流原圖
- **BREAKING** 攝影機中斷時不再結束程式，改為在程式內重連；`/api/status` 新增攝影機連線狀態
- 在 systemd 下執行時送出 watchdog 通知，影像處理停滯時停止通知，讓 systemd 重啟服務
- 新增攝影機裝置、擷取解析度、偵測頻率上下限的環境變數

不在本期範圍：時序狀態估計器、環形緩衝與事件片段、DB、音訊、LLM、ROI 設定、亮度旗標、節點心跳的細節設計、RTSP / go2rtc。

## Capabilities

### New Capabilities
- `frame-source`: 攝影機擷取、主碼流 / 子碼流、影格序號與擷取時間戳、攝影機斷線重連
- `detection-scheduling`: latest-wins 處理、移動分數、人物偵測頻率的動態調整
- `live-view`: 本機即時影像端點，推送主碼流原圖與偵測 metadata

### Modified Capabilities
- `person-detection`: 由「每張影格推論」改為依排程對子碼流推論，人物框座標以主碼流為準
- `service-lifecycle`: 攝影機中斷改為重連而不結束；啟動錯誤訊息改為指出裝置路徑；新增 systemd watchdog 通知
- `web-dashboard`: 監視頁面改為顯示即時串流並由前端畫框，顯示攝影機中斷狀態
- `status-api`: 新增攝影機連線狀態、影格序號、擷取時間與偵測頻率欄位；`/api/frame.jpg` 改回傳沒有框的原圖
- `runtime-configuration`: 新增攝影機裝置、擷取解析度、偵測頻率上下限設定

## Impact

- 程式碼：`src/vision/VisionPipeline` 拆成擷取 / 排程 / 偵測多個元件；`MonitorState` 改以不可變快照共享；`src/main.cpp` 啟動與關閉流程調整；`src/controllers/` 新增即時影像端點
- 前端：`frontend/` 改為 canvas 顯示串流，不再輪詢 `/api/frame.jpg`
- API：`/api/frame.jpg` 不再有框（BREAKING）；`/api/status` 新增欄位（原欄位保留）
- 維運：README 與 systemd 範例更新（`Type=notify`、`WatchdogSec`、新環境變數、攝影機中斷不再以 1 結束）
- 依賴：不新增外部依賴；systemd 通知以 `NOTIFY_SOCKET` 直接實作
