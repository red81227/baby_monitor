# BabyMonitor

用 USB 攝影機看著寶寶的監視器：以 YOLO26n（ONNX Runtime，可用 CUDA）偵測畫面中的人物，人物位置累積位移超過門檻就判定為「有移動」，並透過網頁即時顯示畫面與狀態。

- 靜止的人物以**綠框**標示，判定為移動的影格改為**紅框**並顯示 `MOTION`。
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

### YOLO26n 模型

用 [Ultralytics](https://docs.ultralytics.com/) 匯出 ONNX（輸出格式必須是 YOLO26 內含 NMS 的 `[1, 300, 6]`，這是預設值）：

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install ultralytics onnx
yolo export model=yolo26n.pt format=onnx imgsz=640
mkdir -p models && mv yolo26n.onnx models/
```

`onnxruntime/` 與 `*.onnx` 都已列在 `.gitignore`，不會進版本控制。

## 建置與測試

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`yolo_inference` 測試會用 `tests/data/bus.jpg` 跑真實模型；找不到 `models/yolo26n.onnx` 時會標示為 Skipped。

## 執行

```bash
./build/BabyMonitor
```

開啟 `http://<主機 IP>:18080/` 即可看到即時畫面。也可以從任何工作目錄啟動（前端檔案從執行檔旁的 `frontend/` 讀取），但 `BM_MODEL_PATH` 的預設值是相對於**工作目錄**的 `models/yolo26n.onnx`。

按 Ctrl+C 或送 SIGTERM 會正常關閉並以 0 結束；攝影機中斷時以 1 結束。

### HTTP 介面

| 路徑 | 說明 |
|---|---|
| `GET /` | 監視頁面 |
| `GET /api/status` | `{"motionDetected", "personDetected", "lastMotionEpochMs", "latestFrameBytes"}` |
| `GET /api/frame.jpg` | 最新的標示後影格；還沒有影格時回 503 |

### 環境變數

| 變數 | 預設值 | 說明 |
|---|---|---|
| `BM_MODEL_PATH` | `models/yolo26n.onnx` | YOLO26n ONNX 模型路徑 |
| `BM_PORT` | `18080` | 網頁與 API 的 port（1–65535） |
| `BM_MOTION_PX` | `20` | 人物中心累積位移超過幾個像素算移動（正數） |
| `BM_PERSON_CONF` | `0.45` | 人物偵測的信心門檻（0–1）；包著被子的嬰兒偵測不到時可以調低 |

任何值無效時，程式會在開啟攝影機前印出包含變數名稱的錯誤並以 1 結束。

### 以 systemd 執行

`/etc/babymonitor.env`：

```ini
BM_MODEL_PATH=/opt/babymonitor/models/yolo26n.onnx
BM_PORT=18080
BM_PERSON_CONF=0.45
```

`/etc/systemd/system/babymonitor.service`：

```ini
[Unit]
Description=BabyMonitor
After=network.target

[Service]
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

## 專案結構

```
src/
  main.cpp                  啟動順序：設定 → 載入模型 → 開攝影機 → HTTP 服務 → 擷取執行緒
  config/                   BM_* 環境變數
  models/MonitorState       執行緒安全的共享狀態
  vision/YoloDetector       ONNX Runtime 推論與前處理
  vision/YoloDecoder        YOLO26 輸出解碼（純函式）
  vision/MotionTracker      累積位移的移動判定（純函式）
  vision/VisionPipeline     擷取執行緒
  controllers/              Crow 路由（API、靜態檔案）
frontend/                   監視頁面
tests/                      CTest 測試
openspec/                   規格與變更紀錄
```
