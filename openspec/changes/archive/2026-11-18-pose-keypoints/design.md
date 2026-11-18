# Design

## Context

動機見 proposal.md。目前的現況與限制：

- `YoloDetector` 以 ONNX Runtime 執行 end2end（內含 NMS）的 YOLO26n。載入時檢查輸出必須是 `[1, N, 6]`，推論時以 letterbox（等比例縮放、靠左上放置、pad = 0）把子碼流送進 640x640 輸入，再由 `decodeBestDetection` 把結果換回主碼流座標（`scale / reduceFactor`，並 clamp 到主碼流寬高）。
- `decodeBestDetection` 把每列固定當作 6 欄，只回傳信心最高的 person。
- 偵測結果經 `AnalysisThread` → `LiveHub` 以 JSON 推送（`live::encodeDetection`，目前用 384 bytes 的固定緩衝），前端 `render()` 依 `seq` 找對應的偵測結果畫框。
- 已實測的模型格式：以 `yolo export model=yolo26n-pose.pt format=onnx imgsz=640 end2end=True` 匯出，輸入 `images [1, 3, 640, 640]`，輸出 `[1, 300, 57]`。每列依序為 `x1, y1, x2, y2, score, class`，接著 17 組 `(x, y, 可見度)`，座標在 letterbox 輸入空間，可見度已是 0–1。以 `bus.jpg` 與 Ultralytics 的推論結果比對，框與鼻子座標相差 1–3 像素（差異來自兩邊縮放方式不同）。不加 `end2end=True` 時輸出是 `[1, 56, 8400]`（未經 NMS），本設計不支援。

## Goals / Non-Goals

**Goals:**
- 關鍵點與框走完全相同的座標換算，前端不需要知道子碼流或 letterbox 的存在
- 同一份程式同時支援 pose 與純偵測模型，切換只靠 `BM_MODEL_PATH`
- 量出 CUDA 下 pose 模型每次推論的時間，作為是否導入 TensorRT 的依據

**Non-Goals:**
- 不改 `MotionTracker`、`/api/status`、`/api/frame.jpg`
- 不支援未內含 NMS 的匯出（不在程式內實作 NMS）
- 不調整 `DetectScheduler` 的頻率，也不為推論時間做任何最佳化

## Decisions

### D1. 依輸出欄數辨識模型類型

載入時讀 `output0` 的形狀：`[1, N, 6]` 為純偵測，`[1, N, 57]` 為 pose；其他形狀拋出包含模型路徑與實際形狀的 `std::runtime_error`，沿用現有「模型無法載入時結束」的路徑。啟動訊息加上模型類型，例如 `(執行裝置: CUDA, 類型: pose)`。

替代方案：讀 ONNX metadata 的 `task` / `kpt_shape`。Ultralytics 有寫這些 metadata，但我們真正依賴的是輸出的記憶體排列，形狀就是最直接的檢查；metadata 也可能在其他工具轉檔後遺失。

### D2. 解碼器改為「每列欄數」參數化，關鍵點用同一個換算

`decodeBestDetection` 增加 `int stride`（6 或 57），挑選最佳那一列的邏輯不變。選出後，若 `stride == 57`，用與框相同的 `toX` / `toY`（減 pad、除以 scale、clamp）換算 17 個關鍵點，可見度原樣保留。

`Detection` 增加 `std::optional<std::array<Keypoint, 17>> keypoints`，`Keypoint { float x, y, visibility; }`。用 `optional` 而不是空的 `vector`，讓「沒有關鍵點」與「有 17 個點」以型別區分，不會出現長度不對的情況。

可見度低的關鍵點座標仍可能落在影格外（模型會「猜」被遮住部位的位置），所以一律 clamp；是否要畫交給前端依可見度決定。

### D3. JSON 新增 `keypoints` 欄位

`detection` 訊息加上 `"keypoints": [[x, y, v], ...]`（17 組，x / y 取 1 位小數、v 取 2 位小數），沒有時為 `null`。既有欄位的名稱與格式不變，舊客戶端忽略新欄位即可。`encodeDetection` 改用 `std::string` 組字串，不再用固定大小的 `char` 緩衝；訊息大約從 200 bytes 增加到 550 bytes，以最多每秒 10 次計算可以忽略。

替代方案：改成二進位格式以節省頻寬。目前 metadata 的流量遠小於影像，不值得增加前後端的解析複雜度。

### D4. 前端在同一筆偵測上畫骨架

