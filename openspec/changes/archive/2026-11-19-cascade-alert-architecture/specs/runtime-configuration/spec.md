# Spec Delta

## ADDED Requirements

### Requirement: 規則檔路徑設定
系統 SHALL 從 `BM_RULES_PATH` 讀取狀況規則檔路徑，預設為 `config/situations.yaml`（相對於工作目錄）。

#### Scenario: 預設規則檔
- **WHEN** 沒有設定 `BM_RULES_PATH`，在 repo 根目錄啟動
- **THEN** 系統從 `config/situations.yaml` 載入規則

### Requirement: VLM 設定
系統 SHALL 從 `BM_VLM_URL` 讀取 VLM 端點、從 `BM_VLM_MODEL` 讀取模型名稱。`BM_VLM_URL` 未設定時 VLM 視為停用，`llm` 路由的狀況依 `on_timeout` 處理；值不是有效網址時 SHALL 在開啟攝影機前輸出含變數名稱的錯誤並以非 0 結束碼結束。

#### Scenario: 未設定 VLM
- **WHEN** 沒有設定 `BM_VLM_URL` 就啟動
- **THEN** 服務正常運作，啟動訊息與狀態查詢顯示 VLM 停用

#### Scenario: 無效網址
- **WHEN** 以 `BM_VLM_URL=not a url` 啟動
- **THEN** 程式輸出包含 `BM_VLM_URL` 的錯誤訊息，結束碼不為 0

### Requirement: 事件與紀錄設定
系統 SHALL 從 `BM_RING_SECONDS` 讀取環形緩衝長度（正整數秒，預設 60）、從 `BM_EVENT_DIR` 讀取事件片段目錄（預設 `events`）、從 `BM_EVENT_MAX_MB` 讀取片段空間上限（正整數，預設 2048）、從 `BM_LOG_DIR` 讀取觀測與警報紀錄目錄（預設 `logs`）；數值無效時 SHALL 在開啟攝影機前輸出含變數名稱的錯誤並以非 0 結束碼結束。

#### Scenario: 無效緩衝長度
- **WHEN** 以 `BM_RING_SECONDS=0` 啟動
- **THEN** 程式輸出包含 `BM_RING_SECONDS` 的錯誤訊息，結束碼不為 0

#### Scenario: 片段規則超過緩衝
- **WHEN** 規則檔中某狀況的片段前段長度大於 `BM_RING_SECONDS`
- **THEN** 規則驗證失敗，錯誤訊息指出該狀況與 `BM_RING_SECONDS`
