# Design

## Context

- repo 目前（完成 harden change 之後）是單一 `main.cpp` 加 `kernel.cu`，串流、設定、生命週期都已經修好，Blynk 也已移除，但全部擠在同一個檔案。
- `/workspace/baby_monitor` 有一份沒有 commit 的原型（約 400 行）：`src/main.cpp`、`models/MonitorState`、`vision/VisionPipeline`（ORT + YOLO）、`controllers/{Api,Static}Controller.h`（Crow）、`frontend/`（輪詢 `/api/frame.jpg` 和 `/api/status`）。CMake 用 FetchContent 抓 Crow v1.2.1 與 asio 1.30.2，並連結 `onnxruntime/lib/libonnxruntime.so`。
- 已實測：`yolo26n.onnx` 與 `yolo26n.fp16.onnx` 的輸入都是 `images [1,3,640,640] float`，輸出都是 `output0 [1,300,6]`（模型內含 NMS，每列為 x1,y1,x2,y2,score,class，座標在 640×640 letterbox 空間）。原型照 `[1,84,8400]` 解析，結果是錯的。
- 原型的已知問題：`cap_.open` 呼叫兩次；把 input/output 名稱寫死；CUDA EP 加入失敗時沒有任何提示；`StaticController` 用相對於工作目錄的路徑讀取前端檔案；移動判定只比較相鄰兩張影格（慢速移動永遠不會觸發）。
- 執行環境：RTX 3070 Ti Laptop、OpenCV 4.6、ORT 1.27.0 GPU (CUDA 12) 發行包。

## Goals / Non-Goals

**Goals:**
- 以原型為起點移植，只修上述問題，不重新設計。
- 把可以測試的邏輯（YOLO 解碼、移動判定）寫成純函式，不需要攝影機或 GPU 就能用單元測試驗證。
- harden change 修好的設定驗證、SIGPIPE 防護與正常關閉行為在新架構中不退化。

**Non-Goals:**
- 任何對外通知（推播、Email）。
- 多人追蹤、人物 ID、「寶寶離開畫面」偵測。
- TensorRT EP、`.engine` 檔、FP16 模型切換。
- API 認證與 HTTPS（服務只在區網使用，見 Risks）。

## Decisions

### D1. 目錄結構

```
src/
  main.cpp                     讀設定 → 載入模型 → 開攝影機 → 啟動服務 → 等待結束
  config/Config.{h,cpp}        從 env 讀取並驗證所有 BM_* 變數（從 harden 的 main 搬出來）
  models/MonitorState.{h,cpp}  原型的版本，加上 personDetected
  vision/YoloDetector.{h,cpp}  ORT session、letterbox 前處理、推論
  vision/YoloDecoder.{h,cpp}   純函式：decode(const float*, rows, scale, padX, padY, classId, conf) → best Detection
  vision/MotionTracker.{h,cpp} 純函式狀態機：update(optional<Point2f>) → bool motion
  vision/VisionPipeline.{h,cpp} 擷取執行緒：capture → detect → track → 繪製 → 更新 state
  controllers/ApiController.h, StaticController.h
frontend/  index.html app.js style.css（原樣移植）
tests/     CTest 測試
```
- 替代方案：把整個 `VisionPipeline` 原封搬過來。不採用，因為解碼和移動判定混在執行緒迴圈裡，無法單元測試；拆成兩個純函式檔案的成本很低。

### D2. YOLO26 輸出解碼
逐列讀取 300×6；`class == 0 && score >= conf` 才保留，取 score 最高的一筆。座標轉換：`x = (x1 - padX) / scale`，再 clamp 到影格範圍。letterbox 沿用原型「靠左上補邊」的做法（padX = padY = 0），但解碼器仍保留 pad 參數，以後改成置中補邊時不必改解碼器。模型本身已做 NMS，所以不需要再做一次 NMS。
- input/output 名稱改從 `session.GetInputNameAllocated(0)` 等 API 取得，不再寫死。

### D3. 移動判定：累積位移的基準點
`MotionTracker` 保存 `optional<Point2f> anchor`：
- 沒有偵測到人物 → 清除 anchor，回傳 false
- anchor 為空 → 設為目前位置，回傳 false（同時滿足「第一張影格只建立基準」與「人物重新出現不觸發」）
- 與 anchor 的距離 > `motionPx` → 把 anchor 更新為目前位置，回傳 true
- 替代方案：原型的「相鄰影格」比較。每張影格約 33ms，嬰兒翻身每張影格可能只移動 1–3 像素，永遠到不了門檻，所以不採用。

