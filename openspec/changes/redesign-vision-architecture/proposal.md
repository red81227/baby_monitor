# Proposal

## Why

目前的影像管線是一條單執行緒串行流程（擷取 → YOLO → 畫框 → JPEG 編碼），顯示 FPS 被推論速度綁住、處理跟不上時延遲會累積，前端以每 250ms 輪詢單張 JPEG 顯示，輸出也只有逐影格的「有沒有移動」布林值。要往「能判斷寶寶狀態、留下事件片段、之後接專業攝影機」的方向發展，需要先定下一個可分期擴充的整體架構，避免每加一個功能就重寫一次管線。

## What Changes

本變更先定義整體目標架構與分期，Phase 1 之後再依此拆成可實作的 specs / tasks。

- 引入可替換的影像來源介面 `FrameSource`；Phase 1 以 USB 攝影機（V4L2 MJPG）在本機模擬主碼流 / 子碼流，之後可換成 RTSP 攝影機
  - 主碼流：MJPEG 原樣轉發（不解碼、不重新編碼），供本機直播、環形緩衝、事件片段
  - 子碼流：解碼時直接縮小，供分析使用
- 分析改為「latest-wins」：每一段只取最新影格，跟不上就跳過，不累積延遲
- 新增便宜的移動分數（ROI 影格差分），用來動態調整 YOLO 頻率（1 fps 基準，有動靜時提高）
- 新增時間對齊層：程式內統一使用 `CLOCK_MONOTONIC`，所有觀測值帶擷取時間戳，融合時以 watermark 處理延遲與亂序
- 新增時序狀態估計器，取代逐影格布林判定（ABSENT / SLEEPING / STIRRING / AWAKE / CRYING）
- 新增環形緩衝與事件片段、狀態紀錄 DB、規則 / 警報管理器、本機小 LLM 旁路、資料收集（分期導入）
- 人物框改由前端依 metadata 畫在 canvas 上，影像保持乾淨 **BREAKING**（`/api/frame.jpg` 不再保證有框，Phase 1 時決定是否保留相容）
- 攝影機中斷改為程式內重連，並加入心跳 / watchdog **BREAKING**（攝影機中斷不再立即以 1 結束）
- 範圍限制：只在本機觀看、不連網、沒有遠端推播；對外明確說明本系統不是醫療設備

### 分期

| 期 | 內容 |
|---|---|
| Phase 1 影像骨架 | `FrameSource`（USB 雙碼流模擬）、latest-wins、移動分數 → 動態 YOLO 頻率、時間戳、本機直播（MJPEG 原樣轉發 + metadata）、攝影機重連 + systemd WatchdogSec |
| Phase 2 狀態與事件 | 時序狀態估計器（先只用影像）、環形緩衝 + 事件片段、狀態紀錄 DB、評估用重播測試 |
| Phase 3 警報 | 規則 / 警報管理器、本機通知、使用者回饋 |
| Phase 4 擴充 | 哭聲偵測（接入時間對齊層）、pose、本機小 LLM 旁路、資料收集 |
| 未來 | RTSP 攝影機 + go2rtc / WebRTC 直播 |

### 不在範圍內 / 待討論

- 連網、遠端觀看、認證、TURN（目前限定本機）
- 以合成影像預訓練 / fine-tune 嬰兒模型（待討論）
- 評估資料集與重播測試的細節（待討論）
- 心跳系統的細節設計（待討論）

## Capabilities

本變更目前只定下架構與分期；以下為預期的 capability，實際的 spec 內容在 Phase 1 拆分時撰寫。

### New Capabilities
- `frame-source`: 影像來源抽象、主碼流 / 子碼流、擷取時間戳、攝影機重連
- `live-view`: 本機即時畫面（主碼流原樣轉發）與偵測 metadata 的推送
- `detection-scheduling`: 移動分數與 YOLO 頻率的動態調整、latest-wins 行為
- `time-alignment`: 單一時鐘、觀測值時間戳、watermark 融合規則
- `baby-state`: 時序狀態估計器的狀態與轉換（Phase 2）
- `event-recording`: 環形緩衝與事件片段（Phase 2）
- `alerting`: 規則、警報管理器與使用者回饋（Phase 3）
- `health-watchdog`: 節點心跳與異常回報（細節待討論）

### Modified Capabilities
- `motion-detection`: 由人物中心位移的逐影格判定，改為移動分數 + 時序狀態
- `web-dashboard`: 改為 canvas 顯示主碼流並由前端畫框；更新頻率要求調整
- `status-api`: 狀態欄位改為時序狀態；`/api/frame.jpg` 的框標示要求調整
- `service-lifecycle`: 攝影機中斷改為程式內重連，加入 watchdog
- `person-detection`: 偵測改為由排程器以動態頻率呼叫，輸入為子碼流

## Impact

- 程式碼：`src/vision/VisionPipeline` 拆成擷取 / 分析 / 輸出多個節點；新增 `FrameSource`、時間對齊、狀態估計等模組；`MonitorState` 改為以不可變快照（`shared_ptr<const ...>`）共享
- API：`/api/status` 欄位擴充、新增直播端點（multipart MJPEG 或 WebSocket，Phase 1 spike 決定）
- 前端：`frontend/` 改為 canvas + 串流，不再輪詢 `/api/frame.jpg`
- 依賴：Phase 1 仍以 OpenCV（V4L2）為主；Phase 4 可能新增 ALSA、音訊模型、本機 LLM 執行環境；未來可能新增 go2rtc
- 維運：systemd unit 加入 `WatchdogSec`；事件片段需要磁碟空間與保留策略
