# Design

## Context

動機見 proposal.md 的 Why，行為要求見 `specs/`。和設計直接相關的現況：

- 主機：Ubuntu 24.04、RTX 3070 Ti Laptop（8 GB）、NVIDIA 驅動 595；已安裝 Docker 29.8、Compose v5.5、NVIDIA Container Toolkit 1.20，Docker 已註冊 `nvidia` runtime；`docker` 與 `containerd` 已設定開機啟動；使用者 UID/GID 1000，在 `docker` 群組；攝影機 `/dev/video0` 屬於 `video` 群組（GID 44）
- BabyMonitor 依賴：OpenCV 4.6（Ubuntu 套件）、libcurl、ONNX Runtime 1.27 GPU（CUDA 12 版，需要 cuDNN 9；目前由使用者手動下載解壓到 `onnxruntime/`，不在 git 中）、CMake FetchContent 下載的 Crow / asio / yaml-cpp
- 前端檔案從「執行檔所在目錄 / `frontend/`」讀取，規則檔與模型路徑由 `BM_*` 環境變數指定
- 程式已處理 SIGTERM（5 秒內結束）；有 systemd 時以 `Heartbeat`（擷取、分析）驅動 watchdog，擷取執行緒在攝影機重連期間也會更新 heartbeat
- 紀錄檔依**當地日期**分檔（`observations-` / `alerts-` / `patrols-YYYYMMDD.jsonl`）
- `BM_VLM_URL` 只接受 loopback 或私有網路位址；Compose 內部網路的位址在 172.16.0.0/12 內
- 目前的 VLM：`Qwen2.5-VL-3B-Instruct-Q4_K_M.gguf` + `mmproj-...-Q8_0.gguf`（約 2.6 GB），放在 `~/models/vlm/qwen2.5-vl-3b/`，以 `-c 4096 -np 1 -ngl all -fa on` 執行

## Goals / Non-Goals

**Goals:**
- `docker compose up -d` / `ps` / `logs` / `down` 就能完成日常管理
- 容器化後的行為與直接執行相同：CUDA 偵測、攝影機協商、規則熱重載、紀錄位置與內容
- 映像從原始碼可重現建置，所有外部下載固定版本

**Non-Goals:**
- 縮小映像到最小（CUDA 執行環境本身就有數 GB，優先正確與可維護）
- 開發用的容器（編譯與測試仍在主機上進行）
- 自動更新映像

## 整體配置

```
 repo/
   compose.yaml  .env (不進 git)  .env.example  .dockerignore
   docker/monitor.Dockerfile

 docker compose up -d
 +----------------------------------------------------------------------+
 | net (alpine, sleep)：持有共用的 network namespace                     |
 |   ports: ${BM_PORT}:18080   (唯一發佈到主機的 port)                   |
 |                                                                      |
 |  monitor  (network_mode: service:net)    vlm  (network_mode: service:net)|
 |   build: docker/monitor.Dockerfile        image: llama.cpp server-cuda|
 |   user: ${BM_UID}:${BM_GID} + video       (固定版本)                  |
 |   GPU, ${BM_CAMERA_DEVICE}                GPU                         |
 |   0.0.0.0:18080             --HTTP-->     127.0.0.1:8080 (只在 namespace 內)|
 |   BM_VLM_URL=http://127.0.0.1:8080        健康檢查 /health             |
 |   BM_STALL_EXIT_SECONDS=60                                            |
 |   健康檢查 /api/status                                                 |
 +----------------------------------------------------------------------+
   掛載 (主機 -> 容器)
     ./config                 -> /opt/babymonitor/config   (ro，熱重載只需要讀取)
     ./models                 -> /opt/babymonitor/models   (ro)
     ${BM_DATA_DIR}/logs      -> /var/lib/babymonitor/logs
     ${BM_DATA_DIR}/events    -> /var/lib/babymonitor/events
     ${VLM_MODEL_DIR}         -> /models                   (ro, vlm)
     /etc/localtime           -> /etc/localtime            (ro, 兩者)
```

## Decisions

### D1. 兩個容器、一個 Compose 專案

`monitor` 與 `vlm` 分開成兩個容器：各自重啟、各自更新（換 VLM 模型只重建 `vlm`），VLM 當掉不影響監看；這也延續 `cascade-alert-architecture` D5「VLM 以獨立行程執行」的決定。`monitor` 對 `vlm` 只設 `depends_on: condition: service_started`，**不等健康檢查**：VLM 載入要 10–30 秒，模型路徑錯誤時更永遠不會健康，監看不能因此延後或起不來（spec「VLM 起不來」）；VLM 尚未就緒期間由既有的巡檢失效處理涵蓋。