### D4. 只保留一個 HTTP 服務
Crow 在 `BM_PORT`（18080）提供網頁和 API，harden 版的 MJPEG 伺服器整個刪除，`BM_STREAM_PORT` 不再使用。前端沿用原型的輪詢方式（每 250ms 抓一次 `/api/frame.jpg`）。
- 替代方案：保留 MJPEG 在 8080。原本是給 Blynk App 的影像元件用，Blynk 棄用後只剩「用 VLC 觀看」這個用途，不值得多維護一個 socket 伺服器和一個 port。
- 替代方案：用 Crow 輸出 MJPEG。Crow 1.2 不容易做長時間串流回應，實作風險高；輪詢的 4 FPS 對看嬰兒已經足夠。
- 刪掉 MJPEG 後就沒有自己寫的 socket 程式碼了，但仍保留 `SIG_IGN` SIGPIPE，以防 Crow 寫入已斷線的連線。

### D5. 執行緒與生命週期
- 主執行緒：依序 `Config::load()` → 建立 `YoloDetector`（失敗就 `return 1`，此時還沒開任何 port）→ 開攝影機 → `pipeline.start()` → `app.run()`。
- 訊號：交給 Crow 預設的 SIGINT/SIGTERM 處理（asio `signal_set`），`app.run()` 返回後呼叫 `pipeline.stop()`。harden 版自己寫的 `sigaction` 移除，避免和 Crow 衝突。SIGPIPE 仍然 `SIG_IGN`。
- 攝影機中斷：擷取執行緒設定 `cameraLost` 旗標並呼叫 `app.stop()`；`main` 據此 `return 1`。

### D6. 前端檔案位置
`StaticController` 以執行檔所在目錄（`readlink("/proc/self/exe")`）加上 `frontend/` 作為根目錄；CMake 的 POST_BUILD 步驟會把 `frontend/` 複製到執行檔旁邊（沿用原型）。
- 替代方案：把前端檔案編進執行檔。需要額外的產生步驟，目前只有三個小檔案，所以不採用。

### D7. 建置
- `CMAKE_CXX_STANDARD 17`，`project(... LANGUAGES CXX)`，不再需要 CUDA。
- `set(ORT_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime" CACHE PATH ...)`；configure 時檢查 `${ORT_ROOT}/include/onnxruntime_cxx_api.h` 是否存在，找不到就 `FATAL_ERROR` 並提示下載位置。
- 設定 `BUILD_RPATH`，讓 `${ORT_ROOT}/lib` 不需要 `LD_LIBRARY_PATH` 也能被找到（CUDA provider 的 `.so` 在同一個目錄，由 ORT 動態載入）。
- 加上 `-Wall -Wextra`。
- 核心程式碼編成 `bm_core` static library，讓主程式和測試共用。

### D8. 測試
用 CTest 加上不依賴任何框架的 `assert` 測試執行檔，不額外引入測試框架：
- `test_yolo_decoder`：用合成的 300×6 陣列測試類別過濾、門檻、取最高分、scale/pad 轉換、clamp。
- `test_motion_tracker`：涵蓋 spec 中的所有情境（第一次偵測、緩慢累積、抖動、人物消失後重現）。
- `test_monitor_state`：測試 `lastMotionEpochMs`、影格複製、多執行緒同時讀寫。
- `test_yolo_inference`：用 `tests/data/bus.jpg`（取自原型目錄，810×1080，內含多個人和一台公車）跑真實模型，預期至少偵測到 1 個人，且框落在圖片範圍內；找不到 `BM_MODEL_PATH` 指定的模型時回傳 CTest 的 skip code 77。

## Risks / Trade-offs

- [只追蹤信心最高的人：大人抱寶寶時，追蹤對象可能在兩人之間跳動，造成誤判為移動] → 在 Non-Goals 中說明；之後可改成依 IoU 跟隨同一個人。
- [YOLO 的 person 類別用成人資料訓練，對包著被子的嬰兒偵測率可能偏低] → 信心門檻可以用 `BM_PERSON_CONF` 調整；實機驗證列在 tasks。
- [CUDA EP 無法載入時改用 CPU，推論可能降到每秒 5–10 張影格] → 啟動時會輸出警告；擷取迴圈不假設固定 FPS。
- [網頁 port 沒有認證，區網內任何人都看得到畫面] → 文件中說明只在信任的網路使用；認證留給之後的 change。
- [FetchContent 需要網路] → 第一次 configure 之後會快取在 `build/_deps`。
- [ORT 發行包約 240MB、模型約 10MB，都不進 git] → README 寫明下載與匯出步驟，CMake 找不到時給出清楚的錯誤。

## Migration Plan

1. 先 archive `harden-monitor-stability-security`。
2. 把 ORT 1.27.0 GPU 發行包解壓到 `onnxruntime/`、把 `yolo26n.onnx` 放到 `models/`（可以從 `/workspace/baby_monitor` 複製）。
3. 清掉舊的 build（`rm -rf build`，因為 project 的 languages 改了），重新 configure 與 build。
4. 回滾：回到 harden 版的 commit 即可；注意對外 port 會從 18080 變回 8080（`BM_STREAM_PORT`）。
