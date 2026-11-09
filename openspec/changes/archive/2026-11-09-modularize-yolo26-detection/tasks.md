# Tasks

## 1. 建置骨架與依賴

- [x] 1.1 改寫 `CMakeLists.txt`：C++17、只用 CXX、`ORT_ROOT` cache 變數與存在檢查、FetchContent 抓 Crow v1.2.1 / asio 1.30.2、`bm_core` static library + `BabyMonitor` 執行檔、`BUILD_RPATH`、`-Wall -Wextra`、POST_BUILD 複製 `frontend/`、`enable_testing()`；驗證：`ORT_ROOT` 指到不存在的路徑時 configure 出現明確的 FATAL_ERROR，指到正確路徑時 configure 成功
- [x] 1.2 把 ORT 1.27.0 解壓到 `onnxruntime/`、`yolo26n.onnx` 放到 `models/`（從 `/workspace/baby_monitor` 複製），確認兩者都被 `.gitignore` 排除（補上 `models/*.onnx`）；驗證：`git status` 看不到這兩個項目
- [x] 1.3 從 `/workspace/baby_monitor/bus.jpg` 複製到 `tests/data/bus.jpg`；驗證：檔案存在且 `git status` 顯示為新檔案

## 2. 設定模組

- [x] 2.1 新增 `src/config/Config.{h,cpp}`：把 harden 版 `main()` 中的 env 讀取搬過來，並新增 `BM_MODEL_PATH`、`BM_PORT`、`BM_MOTION_PX`、`BM_PERSON_CONF`，移除 `BM_STREAM_PORT`；驗證：新增 `tests/test_config.cpp`（用 `setenv` 測試預設值與每個變數的無效值），`ctest -R config` 通過

## 3. 狀態與偵測邏輯（純函式）

- [x] 3.1 移植 `MonitorState.{h,cpp}` 到 `src/models/`，加上 `personDetected`；驗證：`tests/test_monitor_state.cpp` 涵蓋初始值、`lastMotionEpochMs`、多執行緒同時讀寫 1 秒不當機，`ctest -R monitor_state` 通過
- [x] 3.2 新增 `src/vision/YoloDecoder.{h,cpp}`，依 `[300,6]` 格式解碼（D2）；驗證：`tests/test_yolo_decoder.cpp` 涵蓋非 person 類別被忽略、低於門檻被忽略、取最高分、scale/pad 轉換、超出範圍時 clamp、全部為 0 時回傳沒有人物，`ctest -R yolo_decoder` 通過
- [x] 3.3 新增 `src/vision/MotionTracker.{h,cpp}`（D3）；驗證：`tests/test_motion_tracker.cpp` 涵蓋 motion-detection spec 中的每個 scenario，`ctest -R motion_tracker` 通過

## 4. 推論

- [x] 4.1 新增 `src/vision/YoloDetector.{h,cpp}`：建立 ORT session、加入 CUDA EP（失敗時輸出 CPU fallback 警告，成功時輸出使用 CUDA）、從 session 查詢 I/O 名稱、letterbox 前處理、呼叫 `YoloDecoder`；模型載入失敗時拋出含路徑的例外；驗證：`tests/test_yolo_inference.cpp` 對 `tests/data/bus.jpg` 至少偵測到 1 個人且框落在 810×1080 範圍內，沒有模型時回傳 77（skip），`ctest -R yolo_inference` 通過，且輸出中看得到使用 CUDA

## 5. 影像管線與 HTTP

- [x] 5.1 新增 `src/vision/VisionPipeline.{h,cpp}`：擷取 → `YoloDetector` → `MotionTracker` → 畫框（靜止綠色、移動紅色）→ 更新 `MonitorState`；攝影機中斷時設定 `cameraLost` 並呼叫停止回呼；驗證：實際執行時人物周圍出現綠框，人移動時框變紅，`/api/status` 的 `motionDetected` 為 true
- [x] 5.2 移植 `ApiController.h`（`/api/status` 加上 `personDetected`）、`StaticController.h`（D6：以執行檔所在目錄為根）與 `frontend/`；驗證：在 repo 根目錄執行 `./build/BabyMonitor`，`curl -i localhost:18080/` 回 200、`curl localhost:18080/api/status` 的欄位符合 status-api spec、還沒有影格時 `/api/frame.jpg` 回 503
- [x] 5.3 改寫 `src/main.cpp`，照 D5 的順序啟動與關閉（Crow 處理訊號、`SIG_IGN` SIGPIPE、模型載入失敗時在開 port 之前就 `return 1`）；刪除根目錄的 `main.cpp`（含 MJPEG 伺服器）和 `kernel.cu`；驗證：`BM_MODEL_PATH=/nonexistent.onnx` 時輸出含該路徑的錯誤、以非 0 結束，且 `ss -ltn` 沒有看到 18080；正常啟動後 8080 沒有被監聽

## 6. 文件

- [x] 6.1 新增 `README.md`：需求（OpenCV、ORT 1.27 GPU 的下載網址、CUDA 12 runtime）、用 ultralytics 匯出 `yolo26n.onnx` 的指令、建置與測試指令、所有 `BM_*` 環境變數與預設值、網頁 port、只在信任網路使用的提醒、systemd `EnvironmentFile=` 範例；驗證：在乾淨的 build 目錄照 README 步驟執行，可以成功建置並啟動

## 7. 整合驗證

- [x] 7.1 `rm -rf build && cmake -S . -B build && cmake --build build && ctest --test-dir build` 全部通過，且沒有編譯警告
- [ ] 7.2 實機驗證：畫面中有人時，網頁 `:18080` 看得到綠框；人慢慢移動時框變紅、狀態顯示有移動；開燈關燈但沒人移動時不判定為移動；有瀏覽器連線時按 Ctrl+C 能在 5 秒內以 0 結束；把 `BM_PERSON_CONF` 調低後，觀察包著被子的嬰兒（或替代物）的偵測率，並把建議值記到 README

## Workflow follow-up

- 本 change 要在 `harden-monitor-stability-security` archive 之後才能 archive（MODIFIED / REMOVED 規格依賴它建立的主規格）。
- 實作完成並審查後 archive，再確認 `openspec/specs/` 中的 `motion-detection` 已經沒有像素差異的需求。
