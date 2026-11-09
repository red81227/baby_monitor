# Proposal

## Why

目前的 repo 把 Blynk auth token 寫死在 `main.cpp` 並 commit 進 git，也把整個 `build/` 產物一起追蹤；主迴圈另有幾個會造成誤判、崩潰或無法關閉的 bug（啟動即判定為移動、客戶端斷線觸發 SIGPIPE 讓程式結束、`q` 鍵與 `stop()` 實際上無法結束程式）。Blynk 推播暫時棄用，因此直接移除整合而不是改成環境變數。在進行模組化與 YOLO26n 改造（後續 change `modularize-yolo26-detection`）之前，先把這些安全與穩定性問題修掉。

## What Changes

- 新增 `.gitignore`，將 `build/`、`.venv/`、模型檔與 ONNX Runtime 發行包排除，並把已追蹤的 `build/` 從 git 索引移除（保留本機檔案，不改寫歷史）。
- **BREAKING**：移除 Blynk 推播整合（寫死的 token、事件代碼、`sendBlynkAlert`、libcurl 依賴）。偵測到移動時只在串流畫面上標示，不再對外通知。舊 token 仍留在 git 歷史中，建議在 Blynk Console 作廢。
- MJPEG 串流 port 改為可由環境變數 `BM_STREAM_PORT` 設定（預設維持 8080）。
- 修正啟動時第一張影格與全黑基準比較而必定判定為移動的問題。
- 修正串流客戶端斷線時因 SIGPIPE 導致整個程式終止的問題。
- 支援 SIGINT / SIGTERM 正常關閉：停止串流伺服器、釋放攝影機與 GPU 資源後以 0 結束；移除無效的 `cv::waitKey` 結束方式。
- 攝影機開啟失敗或影格尺寸與初始化不符時，輸出明確錯誤並以非 0 結束，而非靜默退出或寫壞記憶體。
- 不在範圍內：CUDA 錯誤檢查與效能調整（CUDA 移動偵測將在下一個 change 被 YOLO26n 取代）、改寫 git 歷史、其他通知管道。

## Capabilities

### New Capabilities
- `runtime-configuration`: 執行期設定的來源、預設值與驗證規則。
- `motion-detection`: 從影像判定「畫面中有移動」的行為，包括啟動基準與觸發條件。
- `video-streaming`: 對外提供即時 MJPEG 影像串流，以及多客戶端與斷線時的穩定性。
- `service-lifecycle`: 程式啟動失敗處理與收到終止訊號時的正常關閉。

### Modified Capabilities
（無，目前 `openspec/specs/` 尚無既有規格）

## Impact

- 程式碼：`main.cpp`（移除 Blynk、設定讀取、訊號處理、主迴圈、`MJPEGWriter`）、`CMakeLists.txt`（移除 CURL 依賴）。
- Repo：新增 `.gitignore`；`build/` 從索引移除。
- 使用者行為：手機不再收到推播；監看改為只透過 MJPEG 串流。
- 後續 change `modularize-yolo26-detection` 會以本 change 的規格為基礎修改 `motion-detection` 等能力，因此本 change 應先 archive。