替代方案：單一容器內跑兩個行程（需要 supervisor，失去各自重啟與健康狀態）；systemd 管理兩個服務（使用者選擇 Docker；Docker 也讓 llama.cpp 不必在主機上編譯）。

### D2. monitor 映像：多階段建置

- **建置階段**：`ubuntu:24.04` + `build-essential cmake git unzip libopencv-dev libcurl4-openssl-dev`。以 `ADD --checksum=sha256:...` 從 NuGet 下載 `Microsoft.ML.OnnxRuntime.Gpu.Linux` 1.27.0（`.nupkg` 即 zip），把 `buildTransitive/native/include/` 與 `runtimes/linux-x64/native/` 整理成 `/opt/onnxruntime/{include,lib}`（補上 SONAME 需要的 `libonnxruntime.so.1` 連結），CMake 以 `-DORT_ROOT=/opt/onnxruntime` 指向它；只建置 `BabyMonitor` 與 `bm_replay_rules`，不建置測試。主機的 `build/`、`onnxruntime/` 由 `.dockerignore` 排除，確保乾淨建置
- **執行階段**：`nvidia/cuda:13.0.2-cudnn-runtime-ubuntu24.04`（固定版本，已確認標籤存在；NuGet 的 ONNX Runtime 1.27.0 是 CUDA 13 建置，見下方說明；實作時以 `docker manifest inspect` 確認標籤存在），只安裝執行所需的 OpenCV 函式庫（core、imgproc、imgcodecs、videoio、dnn；以執行檔實際的 NEEDED 為準）、`libcurl4t64`、`curl`（健康檢查用）、`tzdata`；複製 ONNX Runtime 的 `.so` 與程式到 `/opt/babymonitor/`（執行檔旁放 `frontend/`），工作目錄設為 `/opt/babymonitor`，讓 `BM_MODEL_PATH` 等相對路徑的預設值照常可用
- 以 `ldd` 檢查執行檔與 ONNX Runtime CUDA provider 沒有缺少的函式庫，作為建置的最後一步（缺少時建置失敗，而不是執行時才發現）

ONNX Runtime 來源改用 NuGet 的原因（實作時發現）：原訂的 GitHub release（`github.com/microsoft/onnxruntime`）從本機存取整個 repo 都回 404，其他 GitHub repo 正常。NuGet 套件與 GitHub tgz 比對：標頭檔內容相同（只差 CRLF）、`libonnxruntime.so` 與 `libonnxruntime_providers_shared.so` 完全相同；`libonnxruntime_providers_cuda.so` 是 **CUDA 13** 建置（GitHub tgz 是 CUDA 12，251 MB 對 366 MB；建置時的 `ldd` 檢查發現它需要 `libcudart.so.13`、`libcublas.so.13`），NuGet 沒有 1.27 的 CUDA 12 版，因此執行階段改用 CUDA 13 的基底映像。主機驅動 595 支援到 CUDA 13.2。能否在本機 GPU（sm_86）以 CUDA 執行由任務 2.2 實測確認。主機直接執行仍可沿用 README 的 tgz。

替代方案：沿用主機的 `onnxruntime/` 目錄（不可重現、建置內容依主機狀態而定）；以 `nvidia/cuda:*-devel` 當執行階段（多數 GB 不需要的編譯工具）。

### D3. vlm：官方 llama.cpp CUDA server 映像

使用 `ghcr.io/ggml-org/llama.cpp:server-cuda-<build>`（固定 build 編號，寫在 `.env.example` 的 `VLM_IMAGE`），映像的 entrypoint 就是 `llama-server`，Compose 的 `command` 傳入目前教學中驗證過的參數：

```
-m /models/${VLM_MODEL_FILE} --mmproj /models/${VLM_MMPROJ_FILE} -a ${VLM_MODEL_NAME}
--host 127.0.0.1 --port 8080 -c ${VLM_CTX:-4096} -np 1 -ngl all -fa on
```

只監聽 `127.0.0.1`，可達範圍見 D4。`monitor` 的 `BM_VLM_MODEL` 取自同一個 `VLM_MODEL_NAME`，兩邊不會不一致。健康檢查用映像內的 `curl` 打 `/health`（實作時確認映像含 curl，沒有時改用 llama-server 內建的健康檢查方式）。

替代方案：自行從原始碼建置 llama.cpp 映像（重現教學的 6 分鐘編譯，沒有必要）；Ollama（模型格式與管理方式不同，教學已建立 llama.cpp 的流程）。

### D4. 網路：只發佈監視頁面

