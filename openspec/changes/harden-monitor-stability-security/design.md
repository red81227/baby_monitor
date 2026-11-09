# Design

## Context

所有邏輯都在 [main.cpp](../../../main.cpp)：主迴圈（CUDA 灰階 + 影格差異 → OpenCV 輪廓）、`MJPEGWriter`（每個客戶端一個 detached thread、`accept()` 阻塞）、`sendBlynkAlert`（detached thread + libcurl）。Blynk 推播已決定棄用，整段移除。動機見 proposal.md - Why。

本 change 之後，`modularize-yolo26-detection` 會把程式拆成模組並以 YOLO26n 取代 CUDA 移動偵測。因此這裡的修正以「小而能直接搬走」為原則：串流、設定、生命週期的修正之後會原封搬進新模組；CUDA 路徑只修行為上的 bug，不做額外投資。

## Goals / Non-Goals

**Goals:**
- 修正 specs 中列出的行為，盡量維持 `main.cpp` 現有結構，讓 diff 小、容易審查。
- 不新增任何第三方依賴。

**Non-Goals:**
- 拆檔或模組化（交給下一個 change）。
- CUDA 錯誤檢查、減少 host/device 複製、移除每個 kernel 後的 `cudaDeviceSynchronize`。
- 每個客戶端重複編碼 JPEG 的效能問題。
- 改寫 git 歷史。

## Decisions

### D1. 移除 Blynk，設定只剩串流 port
刪除 token、事件代碼、`sendBlynkAlert` 與 `CMakeLists.txt` 的 CURL 依賴；偵測到移動時只在畫面上畫紅框並顯示 `MOTION`。`main()` 開頭用 `std::getenv` + `std::stoi`（捕捉例外、檢查整串都是數字與範圍）讀取 `BM_STREAM_PORT`，驗證失敗就 `return 1`，再開攝影機。
- 替代方案：把 Blynk 改成選用（有 token 才啟用）。目前已決定不用 Blynk，保留沒人用的程式碼只會增加維護成本；之後需要通知時再另開 change。
- 替代方案：設定檔。目前只有 1 個值，不需要。

### D2. 用 `MSG_NOSIGNAL` 處理 SIGPIPE，並全域忽略 SIGPIPE
`clientHandler` 中的 `write()` 全改成 `send(..., MSG_NOSIGNAL)`，失敗就結束該連線。另外在 `main()` 開頭 `signal(SIGPIPE, SIG_IGN)` 當保險，以防其他程式碼路徑寫入已關閉的 socket。

### D3. 用 atomic 旗標加 `shutdown()` 正常關閉
- 用 `sigaction` 註冊 SIGINT/SIGTERM，handler 只設 `std::atomic<bool> g_stop`（async-signal-safe）。
- 主迴圈改成 `while (!g_stop)`，移除 `cv::waitKey`（沒有 `imshow` 視窗就收不到按鍵）。
- `MJPEGWriter::stop()`：先設 `isRunning=false`，再 `shutdown(sock, SHUT_RDWR)` + `close(sock)`，讓阻塞中的 `accept()` 回傳錯誤，`listenHelper` 才結束，`join()` 不會卡住。
- 客戶端 thread 由 detached 改為記錄在 `std::vector<std::thread>`，並追蹤 client fd；`stop()` 對每個 fd 呼叫 `shutdown()` 再 join。這樣關閉時不會有 thread 存取已解構的物件。
- 主迴圈結束後依序執行 `streamer.stop()`、`cudaFree`、`cap.release()`，最後 `return 0`；攝影機讀不到影格時 `return 1`。
- 替代方案：改用 `poll()` 加逾時。`shutdown()` 改動比較小，也不需要額外的輪詢延遲。

### D4. 第一張影格只建立基準
CUDA 緩衝區改在第一張影格時依實際尺寸配置（以緩衝區是否已配置判斷是否為第一張）。第一張影格轉灰階後直接寫入 `d_prev_output`，然後 `continue`，不做差異比較也不判定移動。原本只用於警報冷卻的 `firstRun` 旗標隨 Blynk 一起移除。

### D6. 檢查影格尺寸
在 `cap >> frame` 之後檢查 `frame.cols/rows/channels()`，並確認 `isContinuous()`，任一項與初始化值不符就輸出錯誤並結束（非 0）。這可以防止 `cudaMemcpy` 依舊的 `colorSize` 讀寫越界。

### D7. Repo 清理
新增 `.gitignore`：`build/`、`.venv/`、`*.onnx`、`*.pt`、`*.engine`、`onnxruntime/`、`onnxruntime-*.tgz`、`runs/`。用 `git rm -r --cached build` 取消追蹤，本機檔案保留。

## Risks / Trade-offs

- [舊 token 仍留在 git 歷史，任何能讀 repo 的人都能對該 Blynk 裝置送事件] → 建議在 Blynk Console 作廢，寫進 tasks。
- [移除推播後，沒人盯著畫面時不會知道寶寶有動靜] → 已知取捨；之後需要時另開通知相關的 change。
- [這次對 CUDA 路徑的修正之後會被丟棄] → 只做必要的 D4、D6，成本很低。

## Migration Plan

1. 部署新版執行檔；需要非預設 port 時設定 `BM_STREAM_PORT`。
2. 建議在 Blynk Console 作廢舊 token。
3. 回滾：回到前一個 commit 即可（若舊 token 已作廢，回滾後的版本無法送出推播）。
