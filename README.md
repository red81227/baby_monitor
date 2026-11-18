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
| ONNX Runtime | 1.27.0 GPU 版（CUDA 12），見下方 |
| CUDA 12 runtime + cuDNN 9 | 選用；沒有時自動改用 CPU（較慢） |
| 網路 | 第一次 configure 時會用 FetchContent 下載 Crow v1.2.1 與 asio 1.30.2 |

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

任何值無效時，程式會在開啟攝影機前印出包含變數名稱的錯誤並以 1 結束。

`BM_MOTION_PX` 以主碼流像素計算，同樣的實際位移在解析度越高時像素數越大：從 640x480 改成 1280x720 時，若要維持原本的靈敏度，大約要把 `BM_MOTION_PX` 加倍（例如 20 → 40）。

### 以 systemd 執行

`/etc/babymonitor.env`：

```ini
BM_MODEL_PATH=/opt/babymonitor/models/yolo26n-pose.onnx
BM_PORT=18080
BM_PERSON_CONF=0.45
```

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