VLM 的 API 沒有認證，必須**只有 `monitor` 連得到**。實作時發現原本的做法不夠：`vlm` 不發佈 `ports`、只在 Compose 的 bridge 網路中時，Linux 主機本身仍在該 bridge 上，從主機以容器 IP（例如 `172.20.0.2:8080`）就能連到 VLM；把網路設為 `internal: true` 也一樣（實測主機仍可連到）。

改為**共用 network namespace**（同 Kubernetes pod）：一個只執行 `sleep infinity` 的 `net` 容器（`alpine`，固定版本）持有 namespace 並發佈唯一的 port `"${BM_BIND:-0.0.0.0}:${BM_PORT:-18080}:18080"`；`monitor` 與 `vlm` 都以 `network_mode: "service:net"` 加入。`llama-server` 以 `--host 127.0.0.1` 只監聽這個 namespace 的 loopback，`monitor` 以 `BM_VLM_URL=http://127.0.0.1:8080` 呼叫（loopback，通過 `BM_VLM_URL` 的檢查，不需要修改程式）。主機的 127.0.0.1、容器 IP、區網與同網路的其他容器都連不到 VLM（實測），監視頁面照常由 `net` 的 port 提供。

持有者獨立成第三個容器，而不是讓 `vlm` 加入 `monitor`（或反過來）的 namespace：加入的容器在被加入的容器重啟後會失去網路，而 `monitor`（停滯結束）與 `vlm`（當掉）都會各自重啟；`net` 只執行 `sleep`，不會因程式問題重啟。代價：`monitor` 與 `vlm` 共用 port 空間（18080 與 8080 不衝突），`net` 被重建（例如改了 `BM_PORT`）時兩者也會被重建。

除錯時呼叫 VLM：`docker compose exec vlm curl -s http://127.0.0.1:8080/...`（以 `ports` 發佈無效，因為 VLM 只監聽 loopback）。

替代方案：llama-server 的 `--api-key` 加上 BabyMonitor 送出金鑰（仍可在 TCP 層連到，需要修改程式）；主機 iptables 規則（Docker 的 `DOCKER-USER` 不涵蓋主機自己發出的連線，需要另外維護主機防火牆）。

### D5. 檔案擁有者與裝置權限

`monitor` 以 `user: "${BM_UID}:${BM_GID}"` 執行，`group_add` 加入 `.env` 的 `BM_VIDEO_GID`（預設 44），裝置以 `devices: ["${BM_CAMERA_DEVICE}:${BM_CAMERA_DEVICE}"]` 傳入，容器內的 `BM_CAMERA_DEVICE` 與主機路徑相同。寫入的紀錄與片段因此屬於主機使用者，可以直接用檔案總管查看或刪除。資料目錄由 `.env` 的 `BM_DATA_DIR`（預設 `./data`）指定，第一次啟動前要存在且可寫入（README 的步驟中以 `mkdir -p` 建立；Docker 自動建立的目錄會屬於 root，因此不依賴它）。`vlm` 只讀模型，以映像預設的使用者執行。

### D6. 時區

兩個容器都掛載主機的 `/etc/localtime`（唯讀），並傳入 `TZ`（`.env` 的 `TZ`，預設由 README 指示設為主機時區）。紀錄檔名、頁面時間與主機一致。

### D7. 停滯時自行結束（BM_STALL_EXIT_SECONDS）

Docker 的重啟策略只在行程結束時生效，`unhealthy` 不會觸發重啟。BabyMonitor 已有擷取與分析的 `Heartbeat`，新增一個停滯檢查執行緒：每秒檢查兩個 heartbeat，任一的 `age()` 超過 `BM_STALL_EXIT_SECONDS` 時印出錯誤（哪一個停滯、停了多久），先嘗試正常關閉，3 秒內沒有結束就以 `_exit(2)` 強制結束（停滯的執行緒可能無法回應關閉）。擷取執行緒在攝影機重連時持續 beat，因此拔掉攝影機不會被當成停滯。與 `SystemdNotifier` 共用判斷函式 `allHeartbeatsFresh` 的邏輯；兩者可以同時啟用。Compose 中設為 60 秒。

替代方案：`autoheal` 之類監看 unhealthy 容器並重啟的 sidecar（多一個需要 Docker socket 權限的容器）；讓健康檢查失敗後由外部 cron 重啟（不可靠、難以追查）。

### D8. 健康檢查、重啟與日誌

- `restart: unless-stopped`：開機時 Docker 啟動後自動執行，使用者 `stop` / `down` 後不會自己起來
- 健康檢查：`monitor` 以 `curl -fsS http://127.0.0.1:18080/api/status`；`vlm` 以 `/health`；`start_period` 分別約 30 秒與 120 秒（VLM 載入模型）
- `init: true`：由 tini 轉送訊號與回收子行程
- `stop_grace_period: 10s`（BabyMonitor 5 秒內結束）
- 日誌：`json-file` 驅動，`max-size: 10m`、`max-file: 3`

