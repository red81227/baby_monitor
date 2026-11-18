# Proposal

## Why

目前只知道人物框的位置，移動判定靠框中心位移，無法分辨「翻身」與「手腳在動」，之後也做不了「看不到臉」這類判斷。YOLO26n-pose 在同一次推論中多輸出 17 個 COCO 關鍵點，成本與現在的偵測模型相近，所以先換成 pose 模型並把骨架顯示在畫面上，確認它在實機上可運作、量出推論時間，作為之後「以關鍵點判斷移動」與「是否需要 TensorRT」的依據。

本變更建立在 `vision-pipeline-phase1` 之上（即時影像端點、子碼流推論、前端 canvas 都是該變更新增的），必須在它歸檔之後才能歸檔。

## What Changes

- 預設模型改為 YOLO26n-pose（ONNX、內含 NMS 的 end2end 匯出，輸出 `[1, 300, 57]`）；仍可透過 `BM_MODEL_PATH` 使用原本的 YOLO26n 偵測模型（`[1, 300, 6]`），此時不輸出關鍵點
- 人物偵測結果加上 17 個關鍵點（x、y 以主碼流像素為單位，加上 0–1 的可見度）
- 即時影像端點的偵測 metadata 加上關鍵點欄位（新增欄位，原欄位不變）
- 監視頁面在人物框之外畫出 COCO 骨架，只畫可見度足夠的關鍵點與連線
- README 的模型匯出步驟改為 pose 模型並加上 `end2end=True`
- 實測並記錄 CUDA 下每次推論的時間

不在本期範圍（之後另開 change）：
- TensorRT 執行（依本期量到的推論時間決定要不要做）
- 以關鍵點取代框中心判斷移動；`MotionTracker` 與 `/api/status` 的移動判定行為維持不變
- 多人偵測（仍只回報信心最高的一個人）

## Capabilities

### New Capabilities

（無）

### Modified Capabilities
- `person-detection`: 偵測結果包含人物關鍵點；支援 pose 與純偵測兩種模型輸出
- `live-view`: 偵測 metadata 加上關鍵點（此 capability 由 `vision-pipeline-phase1` 新增）
- `web-dashboard`: 監視頁面在人物框上加畫骨架
- `runtime-configuration`: `BM_MODEL_PATH` 預設改為 `models/yolo26n-pose.onnx`

## Impact

- 程式碼：`src/vision/YoloDecoder`（解碼關鍵點、每列欄數可變）、`src/vision/YoloDetector`（載入時辨識模型類型）、`src/vision/LiveProtocol`（JSON 加欄位）、`src/config/Config`（預設路徑）
- 前端：`frontend/app.js` 畫骨架
- API：`/ws/live` 的 detection 訊息新增 `keypoints` 欄位（向後相容，舊客戶端可忽略）；`/api/status`、`/api/frame.jpg` 不變
- 維運：**BREAKING** 預設模型檔改名，升級時需依 README 匯出 `models/yolo26n-pose.onnx`，或設定 `BM_MODEL_PATH` 指回舊模型
- 依賴：不新增執行期依賴；匯出模型需要 Ultralytics（只在開發機上）
