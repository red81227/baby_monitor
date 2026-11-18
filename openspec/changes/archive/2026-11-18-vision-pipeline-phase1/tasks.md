# Tasks

## 1. Spikes

- [x] 1.1 S1：寫一個暫時的小程式，以 OpenCV V4L2 設定 MJPG + `CAP_PROP_CONVERT_RGB=0` 從 `/dev/video0` 抓 100 張影格，確認 (a) bytes 能以 `cv::imdecode` 與瀏覽器解碼（檢查是否缺 DHT）、(b) `CAP_PROP_POS_MSEC` 單調且與 `steady_clock` 同時間軸、(c) `CAP_PROP_BUFFERSIZE` 可設為 1–2、(d) 攝影機支援的 MJPG 解析度與 fps；把結論寫回 design.md 的 D1
- [x] 1.2 S2：寫一個暫時的 Crow 範例，比較 multipart MJPEG 與 WebSocket binary 推送 720p JPEG 30 秒（兩個瀏覽器、其中一個限速），確認能否持有連線、跨執行緒寫入、斷線清理；把選擇與 binary 訊息格式寫回 design.md 的 D6，刪除暫時程式

## 2. 設定

- [x] 2.1 在 `Config` 新增 `BM_CAMERA_DEVICE`、`BM_CAPTURE_WIDTH`、`BM_CAPTURE_HEIGHT`、`BM_DETECT_MIN_HZ`、`BM_DETECT_MAX_HZ` 與驗證（含下限大於上限），擴充 `tests/test_config.cpp` 涵蓋預設值與每個無效值，`ctest -R config` 通過
- [x] 2.2 README 的環境變數表加入新變數，並說明 `BM_MOTION_PX` 以主碼流像素計、提高解析度時需要調整

## 3. 共用基礎

- [x] 3.1 新增 `LatestSlot<T>`（`shared_ptr<const T>` + `seq`、`publish` / `waitNewerThan` 含逾時與關閉喚醒），新增 `tests/test_latest_slot.cpp` 測試覆蓋、讀取端拿到最新值、關閉時喚醒等待者，`ctest` 通過
- [x] 3.2 定義 `RawFrame { seq, tCapture, jpeg, width, height }` 與 `DetectionResult { seq, tCapture, person, motion }`，`MonitorState` 改為發布不可變 `Snapshot`（含 `cameraConnected`、最新 `seq`、`detectHz`、最新 `RawFrame`），更新 `tests/test_monitor_state.cpp`，`ctest -R monitor_state` 通過

## 4. FrameSource 與擷取

- [x] 4.1 實作 `FrameSource` 介面與 V4L2 實作（依 S1 結論選 OpenCV 或 ioctl；需要時補 DHT；時間戳不 monotonic 時退回 `steady_clock`），啟動訊息輸出裝置、格式與實際解析度；以實機執行確認訊息正確
- [x] 4.2 實作 CaptureThread：擷取 → 指定 `seq`（從 1 開始，重連後延續）→ 發布到 `LatestSlot<RawFrame>`；斷線時發布 `cameraConnected=false` 並以 250ms→5s 退避重連，關閉時可中斷。抽出退避計算與 `seq` 指定為純函式並加上單元測試；實機拔插攝影機，確認 10 秒內恢復且程式沒有結束
- [x] 4.3 實作子碼流的 reduced decode 倍率選擇（長邊 >= 640 的最大倍率；主碼流長邊 < 1280 時為 1）為純函式，新增單元測試涵蓋 640x480、1280x720、1920x1080

## 5. 分析與排程