### D9. 設定集中在 `.env`

`.env.example` 列出所有變數與說明，必要的變數在 `compose.yaml` 中以 `${VAR:?訊息}` 引用，缺少時 Compose 在建立容器前就失敗並指出變數名稱（spec「缺少設定」）。BabyMonitor 本身的 `BM_*` 設定中與部署無關的（偵測門檻等）維持程式預設值，需要時在 `.env` 加上，再於 `compose.yaml` 的 `environment` 中轉傳。

主要變數：`NET_IMAGE`、`BM_PORT`、`BM_BIND`、`BM_CAMERA_DEVICE`、`BM_VIDEO_GID`、`BM_UID`、`BM_GID`、`BM_DATA_DIR`、`TZ`、`VLM_IMAGE`、`VLM_MODEL_DIR`（必填）、`VLM_MODEL_FILE`、`VLM_MMPROJ_FILE`、`VLM_MODEL_NAME`、`VLM_CTX`。

## Risks / Trade-offs

- [容器中的 ONNX Runtime 1.27（NuGet，CUDA 13 建置）需要的 CUDA / cuDNN 小版本與基底映像不符，執行時才載入失敗] → 建置最後以 `ldd` 檢查 CUDA provider；實作時以 `bus.jpg` 在容器中跑一次推論確認是 CUDA
- [官方 llama.cpp 映像的 CUDA 版本比主機驅動支援的新] → 驅動 595 支援 CUDA 13.x；固定映像版本，換版時先確認
- [映像體積大（數 GB），首次建置與下載慢] → 只在首次與更新時發生；`.dockerignore` 排除 build、模型、資料，避免把 GB 級檔案送進建置 context
- [兩個容器共用 8 GB GPU，VLM 換更大的模型時記憶體不足] → 與直接執行相同；README 的換模型步驟提醒先看 `nvidia-smi`
- [掛載的資料目錄不存在時 Docker 會以 root 建立，之後容器無法寫入] → README 步驟先 `mkdir -p`；BabyMonitor 已在無法寫入時印出警告
- [攝影機被其他程式（例如手動執行的 BabyMonitor）占用] → 容器啟動失敗並依重啟策略重試，日誌顯示攝影機無法開啟；README 提醒先停止手動執行的實例
- [VLM 沒有對外 port，教學與除錯不方便] → `docker compose exec` 或 override 檔（D4）
- [停滯結束可能在除錯時（例如 gdb 暫停）誤觸] → 只在 Compose 中預設啟用，直接執行時預設關閉

## Migration Plan

1. 停止手動執行的 BabyMonitor 與 llama-server（教學中在終端機 A、B 的行程）
2. `cp .env.example .env`，填入 `VLM_MODEL_DIR`、`BM_UID` / `BM_GID`（`id -u` / `id -g`）、`TZ`；`mkdir -p data/logs data/events`
3. `docker compose build` → `docker compose up -d` → `docker compose ps` 確認兩者 healthy
4. 舊的紀錄與片段（repo 的 `logs/`、`events/`）若要保留，搬到 `data/`
5. 回復：`docker compose down` 後照原本方式手動執行或使用 systemd，兩者讀取相同的規則檔與資料目錄格式

## 實測結果

2026-11-19，本機（RTX 3070 Ti Laptop、驅動 595.99、Docker 29.8）：

- `monitor` 映像（`docker/monitor.Dockerfile`，執行階段 `nvidia/cuda:13.0.2-cudnn-runtime-ubuntu24.04`）：6.24 GB（壓縮後內容 2.4 GB）
- 建置時間：在乾淨的 repo 複本（無 `build/`、`onnxruntime/`）中首次建置 186 秒（含 apt、NuGet 下載、CMake FetchContent、編譯與基底映像下載）；建置階段有快取、只更換執行階段基底映像時 127 秒
- 建置最後的 `ldd` 檢查有效：第一次以 CUDA 12.9 基底映像建置時，檢查發現 NuGet 的 CUDA provider 缺少 `libcudart.so.13` 等函式庫而讓建置失敗，因此改用 CUDA 13 基底映像
- 容器中推論：`docker run --gpus all --device /dev/video0 --group-add 44 --user 1000:1000` 掛載 `models/` 執行，日誌顯示 `YOLO 模型已載入: models/yolo26n-pose.onnx (執行裝置: CUDA, 類型: pose)`，攝影機以 `MJPG 1280x720 @30fps` 開啟（與直接執行相同），`/api/status` 正常回應，寫出的紀錄檔擁有者為 1000:1000
