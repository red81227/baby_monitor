# Proposal

## Why

監視服務現在由兩個行程組成：BabyMonitor 與本機 VLM（llama-server）。README 只有 BabyMonitor 的 systemd 範例，llama-server 要手動在終端機啟動，設定散在多個指令與環境變數中。`cascade-alert-architecture` 加入 VLM 巡檢後，口鼻被蓋住只能由 VLM 偵測，VLM 必須和監視器一起開機常駐、當掉自動重啟。因此需要一套方式把兩個服務一起啟動、停止、查看狀態與日誌，並且可以重現部署。

## What Changes

- 新增 Docker Compose 部署：一個指令啟動 `monitor`（BabyMonitor）與 `vlm`（llama-server）兩個容器，兩者都使用 GPU，`monitor` 另外使用攝影機
- 新增 BabyMonitor 的映像建置（多階段 Dockerfile）：建置階段編譯程式並下載固定版本、驗證過 checksum 的 ONNX Runtime GPU；執行階段只含程式、前端檔案與執行所需的函式庫
- `vlm` 使用 llama.cpp 官方 CUDA server 映像（固定版本），模型目錄以唯讀方式掛載
- 只有監視頁面的 port 對外開放；VLM API 沒有認證，只在 Compose 的內部網路中提供給 `monitor`
- 規則檔、YOLO 模型、紀錄與事件片段以掛載目錄保存在主機上：規則檔照常熱重載，紀錄與片段在容器重建後仍在，檔案擁有者是主機使用者
- 開機自動啟動、異常結束自動重啟、健康檢查、容器日誌輪替、時區與主機一致（紀錄檔名依當地日期）
- 新增 BabyMonitor 停滯時自行結束：在容器中沒有 systemd watchdog，Docker 只在行程結束時重啟。擷取或分析停滯超過設定時間時，程式以非 0 結束碼結束，由重啟策略重新啟動
- 所有可調整的部署設定（port、資料目錄、VLM 模型檔、映像版本等）集中在 `.env`，並提供 `.env.example`
- README 新增部署與日常操作（啟動、停止、狀態、日誌、更新、換 VLM 模型），原本的 systemd 段落改為「不使用 Docker 時」的替代方式

### 不在範圍內

- 前端頁面的改版（另開 change）
- HTTPS、認證或從網際網路存取
- 多台主機、Kubernetes、映像上傳到 registry
- 選定 VLM 模型與參數調校（沿用目前的 Qwen2.5-VL-3B 設定，之後只改 `.env`）

## Capabilities

### New Capabilities
- `container-deployment`: 以 Docker Compose 部署與管理監視服務：服務組成、GPU 與攝影機、網路暴露範圍、資料保存與擁有者、健康檢查、重啟與開機啟動、時區、日誌、部署設定檔

### Modified Capabilities
- `service-lifecycle`: 新增「停滯時自行結束」：沒有 systemd watchdog 的環境中，擷取或分析停滯超過設定時間時以非 0 結束碼結束
- `runtime-configuration`: 新增停滯結束時間的環境變數與驗證

## Impact

- 新增檔案：`docker/monitor.Dockerfile`、`compose.yaml`、`.env.example`、`.dockerignore`
- 程式碼：`Config` 新增一個設定；`main.cpp` 在沒有 systemd watchdog 時啟動停滯檢查（沿用既有的 `Heartbeat`）
- 依賴：主機需要 Docker、Compose 與 NVIDIA Container Toolkit（本機已安裝）；首次建置需要網路（下載 ONNX Runtime、CMake FetchContent、基底映像）
- 磁碟：BabyMonitor 映像（CUDA + cuDNN 執行環境）與 llama.cpp 映像各數 GB
- GPU：兩個容器共用同一張 8 GB GPU，與目前直接執行時相同
- 文件：README 的部署段落、`cascade-alert-architecture` 的 llama-server 啟動說明改為 Compose
- 既有的直接執行與 systemd 方式仍然可用