- [x] 5.1 實作 `ActivityScorer`（約 160x90 灰階絕對差，`|diff| > 25` 的像素比例），新增 `tests/test_activity_scorer.cpp`：相同影像分數為 0、局部變化分數落在預期範圍、第一張影格分數為 0
- [x] 5.2 實作 `DetectScheduler`（IDLE / ACTIVE、5 秒回復、依 hz 判斷是否該偵測、最近 3 秒的實際 `detectHz`），以注入時間的方式新增 `tests/test_detect_scheduler.cpp` 涵蓋 spec 的三個情境與 min == max 的固定頻率
- [x] 5.3 調整 `YoloDetector` 介面，讓它接受子碼流與縮小倍率，並回傳主碼流座標的框；擴充 `tests/test_yolo_inference.cpp`：以 `bus.jpg` 縮小 1/2 推論，回傳的框落在原圖範圍並與全尺寸推論結果大致一致
- [x] 5.4 實作 AnalysisThread：等待更新的 `RawFrame` → reduced decode → 評分 → 依排程跑 YOLO 與 `MotionTracker` → 發布帶 `seq` / `tCapture` 的 `DetectionResult` 與更新 `MonitorState`；實機執行時以 `/api/status` 確認靜止時 `detectHz` 約 1、揮手時升到上限、停止 5 秒後降回

## 6. HTTP 與即時影像

- [x] 6.1 `/api/status` 新增 `cameraConnected`、`seq`、`detectHz`（原欄位保留）；`/api/frame.jpg` 改回傳最新 `RawFrame` 原圖並加上 `X-Frame-Seq`，尚無影格時回傳 503；以 `curl -i` 確認欄位、標頭與 503 行為，README 的 HTTP 介面表同步更新
- [x] 6.2 依 S2 結論實作 LiveHub 與即時影像端點：每個連線一個 latest-wins 待送槽、沒有連線時不處理、偵測完成時推送 metadata（`seq`、框、信心值、motion）；以兩個瀏覽器（其中一個限速）確認 spec 的情境，並把端點與訊息格式寫進 README
- [x] 6.3 確認關閉時即時影像連線會被關閉：在有瀏覽器連線時按 Ctrl+C，程式在 5 秒內以 0 結束且 port 可重新綁定

## 7. 前端

- [x] 7.1 `frontend/` 改為 canvas：連到即時影像端點、以 `createImageBitmap` 解碼（解碼中收到的影格直接丟棄）、保留約 1 秒的 `seq -> 影格`、依 `seq` 畫框（移動時換色）；手動確認人物快速移動時框不錯位
- [x] 7.2 顯示移動狀態、攝影機中斷訊息、後端無法連線訊息，即時影像中斷時自動重連；手動確認拔插攝影機與重啟服務時頁面都能自動恢復，不需要重新整理

## 8. 服務生命週期

- [x] 8.1 實作 `NOTIFY_SOCKET` 通知（`READY=1`、依 `WATCHDOG_USEC` 檢查 Capture 與 Analysis 的進度後送 `WATCHDOG=1`，支援 `@` 抽象 namespace），以一個接收 datagram 的測試程式驗證訊息內容與停滯時停止送出；沒有 `NOTIFY_SOCKET` 時不輸出錯誤
- [x] 8.2 調整 `main.cpp` 的啟動與關閉順序：設定 → 模型 → 開攝影機（失敗時輸出裝置路徑並結束）→ HTTP → 擷取 / 分析 / notifier；攝影機中斷不再結束程式，SIGTERM 在重連中也能在 5 秒內以 0 結束；刪除舊的 `VisionPipeline` 與 `updateFrameJpeg`，`ctest` 全部通過
- [x] 8.3 README 的「執行」與 systemd 範例更新為 `Type=notify` + `WatchdogSec=30`，說明攝影機中斷時改為重連；以 `systemd-run --user -p Type=notify -p WatchdogSec=30` 實際執行確認服務進入 active，且不會被 watchdog 重啟

## 9. 整合驗證

- [ ] 9.1 在實機上執行 30 分鐘：以 `/api/status` 確認 `seq` 持續增加、`detectHz` 依動靜變化，即時影像影格率接近攝影機影格率，`top` 顯示沒有觀看者時 CPU 明顯低於有觀看者時，記憶體沒有持續成長
- [x] 9.2 執行 `openspec validate vision-pipeline-phase1 --strict` 並修正所有問題

## Workflow follow-up

- 全部 tasks 完成並審查後，以 `/opsx:archive` 歸檔本變更
- 把 spike 結論與實測數據回饋到 `redesign-vision-architecture/design.md`
