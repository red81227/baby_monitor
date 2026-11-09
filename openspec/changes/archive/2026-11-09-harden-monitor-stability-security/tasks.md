# Tasks

## 1. Repo 清理與憑證

- [x] 1.1 新增 `.gitignore`（`build/`、`.venv/`、`*.onnx`、`*.pt`、`*.engine`、`onnxruntime/`、`onnxruntime-*.tgz`、`runs/`），執行 `git rm -r --cached build`；驗證：`git status` 顯示 build/ 為刪除、`git ls-files build` 無輸出、本機 `build/` 仍存在
- [x] 1.2 移除 Blynk 整合：`main.cpp` 中寫死的 `BLYNK_AUTH_TOKEN` / `BLYNK_EVENT_CODE`、placeholder 檢查、`sendBlynkAlert` 與警報冷卻邏輯，以及 `CMakeLists.txt` 的 CURL 依賴；移動時畫面改顯示 `MOTION`；驗證：`git grep -i -e owygqfc -e blynk -e curl -- main.cpp CMakeLists.txt` 無結果，`readelf -d` 顯示執行檔沒有直接依賴 libcurl
- [ ] 1.3 （建議）到 Blynk Console 作廢已外洩的舊 token（需使用者手動完成）；驗證：使用者確認已完成，或明確決定略過

## 2. 執行期設定

- [x] 2.1 在 `main()` 開頭讀取並驗證 `BM_STREAM_PORT`（預設 8080，1–65535），驗證在開啟攝影機之前完成；驗證：`BM_STREAM_PORT=abc` 時輸出含變數名稱的錯誤並以非 0 結束；不設任何環境變數時串流在 8080；`BM_STREAM_PORT=9090` 時串流在 9090

## 3. 串流穩定性與正常關閉

- [x] 3.1 `main()` 開頭 `signal(SIGPIPE, SIG_IGN)`，`clientHandler` 的 `write` 改為 `send(..., MSG_NOSIGNAL)`；驗證：`curl -s http://localhost:8080/ > /dev/null` 執行幾秒後 Ctrl+C 中斷 curl，重複 5 次，程式仍在執行，且瀏覽器畫面持續更新
- [x] 3.2 加上 SIGINT/SIGTERM 的 `sigaction` handler（只設 atomic 旗標），主迴圈改為 `while (!g_stop)`，移除 `cv::waitKey`；驗證：Ctrl+C 與 `kill -TERM <pid>` 都能讓程式在 5 秒內以 0 結束（`echo $?`）
- [x] 3.3 `MJPEGWriter::stop()` 用 `shutdown()` 解除 `accept()` 阻塞；客戶端 thread 改為可 join，並追蹤其 fd，`stop()` 時逐一 shutdown 再 join；驗證：有瀏覽器連線時按 Ctrl+C，5 秒內結束，立刻重啟不會出現 bind 失敗
- [x] 3.4 攝影機開啟失敗時輸出含索引的錯誤並 `return 1`；讀到空影格時輸出攝影機中斷訊息，走正常關閉流程後 `return 1`；驗證：用 `cv::VideoCapture(99)` 或拔除攝影機時，看到預期訊息和結束碼

## 4. 移動偵測修正

- [ ] 4.1 第一張影格只轉灰階存入 `d_prev_output` 作為基準就 `continue`；驗證：畫面靜止時啟動 30 秒，串流畫面上從未出現 `MOTION` 或紅框；之後揮手時出現紅框
- [x] 4.2 每張影格檢查寬、高、通道數與 `isContinuous()` 是否符合初始化值，不符就輸出錯誤並以非 0 結束；驗證：程式碼審查確認檢查位於 `cudaMemcpy` 之前，正常執行時沒有誤觸發

## 5. 整合驗證

- [ ] 5.1 `cmake -S . -B build && cmake --build build` 編譯成功、沒有新的警告；實際跑過一輪：啟動不誤判、揮手出現紅框與 `MOTION`、兩個瀏覽器同時看串流、關掉其中一個不影響另一個、Ctrl+C 正常結束

## Workflow follow-up

- 本 change 驗證完成後先 archive，讓 `openspec/specs/` 產生主規格，再進行 `modularize-yolo26-detection`（該 change 以 MODIFIED 修改這裡建立的規格）。
