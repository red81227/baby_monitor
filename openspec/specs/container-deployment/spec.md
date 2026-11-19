# container-deployment Specification

## Purpose
以 Docker Compose 部署與管理監視服務（BabyMonitor 與本機 VLM），讓兩個服務能以一組指令一起啟動、停止、查看狀態與日誌，開機自動執行、異常時自動恢復，且部署結果可以重現。

## Requirements

### Requirement: 服務組成
系統 SHALL 提供一份 Compose 設定，定義兩個服務：`monitor`（BabyMonitor）與 `vlm`（OpenAI 相容的本機 VLM 服務）。在 repo 根目錄執行 `docker compose up -d` MUST 啟動兩個服務；`docker compose down` MUST 停止並移除兩者的容器，且不刪除主機上保存的資料。`monitor` MUST 能在 `vlm` 尚未就緒或無法使用時照常啟動並提供監看（VLM 相關功能依既有的失效處理）。

#### Scenario: 一個指令啟動
- **WHEN** 在已依 README 準備好 `.env`、模型與規則檔的主機上執行 `docker compose up -d`
- **THEN** `monitor` 與 `vlm` 都進入執行狀態，約 1 分鐘內監視頁面可以開啟，`/api/situations` 顯示 VLM 可用

#### Scenario: VLM 起不來
- **WHEN** `.env` 中的 VLM 模型檔路徑錯誤，執行 `docker compose up -d`
- **THEN** `monitor` 照常提供監視頁面，頁面顯示 VLM 無法連線與巡檢停止；`docker compose ps` 顯示 `vlm` 異常

### Requirement: 使用 GPU 與攝影機
兩個服務 SHALL 都能使用主機的 NVIDIA GPU；`monitor` SHALL 能使用 `.env` 指定的攝影機裝置。容器中的人物偵測 MUST 使用 CUDA 執行，攝影機格式與解析度的協商結果 MUST 與直接執行時相同。

#### Scenario: 容器中使用 CUDA
- **WHEN** 以 Compose 啟動後查看 `monitor` 的日誌
- **THEN** 日誌顯示 YOLO 模型以 CUDA 執行，攝影機以 MJPG 1280x720 開啟

### Requirement: 網路暴露範圍
只有 `monitor` 的網頁與 API port SHALL 對主機的網路開放，port 由 `.env` 設定。`vlm` 的 API MUST NOT 發佈到主機的任何網路介面，只能由 Compose 內部網路中的 `monitor` 存取。

#### Scenario: VLM 不對外
- **WHEN** 兩個服務執行中，從主機或區網對 VLM 的 port 發出請求
- **THEN** 連線失敗；`monitor` 的 VLM 判定與巡檢照常運作

### Requirement: 資料保存
規則檔與 prompt、YOLO 模型、VLM 模型 SHALL 由主機目錄掛載提供（模型唯讀）；觀測紀錄、警報紀錄、巡檢紀錄與事件片段 SHALL 寫入主機上 `.env` 指定的資料目錄，重建或移除容器後 MUST 仍然存在。容器寫出的檔案 MUST 屬於 `.env` 指定的主機使用者，而不是 root。修改主機上的規則檔 MUST 照常在 5 秒內熱重載。

#### Scenario: 重建容器後資料仍在
- **WHEN** 產生幾筆警報與事件片段後執行 `docker compose down` 再 `docker compose up -d`
- **THEN** 監視頁面仍列出原本的警報，片段可以播放；主機資料目錄中的檔案擁有者是設定的使用者

#### Scenario: 在主機修改規則
- **WHEN** 服務執行中，在主機上修改規則檔的門檻並存檔
- **THEN** 5 秒內監視頁面顯示新的規則版本，不需要重啟容器

### Requirement: 開機啟動與自動恢復
兩個服務 SHALL 在主機開機、Docker 啟動後自動執行（除非使用者以 `docker compose stop` 或 `down` 停止）；行程異常結束時 SHALL 自動重啟。每個服務 SHALL 有健康檢查，`docker compose ps` MUST 顯示其健康狀態。

#### Scenario: 主機重開機
- **WHEN** 兩個服務執行中時重新開機
- **THEN** 開機後不需要任何操作，兩個服務自動執行，監視頁面可以開啟

#### Scenario: VLM 當掉
- **WHEN** `vlm` 的行程被強制結束
- **THEN** `vlm` 自動重啟；期間 `monitor` 依巡檢停止的規則提醒，`vlm` 恢復後巡檢自動恢復

#### Scenario: 監視器停滯
- **WHEN** `monitor` 的擷取或分析停滯超過設定的時間
- **THEN** `monitor` 以非 0 結束碼結束並被自動重啟

### Requirement: 時區與日誌
容器中的當地時間 MUST 與主機相同（紀錄檔依當地日期分檔、頁面與紀錄中的時間正確）。兩個服務的標準輸出 SHALL 可以用 `docker compose logs` 查看，且容器日誌 MUST 有大小上限並自動輪替，不會無限制占用磁碟。

#### Scenario: 紀錄檔日期
- **WHEN** 主機時區為 Asia/Taipei，於當地時間凌晨 0:30 產生警報
- **THEN** 警報寫入當地當天日期的 `alerts-YYYYMMDD.jsonl`

### Requirement: 部署設定檔
所有部署相關的可調整值（監視頁面 port、攝影機裝置、資料目錄、主機使用者 ID、VLM 模型檔與 context 長度、映像版本）SHALL 集中在 repo 根目錄的 `.env`，並提供含說明與預設值的 `.env.example`；`.env` MUST NOT 納入版本控制。缺少必要設定時 `docker compose` MUST 在建立容器前輸出指出該設定名稱的錯誤。

#### Scenario: 換 VLM 模型
- **WHEN** 修改 `.env` 中的 VLM 模型檔與模型名稱後執行 `docker compose up -d`
- **THEN** 只有 `vlm` 重新建立並載入新模型，`monitor` 以新的模型名稱呼叫

#### Scenario: 缺少設定
- **WHEN** `.env` 沒有設定 VLM 模型目錄就執行 `docker compose up -d`
- **THEN** 指令失敗，錯誤訊息包含該設定名稱，沒有建立任何容器

### Requirement: 可重現的映像建置
`monitor` 的映像 SHALL 能以 `docker compose build` 從 repo 原始碼建置，不依賴主機上已下載的 ONNX Runtime 或建置目錄；所有外部下載（基底映像、ONNX Runtime、VLM 映像）MUST 使用固定版本，下載的 ONNX Runtime MUST 驗證 checksum。

#### Scenario: 乾淨環境建置
- **WHEN** 在沒有 `build/` 與 `onnxruntime/` 目錄的 repo 複本中執行 `docker compose build`
- **THEN** 建置成功，產生的映像可以啟動並以 CUDA 執行偵測