`render()` 畫完框之後，若該筆偵測有 `keypoints`，用 COCO 的 16 條連線（`[5,7],[7,9],[6,8],[8,10],[5,6],[5,11],[6,12],[11,12],[11,13],[13,15],[12,14],[14,16],[0,1],[0,2],[1,3],[2,4]`）畫線，再畫點。可見度門檻 0.5，連線兩端都達到門檻才畫；顏色與框相同（移動紅、靜止綠），點的半徑與線寬依 canvas 寬度縮放。骨架與框使用 `detectionFor()` 回傳的同一筆資料，所以 `seq` 對應與 1 秒過期規則自動一致。

### D5. 推論時間以 `yolo_inference` 測試量測

在 `tests/test_yolo_inference.cpp` 加一段量測：先暖機數次，再對 `bus.jpg` 連續推論 50 次，輸出平均與最大的毫秒數，以及實際的執行裝置與模型類型。只輸出、不設門檻，避免在不同機器上誤判失敗。量測結果寫回本文件的「實測結果」段落。

替代方案：在執行中把推論時間放進 `/api/status`。這是長期有用的觀測值，但屬於新的 API 欄位，留到決定 TensorRT 的 change 再一起規劃。

### D6. 預設模型與測試資料

`Config` 的預設路徑改為 `models/yolo26n-pose.onnx`。CMake 的測試環境變數改指向 pose 模型；`yolo_inference` 測試另外在 `models/yolo26n.onnx` 存在時也跑一次，確認純偵測模型仍正常、且不回報關鍵點。兩個模型檔都不存在時沿用現有的 Skipped 行為。

## 實測結果

2.3 的量測（`test_yolo_inference`，RTX 3070 Ti Laptop、ONNX Runtime 1.27 CUDA EP、`SetIntraOpNumThreads(1)`；輸入為 640x360，即 1280x720 主碼流縮小 2 倍的子碼流；暖機 5 次後連續 50 次，共跑 3 輪）：

| 模型 | 每次總時間 avg | max | 其中前處理（letterbox + blob） | 推論 + 資料搬移 + 解碼（相減） |
|---|---|---|---|---|
| YOLO26n-pose | 9.4–9.8 ms | 11.0–11.4 ms | 約 4.1 ms | 約 5.5 ms |
| YOLO26n（純偵測） | 8.6–9.3 ms | 9.8–10.8 ms | 約 3.6–4.3 ms | 約 4.8 ms |

- pose 模型比純偵測模型每次只多約 0.7–1 ms。
- 以 `bus.jpg` 驗證：pose 回報 16 個可見度 ≥ 0.5 的關鍵點，全部落在框內；以縮小 2 倍的影像推論時，可見關鍵點與全尺寸推論相差最多 3.7 px，框的 IoU 0.98。
- 對 TensorRT 的意義：最高頻率每秒 10 次時，YOLO 每秒約佔 100 ms，其中約 40% 是 CPU 上的前處理，TensorRT 只能縮短剩下的 GPU 部分（約 5.5 ms）。即使減半，每秒也只省下約 30 ms，延遲改善約 3 ms。

## Risks / Trade-offs

- [pose 模型對包在被子裡的嬰兒，偵測率可能低於純偵測模型] → 兩種模型都支援，以 `BM_MODEL_PATH` 一行切回；實機使用一段時間後再決定預設值
- [使用者照舊指令匯出，得到 `[1, 56, 8400]` 的模型] → 啟動時明確報錯並印出形狀；README 的匯出指令加上 `end2end=True` 並說明原因
- [關鍵點在快速移動時比框更容易看出延遲，因為偵測最快每秒 10 次] → 與框相同的限制，本期只記錄觀察，不處理
- [預設模型檔改名，舊部署升級後啟動失敗] → 失敗訊息含模型路徑；README 寫明升級步驟

## Migration Plan

1. 依 README 在開發機匯出 `models/yolo26n-pose.onnx`（需 `end2end=True`），複製到部署機的 `models/`
2. 部署新版本；若要繼續用舊模型，在 systemd 的 `EnvironmentFile` 設定 `BM_MODEL_PATH=.../yolo26n.onnx`
3. 回滾：設定 `BM_MODEL_PATH` 指回舊模型即可，不需要回退程式

本變更依賴 `vision-pipeline-phase1`（`live-view` capability 由它新增），歸檔順序必須在它之後。

## Open Questions

- 是否需要 TensorRT：依 D5 量到的推論時間決定，另開 change。
