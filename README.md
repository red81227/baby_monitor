# BabyMonitor

用 USB 攝影機看著寶寶的監視器：以 YOLO26n-pose（ONNX Runtime，可用 CUDA）偵測畫面中的人物與骨架關鍵點，人物位置累積位移超過門檻就判定為「有移動」，並透過網頁即時顯示畫面與狀態。

- 攝影機以 MJPG 擷取，網頁直接顯示攝影機輸出的 JPEG（不重新編碼）；人物框由網頁依偵測結果畫上：靜止為**綠框**、移動為**紅框**。
- 畫面靜止時人物偵測約每秒 1 次，畫面有動靜時提高到每秒 10 次（可設定）。
- 只在本機網路提供服務，**沒有任何認證**，請只在信任的網路中使用，不要把 port 轉發到網際網路。

## 需求

| 項目 | 版本 / 說明 |
|---|---|
| Linux x86_64 | 已在 Ubuntu 24.04 測試 |
| CMake | 3.20 以上 |
| C++17 編譯器 | GCC 11 以上 |
| OpenCV | 4.x（`sudo apt install libopencv-dev`） |
| ONNX Runtime | 1.27.0 GPU 版（CUDA 12），見下方（以 Docker Compose 部署時不需要，映像建置時自動下載） |
| CUDA 12 runtime + cuDNN 9 | 選用；沒有時自動改用 CPU（較慢） |
| libcurl | 呼叫本機 VLM 服務（`sudo apt install libcurl4-openssl-dev`） |
| 網路 | 第一次 configure 時會用 FetchContent 下載 Crow v1.2.1、asio 1.30.2 與 yaml-cpp 0.8.0 |

### ONNX Runtime

```bash
wget https://github.com/microsoft/onnxruntime/releases/download/v1.27.0/onnxruntime-linux-x64-gpu_cuda12-1.27.0.tgz
mkdir onnxruntime && tar -xzf onnxruntime-linux-x64-gpu_cuda12-1.27.0.tgz -C onnxruntime --strip-components=1
```

預設路徑是 repo 根目錄的 `onnxruntime/`，也可以用 `-DORT_ROOT=<路徑>` 指定。執行檔會以 RPATH 找到 `onnxruntime/lib`，不需要設定 `LD_LIBRARY_PATH`。

### YOLO26n-pose 模型

