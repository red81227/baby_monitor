# Proposal

## Why

像素差異式的移動偵測會被光線變化、窗簾、影子觸發，誤報多，而且分不出畫面裡是不是寶寶在動。`/workspace/baby_monitor` 已經有一份未 commit 的原型，做了模組化（`VisionPipeline`、`MonitorState`、Crow API、網頁前端）並接上 ONNX Runtime + YOLO26n，但它的輸出解析是照 YOLOv8 的 `[1,84,8400]` 寫的，而實際模型輸出是 YOLO26 免 NMS 的 `[1,300,6]`，所以目前偵測結果是錯的。這個 change 把那份原型正式移進 repo、修正解析，並以人物偵測取代 CUDA 像素差異。

## What Changes

- 將 `/workspace/baby_monitor` 的模組化結構移植進 repo（`src/` 下的 models / vision / controllers、`frontend/`），`main.cpp` 搬到 `src/main.cpp`。
- 修正 YOLO26n 輸出解析為 `[1,300,6]`（x1, y1, x2, y2, score, class），並把座標從 letterbox 空間正確轉回原始影格。
- **BREAKING**：移除 CUDA 灰階與影格差異偵測（`kernel.cu`），改用「人物框中心相對基準點的位移」判定移動；建置不再需要 CUDA 編譯器（推論仍可透過 ONNX Runtime CUDA EP 使用 GPU）。
- **BREAKING**：移除 harden change 留下的 MJPEG 串流伺服器（port 8080 / `BM_STREAM_PORT`），即時畫面改由網頁介面提供，只剩一個 HTTP port。
- 新增網頁介面與 HTTP API：`/`（即時畫面與狀態）、`/api/status`、`/api/frame.jpg`。
- 新增設定：`BM_MODEL_PATH`、`BM_PORT`、`BM_MOTION_PX`、`BM_PERSON_CONF`；移除 `BM_STREAM_PORT`。
- 加入 CTest 單元測試：YOLO 輸出解碼、移動判定、`MonitorState`，以及以樣本圖片驗證模型推論的整合測試。
- 不在範圍內：任何對外通知（Blynk 已在 harden change 移除）、多人追蹤、「寶寶離開畫面」偵測、API 認證、TensorRT engine。

## Capabilities

### New Capabilities
- `person-detection`: 以 YOLO26n 在每張影格中找出人物並給出位置與信心值，包含模型載入與執行裝置。
- `status-api`: 以 HTTP 提供目前監視狀態（JSON）與最新影格（JPEG）。
- `web-dashboard`: 瀏覽器可直接開啟的監視頁面，顯示即時畫面與移動狀態。

### Modified Capabilities
- `motion-detection`: 移動判定從「連續影格的像素差異」改為「人物位置相對基準的位移」；移除影格尺寸必須固定的限制。
- `video-streaming`: 移除 MJPEG 串流的所有需求，即時畫面改由 `status-api` 與 `web-dashboard` 提供。
- `runtime-configuration`: 新增模型路徑、網頁 port、移動門檻與人物信心門檻設定；移除串流 port 設定。
- `service-lifecycle`: 正常關閉的對象從串流伺服器改為網頁服務；新增模型無法載入時的啟動失敗行為。

## Impact

- 程式碼：新增 `src/`（main、config、models、vision、controllers）與 `frontend/`；刪除根目錄的 `main.cpp`、`kernel.cu`；新增 `tests/`。
- 建置：`CMakeLists.txt` 改為 C++17，移除 CUDA language，新增 ONNX Runtime（`ORT_ROOT`，預設 `onnxruntime/`）與以 FetchContent 取得的 Crow v1.2.1、asio 1.30.2；第一次 configure 需要網路。
- 執行環境：需要 ONNX Runtime 1.27 GPU 版和 `yolo26n.onnx`（兩者都不進 git，取得方式寫在 README）；對外 port 從 8080（MJPEG）改為 18080（網頁）。
- 依賴關係：必須先 archive `harden-monitor-stability-security`，本 change 的 MODIFIED / REMOVED 規格才有對象。
