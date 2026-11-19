# Tasks

## 1. 停滯時自行結束

- [x] 1.1 `Config` 新增 `BM_STALL_EXIT_SECONDS`（預設 0；0 或 10–3600 的整數，無效時錯誤訊息含變數名稱）；`tests/test_config.cpp` 涵蓋預設值、合法值、`5`、`abc`、`4000`；README 環境變數表新增此項並說明容器部署中設為 60；`ctest -R config` 通過
- [ ] 1.2 實作停滯檢查（D7）：大於 0 時每秒檢查擷取與分析的 heartbeat，任一超過設定時間就印出停滯的部分與時間，嘗試正常關閉，3 秒內未結束則以非 0 結束碼強制結束；與 `SystemdNotifier` 共用新鮮度判斷；單元測試以可控制時間的 heartbeat 驗證「停滯時觸發一次結束回呼」與「持續 beat 時不觸發」；實機以 `BM_STALL_EXIT_SECONDS=10` 執行並拔掉攝影機 30 秒，確認不會結束且接回後恢復

## 2. monitor 映像

- [x] 2.1 新增 `.dockerignore`（排除 `build/`、`onnxruntime/`、`models/`、`logs/`、`events/`、`data/`、`.git/`、`.venv/`、`openspec/` 等）與 `docker/monitor.Dockerfile`（D2）：建置階段以 `ADD --checksum` 下載 ONNX Runtime 1.27 GPU（CUDA 12）並建置 `BabyMonitor` 與 `bm_replay_rules`；執行階段使用固定標籤的 `nvidia/cuda:12.x-cudnn-runtime-ubuntu24.04`，安裝執行所需的函式庫，以 `ldd` 確認執行檔與 `libonnxruntime_providers_cuda.so` 沒有缺少的函式庫；`docker build -f docker/monitor.Dockerfile .` 在乾淨的 repo 複本（`git worktree` 或 `git clone`，沒有 `build/`、`onnxruntime/`）中成功
- [x] 2.2 在容器中驗證推論：以 `docker run --gpus all` 掛載 `models/` 執行一次 BabyMonitor（或測試用的推論指令），日誌顯示 YOLO 以 CUDA 執行；記錄映像大小與建置時間於本 change 的 design.md「實測結果」

## 3. Compose 設定

- [x] 3.1 新增 `compose.yaml` 與 `.env.example`（D1、D3–D6、D8、D9）：`monitor` 與 `vlm` 兩個服務、GPU、攝影機裝置、`user` / `group_add`、掛載、只發佈 `monitor` 的 port、`BM_VLM_URL=http://vlm:8080`、`BM_STALL_EXIT_SECONDS=60`、`restart: unless-stopped`、`init: true`、健康檢查、日誌輪替、`/etc/localtime` 與 `TZ`；必要變數以 `${VAR:?}` 引用；`vlm` 使用固定 build 的官方 server-cuda 映像（實作時確認標籤存在與映像是否含 curl）；`.gitignore` 加入 `.env` 與 `/data/`；`docker compose config` 通過，刪掉 `VLM_MODEL_DIR` 時 `docker compose up` 在建立容器前失敗且訊息含變數名稱
- [x] 3.2 啟動與功能驗證：`docker compose up -d` 後 `docker compose ps` 兩者 healthy；監視頁面可開啟、`/api/situations` 顯示 VLM 可用、巡檢紀錄寫入 `${BM_DATA_DIR}/logs` 且擁有者為設定的 UID；從主機 `curl 127.0.0.1:8080` 與容器的 IP 都連不到 VLM（只有容器網路內可達）；主機修改規則檔 5 秒內熱重載；紀錄檔名日期與主機當地日期一致

## 4. 自動恢復與持久性

- [ ] 4.1 `docker compose kill vlm`（或在容器中結束 llama-server）後 `vlm` 自動重啟，期間頁面出現巡檢停止、恢復後巡檢恢復；`docker compose down` 再 `up -d` 後原本的警報與片段仍在；`.env` 改錯 VLM 模型檔時 `monitor` 照常提供頁面並顯示 VLM 無法連線（3.2 改為共用 network namespace 後需重測：`vlm` 當掉重啟與 `docker compose restart monitor` 後，`monitor` 仍連得到 VLM）
- [ ] 4.2 重新開機後（不做任何操作）兩個服務自動執行、頁面可開啟；`docker compose stop` 後重新開機不會自動啟動

## 5. 文件

- [x] 5.1 README 新增「以 Docker Compose 部署」：前置需求（Docker、Compose、NVIDIA Container Toolkit、驅動版本）、首次安裝步驟（`.env`、`mkdir -p data/...`、build、up）、日常操作表（啟動、停止、狀態、日誌、重啟單一服務、更新程式後重建、換 VLM 模型、除錯時呼叫 VLM 的方式）、資料目錄結構與備份、常見問題（攝影機被占用、GPU 記憶體不足、資料目錄權限）；原本的 systemd 段落改為「不使用 Docker 時」並保留；`cascade-alert-architecture` 的 README「VLM 服務」相關說明改為以 Compose 為主
- [x] 5.2 `openspec validate containerize-services --strict` 通過；主機上 `ctest` 全部通過

## Workflow follow-up

- 前端頁面改版另開 change（側邊欄、家長 / 除錯分開、紀錄可讀性、提醒；需要先確認「自訂影像關注區塊」是畫面顯示偏好還是修改 `bed_roi`，後者與「規則只能改檔案」的決定衝突）
- `cascade-alert-architecture` 的 7.1（VLM 模型比較）可以在 Compose 環境中進行，換模型只改 `.env`