用 [Ultralytics](https://docs.ultralytics.com/) 匯出內含 NMS 的 ONNX：

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install ultralytics onnx
yolo export model=yolo26n-pose.pt format=onnx imgsz=640 end2end=True
mkdir -p models && mv yolo26n-pose.onnx yolo26n-pose.pt models/
```

`end2end=True` 不可省略：pose 模型預設匯出的是未經 NMS 的 `[1, 56, 8400]`，程式不支援，啟動時會印出模型路徑與輸出形狀後結束。正確的輸出是 `[1, 300, 57]`（框、分數、類別，加上 17 個關鍵點）。

也可以改用不含關鍵點的純偵測模型（`yolo export model=yolo26n.pt format=onnx imgsz=640`，輸出 `[1, 300, 6]`），以 `BM_MODEL_PATH` 指定；此時網頁只畫框、不畫骨架。

**從舊版升級**：預設模型已從 `models/yolo26n.onnx` 改為 `models/yolo26n-pose.onnx`。升級前先依上面的步驟匯出 pose 模型；或設定 `BM_MODEL_PATH` 指向原本的 `yolo26n.onnx` 繼續使用純偵測模型。

`onnxruntime/` 與 `*.onnx` 都已列在 `.gitignore`，不會進版本控制。

## 建置與測試

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`yolo_inference` 測試會用 `tests/data/bus.jpg` 跑真實模型；找不到 `models/yolo26n-pose.onnx` 時會標示為 Skipped；`models/yolo26n.onnx` 存在時也會一併測試純偵測模型。

## 執行

```bash
./build/BabyMonitor
```

開啟 `http://<主機 IP>:18080/` 即可看到即時畫面。也可以從任何工作目錄啟動（前端檔案從執行檔旁的 `frontend/` 讀取），但 `BM_MODEL_PATH` 的預設值是相對於**工作目錄**的 `models/yolo26n-pose.onnx`。

啟動時會印出實際使用的攝影機格式與解析度，例如：

```
[INFO] 攝影機: /dev/video0 格式 MJPG 1280x720 @30fps
```

- 啟動時攝影機開不了（裝置不存在、沒有權限、不支援 MJPG）：印出包含裝置路徑的錯誤並以 1 結束。
- 執行中攝影機中斷（例如 USB 被拔除）：程式**不會結束**，`/api/status` 的 `cameraConnected` 變成 `false`、網頁顯示攝影機中斷，並以 250ms、500ms、1s、2s、4s、之後每 5s 的間隔重新開啟同一個裝置；恢復後 `seq` 從中斷前繼續。
- 按 Ctrl+C 或送 SIGTERM 會關閉網頁服務與所有即時影像連線，並在 5 秒內以 0 結束（攝影機重連中也一樣）。

### HTTP 介面

| 路徑 | 說明 |
|---|---|
| `GET /` | 監視頁面 |
| `GET /api/status` | JSON，欄位見下表 |
| `GET /api/frame.jpg` | 最新影格的攝影機原圖（**沒有畫框**），`X-Frame-Seq` 標頭為其 `seq`；還沒有影格時回 503 |
| `GET /ws/live` | WebSocket 即時影像與偵測結果，格式見下方 |
| `GET /api/situations` | JSON，目前的規則（路徑、`version`、`hash`、載入時間、載入錯誤 `error`、警告）、VLM 狀態（`vlm.enabled`、`vlm.available`、`vlm.model`、`vlm.error`）、每個狀況的 `id`、`severity`、`route`、`shadow`、`enabled`、`active`（目前是否成立）、條件文字；每個巡檢（`patrols[]`）的 `id`、`severity`、`everyS`、`frames`、`confirm`、`cooldownS`、`shadow`、`enabled`、`while`（前提條件文字）、`paused`（前提條件不成立）、`stalled` 與 `stallReason`（巡檢停止）、`inFlight`、`positives`（目前連續回覆需要提醒的次數）、`lastOkEpochMs`（最後一次成功，0 = 尚未成功）、`lastAlert`、`lastCause`、`lastError`；ROI（`bedRoi` 生效的 `[x, y, w, h]` 或 `null`、`bedRoiSource` 為 `override` / `rules` / `none`、`fileBedRoi` 規則檔的值、`bedEdgeMargin`）；以及最新的訊號值 |
| `GET /api/alerts` | JSON，最近 100 筆觸發與警報（新的在前），見下表 |
| `GET /api/events/<id>` | 事件片段的 `meta.json`（影格清單、`seq`、擷取時間）；id 格式不符回 400，不存在回 404 |
| `GET /api/events/<id>/<NNNNNN>.jpg` | 事件片段中的一張影格（原始 JPEG） |
| `PUT /api/roi` | 從網頁設定嬰兒床 ROI，見「ROI 設定」 |
| `DELETE /api/roi` | 刪除網頁設定的 ROI，回到規則檔的 `bed_roi`，見「ROI 設定」 |

除了 `/api/roi` 之外的 API 都是唯讀的；規則只能以修改規則檔的方式變更，ROI 是唯一的例外。

`/api/status` 欄位：

| 欄位 | 說明 |
|---|---|
| `motionDetected` | 最近一次偵測是否判定為移動 |
| `personDetected` | 最近一次偵測是否有人物 |
| `lastMotionEpochMs` | 最後一次移動的 Unix 毫秒時間；沒有過移動時為 0 |
| `latestFrameBytes` | 最新影格的 JPEG 大小 |
| `cameraConnected` | 攝影機是否連線中 |
| `seq` | 最新影格的序號（從 1 開始遞增，攝影機重連後延續）；還沒有影格時為 0 |
| `detectHz` | 最近 3 秒實際的人物偵測頻率（每秒次數） |
| `rulesVersion` | 目前使用的規則檔 `version` |
| `rulesError` | 最近一次重新載入規則失敗的原因；沒有錯誤時為 `null` |
| `lastAlertEpochMs` | 最後一次送出警報的 Unix 毫秒時間；沒有過警報時為 0 |

`/api/alerts` 的每一筆：

| 欄位 | 說明 |
|---|---|
| `id` | 觸發 id |
| `situation`、`severity`、`route`、`shadow` | 狀況或巡檢與觸發當下的設定。`route` 為 `patrol` 表示巡檢觸發；`system` 表示系統警報或紀錄，`situation` 為 `patrol_stalled:<巡檢 id>`（巡檢停止，`warning`）、`patrol_recovered:<巡檢 id>`（已恢復，`info`）或 `roi_changed`（ROI 由網頁修改，`info`，`cause` 含新舊值與來源 IP），不會被壓制 |
| `epochMs` | 觸發時間（Unix 毫秒） |
| `delivery` | `sent` 已送出、`suppressed` 被壓制（`suppressedBy` 為壓制來源）、`shadow` 影子模式、`no_alert` VLM 判定不需要提醒、`dropped` 判定失敗且 `on_timeout: drop`、`pending` 等待片段或判定、`suppressor` 本身是 suppress 狀況、`logged` 系統紀錄（不是警報，不跳出提醒） |
| `deliveredEpochMs` | 送出時間；未送出為 0 |
| `viaLlm` | 是否經 VLM 判定決定提醒與否（`direct` 路由為 false，巡檢觸發為 true） |
| `cause` | 原因說明；VLM 判定失敗時為「未經 LLM 判定 (...)」 |
| `llm` | VLM 判定結果（`status`、`alert`、`cause`、`model`、`latencyMs`、`error`、`raw` 原始回覆）；沒有送 VLM 時為 `null` |
| `signals` | 觸發當下的訊號值 |
| `rulesVersion`、`rulesHash` | 觸發時使用的規則 |
| `clipId` | 事件片段 id（`/api/events/<clipId>`） |

### 即時影像（`/ws/live`）

連上後伺服器持續推送兩種訊息，直到客戶端斷線或服務關閉：

- **binary：影格**。`[u32 magic][u64 seq][u64 tCaptureUs][JPEG]`，little-endian，標頭 20 bytes；`magic` 為 `0x314D4246`（ASCII `FBM1`），`tCaptureUs` 為擷取當下的 monotonic 時間（微秒，只用來比較先後與間隔）。JPEG 是攝影機輸出的原圖，沒有任何標示。
- **text：偵測結果**（每次人物偵測完成後送出，剛連上時會先送最近一筆）：

  ```json
  {"type":"detection","seq":1234,"tCaptureUs":5844012345,"person":true,
   "box":[412.0,180.5,220.0,410.0],"confidence":0.873,"motion":false,"width":1280,"height":720,
   "keypoints":[[520.5,231.0,0.98],[530.1,222.4,0.95], ...共 17 組... ,[480.0,588.2,0.12]]}
  ```

  `box` 為 `[x, y, 寬, 高]`，以影格原圖（`width` x `height`）的像素為單位；沒有人物時 `person` 為 `false`、`box` 為 `null`。

  `keypoints` 為 17 組 `[x, y, 可見度]`，依 COCO 順序：0 鼻、1 左眼、2 右眼、3 左耳、4 右耳、5 左肩、6 右肩、7 左肘、8 右肘、9 左腕、10 右腕、11 左髖、12 右髖、13 左膝、14 右膝、15 左踝、16 右踝（「左右」指被拍攝者本人的左右）。`x`、`y` 與 `box` 同樣以影格原圖的像素為單位，並限制在影格範圍內；可見度為 0–1，被遮住的部位可見度低、位置是模型推測的。沒有人物、或使用純偵測模型時為 `null`。

客戶端**每收到一個 binary 訊息就要回一個 text 訊息 `ack`**。伺服器對每個連線最多只有 1 張影格在送，ack 之前產生的影格只保留最新一張，所以網路慢的客戶端只會跳格、不會累積延遲，也不影響其他客戶端。影格送出後 10 秒都沒有 ack 的連線會被關閉。

### 環境變數

| 變數 | 預設值 | 說明 |
|---|---|---|
| `BM_MODEL_PATH` | `models/yolo26n-pose.onnx` | YOLO26n 系列 ONNX 模型路徑（pose 或純偵測，見「YOLO26n-pose 模型」） |
| `BM_PORT` | `18080` | 網頁與 API 的 port（1–65535） |
| `BM_MOTION_PX` | `20` | 人物中心累積位移超過幾個像素算移動（正數）；以**主碼流**（攝影機實際解析度）的像素計，見下方說明 |
| `BM_PERSON_CONF` | `0.45` | 人物偵測的信心門檻（0–1）；包著被子的嬰兒偵測不到時可以調低 |
| `BM_CAMERA_DEVICE` | `/dev/video0` | 攝影機裝置路徑（V4L2，需支援 MJPG） |
| `BM_CAPTURE_WIDTH` | `1280` | 要求的擷取寬度（1–8192）；攝影機不支援時使用協商出的最接近解析度，實際值會印在啟動訊息 |
| `BM_CAPTURE_HEIGHT` | `720` | 要求的擷取高度（1–8192） |
| `BM_DETECT_MIN_HZ` | `1` | 畫面靜止時的人物偵測頻率（每秒次數，大於 0） |
| `BM_DETECT_MAX_HZ` | `10` | 畫面有動靜時的人物偵測頻率（每秒次數，不可小於 `BM_DETECT_MIN_HZ`）；兩者相同即為固定頻率 |
| `BM_RULES_PATH` | `config/situations.yaml` | 狀況規則檔路徑（相對於工作目錄），見「狀況規則」 |
| `BM_VLM_URL` | （空，停用） | 本機 VLM 服務的網址（OpenAI 相容 API，例如 `http://127.0.0.1:8080`；Docker Compose 部署中為 `http://127.0.0.1:8080`，見「以 Docker Compose 部署」）；只接受本機或私有網路位址，主機名稱會解析後檢查。未設定時 `llm` 路由依 `on_timeout` 處理，巡檢無法進行並送出「巡檢停止」警報 |
| `BM_VLM_MODEL` | （空） | 送給 VLM 服務的模型名稱 |
| `BM_RING_SECONDS` | `60` | 主碼流環形緩衝長度（1–3600 秒）；事件片段的前段不可超過此值。緩衝存原始 JPEG，720p 約 2–5 MB/秒，60 秒約占 120–300 MB 記憶體 |
| `BM_EVENT_DIR` | `events` | 事件片段目錄 |
| `BM_EVENT_MAX_MB` | `2048` | 事件片段占用空間上限（MB），超過時從最舊的刪除 |
| `BM_LOG_DIR` | `logs` | 觀測紀錄與警報紀錄目錄 |
| `BM_OVERRIDES_DIR` | `overrides` | 網頁設定的保存目錄（目前只有 ROI，見「ROI 設定」）；第一次從網頁儲存時建立。必須可由服務的使用者寫入，容器部署時要掛載為可寫入的主機目錄，否則重建容器後設定會消失 |
| `BM_STALL_EXIT_SECONDS` | `0` | 停滯自行結束：大於 0 時，擷取或分析停滯超過此秒數就以非 0 結束碼結束，交由外部重啟（容器的重啟策略）。0 = 停用，否則為 10–3600。攝影機重連期間不算停滯。Docker Compose 部署中設為 `60`；systemd 部署用 watchdog 即可，不需設定 |

任何值無效時，程式會在開啟攝影機前印出包含變數名稱的錯誤並以 1 結束。

`BM_MOTION_PX` 以主碼流像素計算，同樣的實際位移在解析度越高時像素數越大：從 640x480 改成 1280x720 時，若要維持原本的靈敏度，大約要把 `BM_MOTION_PX` 加倍（例如 20 → 40）。

### 不使用 Docker 時：以 systemd 執行

建議以「以 Docker Compose 部署」執行。不使用 Docker 時，可以直接執行或用下列 systemd 設定；兩種方式讀取相同的規則檔與資料格式，可以互相切換（先停止另一種）。

本機 VLM 需要另外啟動 llama.cpp 的 `llama-server`（只綁 127.0.0.1，API 沒有認證），並設定 `BM_VLM_URL=http://127.0.0.1:8080`、`BM_VLM_MODEL=qwen2.5-vl-3b`：

```bash
llama-server -m ~/models/vlm/qwen2.5-vl-3b/Qwen2.5-VL-3B-Instruct-Q4_K_M.gguf \
  --mmproj ~/models/vlm/qwen2.5-vl-3b/mmproj-Qwen2.5-VL-3B-Instruct-Q8_0.gguf \
  -a qwen2.5-vl-3b --host 127.0.0.1 --port 8080 -c 4096 -np 1 -ngl all -fa on
```


`/etc/babymonitor.env`：

```ini
BM_MODEL_PATH=/opt/babymonitor/models/yolo26n-pose.onnx
BM_PORT=18080
BM_PERSON_CONF=0.45
BM_RULES_PATH=/opt/babymonitor/config/situations.yaml
BM_LOG_DIR=/var/lib/babymonitor/logs
BM_EVENT_DIR=/var/lib/babymonitor/events
BM_OVERRIDES_DIR=/var/lib/babymonitor/overrides
# BM_VLM_URL=http://127.0.0.1:8080
```

規則檔不存在或驗證失敗時服務無法啟動，部署時請把 repo 的 `config/`（`situations.yaml` 與 `prompts/`）一併複製到 `BM_RULES_PATH` 所在目錄；`BM_LOG_DIR`、`BM_EVENT_DIR`、`BM_OVERRIDES_DIR` 必須可由服務的使用者寫入。

`/etc/systemd/system/babymonitor.service`：

```ini
[Unit]
Description=BabyMonitor
After=network.target

[Service]
Type=notify
WatchdogSec=30
EnvironmentFile=/etc/babymonitor.env
ExecStart=/opt/babymonitor/build/BabyMonitor
Restart=on-failure
User=babymonitor
SupplementaryGroups=video

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now babymonitor
```

- `Type=notify`：程式在網頁可以連線後才送出 `READY=1`，`systemctl start` 會等到那時才返回。
- `WatchdogSec=30`：影像擷取（含攝影機重連）與偵測都在運作時，程式每 15 秒送一次 `WATCHDOG=1`；任一停滯超過 30 秒就停止送出，由 systemd 判定逾時並依 `Restart=on-failure` 重啟。
- 攝影機中斷時程式在內部重連、不會結束，所以不會觸發 `Restart`；要知道攝影機是否中斷請看 `/api/status` 的 `cameraConnected`。
- 直接執行（沒有 systemd）時這些通知會自動停用。

## 以 Docker Compose 部署

建議的部署方式：一個指令啟動 `monitor`（BabyMonitor）與 `vlm`（llama.cpp 的 `llama-server`，本機 VLM）兩個容器（另有一個只持有網路的 `net` 容器），開機自動執行、異常結束自動重啟。設定都在 repo 根目錄的 `.env`。

```
 docker compose up -d
 ┌──────── net：共用的 network namespace，只發佈 ${BM_PORT} → 18080 ────────┐
 │  monitor (GPU、攝影機)                    vlm (GPU)                      │
 │   0.0.0.0:18080  ──HTTP 127.0.0.1:8080──▶  127.0.0.1:8080 (只在 namespace 內) │
 └─────────────────────────────────────────────────────────────────────────┘
   ./config → 規則檔 (唯讀，熱重載)        ${VLM_MODEL_DIR} → VLM 模型 (唯讀)
   ./models → YOLO 模型 (唯讀)             ${BM_DATA_DIR}/logs、events → 紀錄與片段
```

VLM 的 API 沒有認證，因此只有 `monitor` 連得到：`monitor` 與 `vlm` 共用 `net` 容器的 network namespace（同 Kubernetes pod），`llama-server` 只監聽其中的 127.0.0.1。主機（含容器 IP）、區網與其他容器都連不到 VLM。`net` 只執行 `sleep`，讓 `monitor` 或 `vlm` 各自重啟時不影響另一個的網路。

### 前置需求

| 項目 | 說明 |
|---|---|
| Docker Engine + Compose v2 | `docker compose version` 可執行；使用者在 `docker` 群組；`docker` 服務設為開機啟動（`sudo systemctl enable docker`） |
| NVIDIA Container Toolkit | `docker run --rm --gpus all nvidia/cuda:13.0.2-base-ubuntu24.04 nvidia-smi` 能看到 GPU |
| NVIDIA 驅動 | 支援 CUDA 13 的版本（580 以上；已在 595 測試）。`monitor` 映像使用 CUDA 13 + cuDNN 9 |
| YOLO 模型 | `models/yolo26n-pose.onnx`，見「YOLO26n-pose 模型」 |
| VLM 模型 | GGUF 模型與 mmproj 檔（預設 Qwen2.5-VL-3B-Instruct Q4_K_M，約 2.6 GB），放在同一個目錄 |
| 網路 | 首次建置需要下載基底映像、ONNX Runtime（NuGet）與 CMake FetchContent 的套件；主機上不需要 `onnxruntime/` 或 `build/` |

### 首次安裝

```bash
# 1. 停止手動執行的 BabyMonitor 與 llama-server (攝影機與 GPU 記憶體不能被占用)

# 2. 設定：填入 BM_UID / BM_GID (id -u / id -g)、TZ (timedatectl show -p Timezone --value)、VLM_MODEL_DIR
cp .env.example .env
$EDITOR .env

# 3. 資料目錄：必須先建立 (Docker 自動建立的目錄屬於 root，容器無法寫入)
mkdir -p data/logs data/events data/overrides

# 4. 建置與啟動 (首次建置約 3–5 分鐘，llama.cpp 映像下載數 GB)
docker compose build
docker compose up -d

# 5. 確認兩者都是 healthy (vlm 載入模型最多約 2 分鐘)
docker compose ps
```

開啟 `http://<主機 IP>:18080/`。`/api/situations` 的 `vlm.available` 為 `true` 表示巡檢可以進行。

### 日常操作

| 要做的事 | 指令 |
|---|---|
| 啟動 / 重新套用設定 | `docker compose up -d`（只重建設定有變的服務） |
| 停止（保留容器） | `docker compose stop`（重新開機後**不會**自動啟動，直到再次 `up -d` 或 `start`） |
| 停止並移除容器 | `docker compose down`（資料目錄、規則檔、模型都保留） |
| 狀態與健康檢查 | `docker compose ps`（`monitor`、`vlm` 有健康檢查；`net` 只持有網路，沒有健康檢查） |
| 日誌 | `docker compose logs -f monitor`、`docker compose logs -f vlm`（每個容器最多保留 3 × 10 MB） |
| 重啟單一服務 | `docker compose restart vlm` |
| 更新程式後重建 | `git pull && docker compose build monitor && docker compose up -d monitor` |
| 修改規則或 prompt | 直接編輯 `config/` 中的檔案，5 秒內熱重載，不需要重啟 |
| 設定嬰兒床 ROI | 在監視頁面按「編輯 ROI」框選後儲存，立即生效；存在 `data/overrides/bed_roi.json`（見「ROI 設定」） |
| 換 VLM 模型 | 把模型放到 `VLM_MODEL_DIR`（或改這個目錄），修改 `.env` 的 `VLM_MODEL_FILE`、`VLM_MMPROJ_FILE`、`VLM_MODEL_NAME`，執行 `docker compose up -d`：`vlm` 重新建立，`monitor` 也以新的模型名稱重新建立。換更大的模型前先用 `nvidia-smi` 確認 GPU 記憶體夠兩個容器共用 |
| 除錯時呼叫 VLM | VLM 只監聽共用 namespace 的 127.0.0.1，從容器內呼叫：`docker compose exec vlm curl -s http://127.0.0.1:8080/v1/models`（以 `ports` 發佈無效） |

### 資料目錄與備份

```
data/                       (.env 的 BM_DATA_DIR，擁有者是 BM_UID)
  logs/
    observations-YYYYMMDD.jsonl   觀測紀錄 (約 130 MB/天，保留 14 天)
    alerts-YYYYMMDD.jsonl         警報紀錄
    patrols-YYYYMMDD.jsonl        巡檢紀錄
  events/
    <YYYYMMDD-HHMMSS>-<狀況>-<NNN>/   事件片段 (JPEG + meta.json，總量受 BM_EVENT_MAX_MB 限制)
  overrides/
    bed_roi.json                  網頁設定的 ROI (見「ROI 設定」；刪除即還原為規則檔)
```

紀錄檔依**主機當地日期**分檔（容器掛載主機的 `/etc/localtime` 並使用 `.env` 的 `TZ`）。備份時複製 `data/`、`config/` 與 `.env` 即可；不需要停止服務（紀錄檔是逐行附加）。舊的直接執行留下的 `logs/`、`events/`、`overrides/` 要保留時，搬到 `data/` 對應的目錄。從沒有 `overrides` 掛載的舊版升級時，先執行 `mkdir -p data/overrides` 再 `docker compose up -d`。

容器中設定了 `BM_STALL_EXIT_SECONDS=60`：擷取或分析停滯超過 60 秒時程式自行結束，由 `restart: unless-stopped` 重新啟動（Docker 的健康檢查失敗本身不會觸發重啟）。

### 常見問題

- **`monitor` 一直重啟，日誌顯示攝影機無法開啟**：攝影機被其他程式占用（例如手動執行的 BabyMonitor、瀏覽器、`cheese`）。以 `fuser -v /dev/video0` 找出並停止它。裝置路徑不是 `/dev/video0` 時修改 `.env` 的 `BM_CAMERA_DEVICE`，群組不是 44 時修改 `BM_VIDEO_GID`（`stat -c %g /dev/video0`）。
- **`vlm` 一直 unhealthy 或重啟**：看 `docker compose logs vlm`。模型檔名或 `VLM_MODEL_DIR` 錯誤時會找不到檔案；`out of memory` 表示 GPU 記憶體不足：先用 `nvidia-smi` 確認沒有其他程式占用，或改用較小的模型 / 調低 `VLM_CTX`。`vlm` 起不來時 `monitor` 照常監看，頁面會顯示 VLM 無法連線與巡檢停止。
- **紀錄沒有寫出、日誌有無法寫入的警告**：資料目錄屬於 root（啟動前沒有先 `mkdir -p`，被 Docker 自動建立）。執行 `sudo chown -R $(id -u):$(id -g) data`，並確認 `.env` 的 `BM_UID` / `BM_GID` 與目錄擁有者一致。
- **在頁面儲存 ROI 時顯示「無法寫入 ROI 檔」**：`data/overrides` 不存在（被 Docker 以 root 建立）或擁有者不是 `BM_UID`。執行 `mkdir -p data/overrides && sudo chown -R $(id -u):$(id -g) data` 後重試；監看不受影響。
- **`docker compose up` 顯示 `required variable ... is missing a value`**：`.env` 缺少必填設定（`BM_UID`、`BM_GID`、`TZ`、`VLM_MODEL_DIR`），依訊息補上。
- **重新開機後沒有自動啟動**：確認 `systemctl is-enabled docker` 是 `enabled`；若之前執行過 `docker compose stop` 或 `down`，需要再 `docker compose up -d` 一次。

## 狀況判斷

監視器以兩段式判斷「狀況」：第一段由人物偵測與移動分數算出下列**訊號**，依規則檔觸發狀況；第二段（路由為 `llm` 的狀況）由本機 VLM 看事件前後的畫面判斷原因並決定是否提醒。第一段訊號偵測不到的狀況（口鼻被蓋住）由 VLM **定期巡檢**直接看畫面判斷，見「巡檢」。

### 訊號

每張分析影格都會產生一組訊號。人物相關的訊號只在執行人物偵測時更新，兩次偵測之間沿用上一次的值；上一次偵測距今超過 3 秒時視為**未知**。偵測不到人物時，`person_present` 為 false、`person_count` 為 0，其餘人物訊號為未知。條件引用未知的訊號時一律不成立。

| 訊號 | 型別 | 定義 |
|---|---|---|
| `person_present` | 布林 | 是否偵測到人物 |
| `person_count` | 數字 | 信心值達到 `BM_PERSON_CONF` 的人物數量 |
| `person_conf` | 數字 | 信心值最高的人物的信心值（0–1） |
| `face_visible` | 布林 | 鼻、左眼、右眼任一可見度達到門檻（需要姿態模型）。**無法偵測口鼻被蓋住**：姿態模型沒有嘴巴關鍵點，被遮住的鼻子也會依眼睛與耳朵的位置被推算出來並給出高可見度（實測口鼻被蓋住時鼻子可見度仍有 0.98–1.00），因此只有整張臉或雙眼都被蓋住時才會變成 false；口鼻被蓋住由 `airway_covered` 巡檢負責 |
| `body_visible` | 布林 | 雙肩或雙髖任一可見度達到門檻（需要姿態模型） |
| `prone_likely` | 布林 | `body_visible`，且鼻與雙眼都低於門檻、雙耳至少一個達到門檻（看得到頭側卻看不到臉）；尚未以真實資料驗證 |
| `keypoint_motion` | 數字 | 與上一次偵測相比，兩次都可見的關鍵點平均位移 ÷ 人物框對角線 ÷ 經過秒數（每秒）；兩次偵測相隔超過 3 秒時為未知 |
| `near_bed_edge` | 布林 | 人物框中心在嬰兒床區域外，或離區域邊界小於「區域寬度 × `bed_edge_margin`」；沒有設定區域時為未知 |
| `activity` | 數字 | 移動分數（0–1，前後兩張分析影格有變化的像素比例）；設定嬰兒床區域時只計算區域內 |

關鍵點可見度門檻（`keypoint_visibility`，預設 0.5）、嬰兒床區域（`bed_roi`）與 `bed_edge_margin`（預設 0.1）在規則檔中設定。

### 狀況規則

規則檔（預設 `config/situations.yaml`，以 `BM_RULES_PATH` 指定）定義所有狀況與巡檢；隨附的預設檔含六個初始狀況與一個巡檢（`airway_covered`），門檻尚未以真實嬰兒影像校正。prompt 檔的路徑相對於規則檔所在目錄。

```yaml
version: 3                        # 必填整數；每次修改請遞增，會記在每筆觸發與警報上
keypoint_visibility: 0.5          # 可省略
bed_roi: [0.20, 0.15, 0.60, 0.75] # 可省略；畫面比例 x, y, w, h
bed_edge_margin: 0.1              # 可省略
situations:
  - id: face_covered              # 小寫英文、數字、底線，不可重複
    severity: critical            # info / warning / critical
    route: direct                 # direct / llm / suppress
    when:                         # 全部成立 (AND) 才算成立
      - { signal: body_visible, is: true }
      - { signal: face_visible, is: false, for: 10s, grace: 1s }
    cooldown: 60s                 # 持續成立時，每隔多久再觸發一次
    clip: [-20s, 5s]              # 可省略 (預設 [-20s, 5s])；事件片段取觸發前 / 後多久
    prompt: prompts/face_covered.md  # llm 路由必填；direct 路由可選 (只產生補充說明)
    timeout: 20s                  # 可省略 (預設 20s)；VLM 判定逾時
    on_timeout: alert             # 可省略 (預設 alert)；VLM 逾時、失效或未設定時 alert = 提醒, drop = 只記錄
    shadow: false                 # 可省略；true = 影子模式，照常判斷但只記錄、不提醒
    enabled: true                 # 可省略；false = 停用
  - id: adult_present
    severity: info
    route: suppress
    when:
      - { signal: person_count, above: 1 }
    suppress: { below: critical, linger: 2m }  # 成立期間及之後 2 分鐘，不送出低於 critical 的提醒
    cooldown: 0s
patrols:                          # 可省略；見「巡檢」
  - id: airway_covered
    severity: critical
    every: 30s
    prompt: prompts/airway_covered.md
    cooldown: 60s
```

- 條件：`is: true/false` 用於布林訊號，`above` / `below` 用於數字訊號（嚴格大於 / 小於）；`for` 表示要依擷取時間**連續**成立多久，中途訊號變成未知時重新計時。時間單位為 `ms`、`s`、`m`、`h`。
- 容許中斷：條件可加 `grace`（預設 0）。逐格訊號會短暫閃爍（例如臉被遮住時偶爾被判定為看得到、揮手換方向時 `activity` 短暫掉到 0），條件不成立或訊號未知的時間**連續短於** `grace` 時，`for` 不重新計時（中斷的時間也計入 `for`），已成立的狀況在中斷期間仍視為成立，因此也不會因為閃爍而重複觸發；中斷達到 `grace` 時才重新計時。
- 觸發：狀況從不成立變成成立時觸發一次；持續成立時每隔 `cooldown` 再觸發一次（`cooldown: 0s` 表示持續成立期間不重複觸發）。
- 路由：`direct` 立即提醒；`llm` 等片段結束後交給 VLM 判斷是否提醒；`suppress` 不提醒，而是壓制其他狀況。
- **底線保護**：`critical` 狀況的路由只能是 `direct`，也無法被壓制（`suppress.below` 只接受 `warning` 或 `critical`，表示壓制「低於」該等級的提醒）。違反時規則驗證失敗。`critical` 狀況設為影子模式或停用是允許的，但啟動與重新載入時會印出警告，監視頁面也會醒目標示。
- 驗證：不認得的欄位、訊號名稱拼錯、型別錯誤、找不到 prompt 檔、片段前段超過 `BM_RING_SECONDS` 都會驗證失敗，錯誤訊息含檔案路徑、行號與狀況 id。啟動時規則檔不存在或驗證失敗，程式以 1 結束。
- prompt：VLM 的回覆格式固定為 `{"cause": ..., "alert": ...}`，要求先描述影格中實際看到的內容再決定是否提醒。prompt 只寫判斷標準，不要列出可以照抄的原因句子：小模型會照抄 prompt 裡的選項當作 cause，造成 cause 與 alert 互相矛盾（實測 `{"alert": true, "cause": "只是轉頭、低頭或離開畫面"}`）。

#### 調整流程

1. 修改規則檔或 prompt 並遞增 `version`。
2. 用前幾晚的觀測紀錄重播（見「離線重播」），確認每個狀況的觸發次數合理。
3. 新的或大幅修改的狀況先設 `shadow: true` 觀察幾晚，在監視頁面查看它本來會送出的提醒。
4. 確認後移除 `shadow`，正式啟用。

服務執行中會每 2 秒檢查規則檔與 prompt 檔，變更後 5 秒內重新載入；新規則驗證失敗時繼續使用原本的規則，錯誤顯示在監視頁面與 `/api/situations`，修正後自動恢復。規則只能以修改檔案的方式變更，網頁只顯示、不提供編輯。

#### 巡檢

巡檢每隔 `every` 從環形緩衝取最近 `frames` 張影格（相隔 1 秒）交給 VLM 判斷某個狀況是否發生，用於第一段訊號偵測不到的狀況。預設的 `airway_covered` 檢查口鼻是否被蓋住。

```yaml
patrols:
  - id: airway_covered            # 與狀況共用 id 命名規則，不可與狀況重複
    severity: critical            # info / warning / critical
    every: 30s                    # 必填；5s–1h
    frames: 2                     # 可省略 (預設 2，1–6)；送出最近幾張影格
    confirm: 2                    # 可省略 (預設 1，1–5)；連續幾次回覆需要提醒才觸發
    cooldown: 60s                 # 必填；持續回覆需要提醒時，每隔多久再觸發一次 (0s = 不重複)
    clip: [-20s, 5s]              # 可省略；觸發時的事件片段範圍
    prompt: prompts/airway_covered.md  # 必填
    timeout: 15s                  # 可省略 (預設 20s)；必須小於 every
    # while:                      # 可省略；前提條件 (格式同 when)，不成立時暫停巡檢。省略 = 一律巡檢
    #   - { signal: person_present, is: true }
    # shadow / enabled            # 同狀況
```

- 確認：第一次回覆需要提醒且 `confirm` 大於 1 時，立即再巡檢一次（不等 `every`），用來過濾小模型的單次誤判。`every: 30s, confirm: 2` 時，口鼻被蓋住到送出提醒最久約 30 秒加兩次判定（約 2 × 1 秒）。
- 觸發：達到確認次數時立即送出警報（巡檢已經過 VLM 判定，不再經過其他判定），附上 VLM 的原因說明並寫出事件片段。巡檢只會讓提醒發生，沒有撤回提醒的路徑，因此 `severity` 可以是 `critical`；`critical` 巡檢的警報無法被壓制。
- 佇列：巡檢排在觸發後的判定之前，不受判定佇列上限（4 筆）限制。
- **巡檢停止**：VLM 未設定、無法連線、逾時、回覆無法解析或沒有影格時算失敗。連續 3 次失敗，或巡檢進行中超過 `3 × every` 沒有成功結果時，送出 `warning` 等級的系統警報「`<id>` 巡檢停止」（不可壓制），監視頁面醒目標示；巡檢再次成功時記錄「已恢復」。前提條件不成立而暫停的時間不計入。口鼻被蓋住只靠巡檢偵測，因此 VLM 服務必須常駐：以 Docker Compose 部署時 `vlm` 會隨開機啟動並在異常結束時自動重啟（見「以 Docker Compose 部署」）。
- 巡檢紀錄：每次巡檢結果寫入 `BM_LOG_DIR/patrols-YYYYMMDD.jsonl`（每 30 秒一行，保留 14 天），包含是否提醒、原因、耗時、失敗原因與原始回覆，可用來統計 VLM 的誤報率與延遲。只有觸發的巡檢會進入警報紀錄。
- 成本：每次巡檢 2 張長邊 640 的影格約 650 tokens，以 Qwen2.5-VL-3B 約 0.6 秒，每 30 秒一次約占 GPU 時間 2%。

### ROI 設定

嬰兒床區域（ROI）決定 `activity` 只計算哪個區域，以及 `near_bed_edge`（`near_bed_side` 狀況）是否有值。ROI 可以寫在規則檔的 `bed_roi`，也可以在監視頁面用滑鼠或觸控框選：

- **優先順序**：網頁設定（`BM_OVERRIDES_DIR/bed_roi.json`）> 規則檔的 `bed_roi` > 未設定。程式不會改寫規則檔。
- **生效**：網頁儲存後立即生效（回應前已重新載入）。ROI 檔與規則檔一起熱重載，內容納入規則的 `hash`，因此只改 ROI 時 `version` 不變、`hash` 改變，警報紀錄的 `rulesHash` 可以分辨。ROI 檔內容無效時與規則檔驗證失敗相同處理（啟動失敗；執行中沿用舊規則並回報錯誤）。
- **還原**：頁面的「還原為規則檔」或 `DELETE /api/roi`，也可以直接刪除 `bed_roi.json`。
- **不能從網頁清除**：清除會讓 `near_bed_side` 無聲無息地失效。要取消 ROI，請刪除規則檔的 `bed_roi` 並還原網頁設定。
- **修改紀錄**：每次設定或還原都在觸發紀錄新增一筆 `roi_changed`（`delivery: logged`），含新舊值與來源 IP。
- **重播**：觀測紀錄存的是已經算好的訊號，ROI 變更不影響 `bm_replay_rules` 的結果，也無法以重播評估新的 ROI；請用影子模式觀察。
- **安全性**：目前沒有認證，只擋跨站請求（只接受 `Content-Type: application/json`、`Origin` 必須與 `Host` 相同、內容上限 4 KiB），**同網段的任何人都能修改 ROI**。請只在信任的網路使用，並留意觸發紀錄中的 `roi_changed`。

`PUT /api/roi` 的內容是 `{"roi": [x, y, w, h]}`（畫面比例，數值四捨五入到小數 3 位；寬、高至少 0.05）。`If-Match` 標頭必須是讀到的規則 `hash`（`/api/situations` 的 `hash`），避免覆蓋別人剛做的修改：

```bash
HASH=$(curl -s http://localhost:18080/api/situations | python3 -c 'import json,sys; print(json.load(sys.stdin)["hash"])')
curl -X PUT -H 'Content-Type: application/json' -H "If-Match: $HASH" \
     -d '{"roi": [0.2, 0.15, 0.6, 0.75]}' http://localhost:18080/api/roi
# 還原：每次修改後 hash 都會改變，要重新讀取
HASH=$(curl -s http://localhost:18080/api/situations | python3 -c 'import json,sys; print(json.load(sys.stdin)["hash"])')
curl -X DELETE -H "If-Match: $HASH" http://localhost:18080/api/roi
```

回應包含處理後的 `bedRoi`、`bedRoiSource`、`fileBedRoi`、`bedEdgeMargin`、`hash`、`version`，失敗時另有 `error`：

| 狀態碼 | 原因 |
|---|---|
| 200 | 成功（還原時沒有網頁設定也是 200） |
| 400 | ROI 無效（超出畫面、太小、`null` 或空陣列）、內容不是 JSON、缺少 `If-Match` |
| 403 | `Origin` 與 `Host` 不同（跨站請求） |
| 409 | `If-Match` 不是目前的 `hash`（回應附上目前的 ROI），或規則目前載入失敗 |
| 413 | 內容大於 4 KiB |
| 415 | `Content-Type` 不是 `application/json` |
| 500 | 無法寫入 `BM_OVERRIDES_DIR` |

### 事件片段

狀況觸發後，服務等到片段結束時間（觸發時間 + `clip` 後段），從主碼流環形緩衝取出 `[觸發時間 - 前段, 觸發時間 + 後段]` 的影格，存成 `BM_EVENT_DIR/<YYYYMMDD-HHMMSS>-<狀況>-<NNN>/`：`000001.jpg` 起為原始 JPEG（不重新編碼），`meta.json` 記錄狀況、觸發時間、規則版本與每張影格的 `seq`、擷取時間。攝影機中斷期間沒有影格，片段只含實際擷取到的影格。總大小超過 `BM_EVENT_MAX_MB` 時從最舊的片段開始刪除。

### 觀測紀錄與離線重播

服務把每張分析影格的訊號寫入 `BM_LOG_DIR/observations-YYYYMMDD.jsonl`（依當地日期分檔，約 130 MB/天，保留 14 天）。每次啟動先寫一行 `{"type":"session","signals":[...],"monoUs":...,"epochMs":...}`，之後每行是 `[seq, 擷取時間(微秒), 各訊號值...]`，未知為 `null`。

`bm_replay_rules` 以規則檔重新判斷觀測紀錄，時間取自紀錄中的擷取時間，因此相同輸入一定產生相同輸出，並與服務即時判斷的條件語意一致：

```bash
./build/bm_replay_rules config/situations.yaml logs/observations-20261117.jsonl logs/observations-20261118.jsonl
```

```
2026-11-18 02:13:40.512  +   4380.2s  seq=131402   face_covered
2026-11-18 03:02:11.088  +   7290.8s  seq=218700   awake_active

規則 version 3 (1a2b3c4d5e6f)，2592000 張影格，共 2 次觸發
  face_covered             1 次
  awake_active             1 次
  ...
```

重播時片段前段不受 `BM_RING_SECONDS` 限制；要一併檢查時加上 `--ring-seconds 60`。巡檢需要 VLM，重播時略過並在輸出中註明。

## 專案結構

```
src/
  main.cpp                  啟動順序：設定 → 載入模型 → 開攝影機 → HTTP 服務 → 擷取 / 分析 / 即時影像 / systemd 通知
  config/                   BM_* 環境變數
  common/LatestSlot         只保留最新值的單槽（latest-wins）
  common/SystemdNotifier    NOTIFY_SOCKET 的 READY / WATCHDOG 通知
  models/MonitorState       以不可變快照共享的監視狀態
  vision/FrameSource        攝影機 MJPG 擷取（OpenCV V4L2，不解碼）
  vision/CaptureThread      擷取執行緒：指定 seq、攝影機中斷時重連
  vision/AnalysisThread     分析執行緒：子碼流解碼 → 移動分數 → 依排程人物偵測
  vision/ActivityScorer     影格差分的移動分數
  vision/DetectScheduler    依移動分數調整人物偵測頻率
  vision/YoloDetector       ONNX Runtime 推論與前處理
  vision/YoloDecoder        YOLO26 輸出解碼（純函式）
  vision/MotionTracker      累積位移的移動判定（純函式）
  vision/Jpeg               補 Huffman table、子碼流縮小倍率
  vision/LiveProtocol       /ws/live 訊息格式
  controllers/              Crow 路由（API、靜態檔案、/ws/live）
frontend/                   監視頁面
tests/                      CTest 測試
openspec/                   規格與變更紀錄
```
