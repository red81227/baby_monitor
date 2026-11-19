# Spec Delta

## ADDED Requirements

### Requirement: 規則狀態查詢
系統 SHALL 以 `GET /api/situations` 回傳 JSON，包含目前使用的規則檔路徑、`version`、內容雜湊值、載入時間、最近一次載入錯誤（沒有時為 null）、VLM 是否可用，每個狀況的 `id`、路由、嚴重程度、是否為影子模式、目前是否成立，以及每個巡檢的 `id`、嚴重程度、間隔、是否為影子模式、最後一次成功的時間、最近一次的結果與原因、是否已停止。此端點 MUST 是唯讀的。

#### Scenario: 正常載入
- **WHEN** 規則檔載入成功後呼叫 `GET /api/situations`
- **THEN** 回應狀態碼 200，`version` 與規則檔相同，載入錯誤為 null，列出所有狀況

#### Scenario: 載入失敗
- **WHEN** 執行中存入一個驗證失敗的規則檔後呼叫 `GET /api/situations`
- **THEN** `version` 仍是原本的規則版本，載入錯誤欄位說明失敗原因

#### Scenario: 巡檢停止
- **WHEN** VLM 服務停止使巡檢停止後呼叫 `GET /api/situations`
- **THEN** 該巡檢標示為已停止，並附上最後一次成功的時間

### Requirement: 觸發與警報查詢
系統 SHALL 以 `GET /api/alerts` 回傳最近的觸發與警報（至少最近 100 筆，新的在前），每筆包含狀況或巡檢 `id`（系統警報另外標示）、時間、嚴重程度、送出狀態（已送出、被壓制、影子模式、判定為不提醒）、原因說明、LLM 判定結果與規則版本，以及事件片段 id；並以 `GET /api/events/<id>` 提供事件片段的影格清單與影格內容。此兩個端點 MUST 是唯讀的。

#### Scenario: 查詢最近警報
- **WHEN** 發生一次 `face_covered` 警報後呼叫 `GET /api/alerts`
- **THEN** 第一筆是該警報，送出狀態為已送出，帶有事件片段 id

#### Scenario: 查看片段
- **WHEN** 以警報中的事件片段 id 呼叫 `GET /api/events/<id>`
- **THEN** 回應包含依時間排序的影格清單，每張影格可取得可解碼的 JPEG

### Requirement: 狀態查詢包含規則摘要
`GET /api/status` SHALL 在既有欄位之外新增 `rulesVersion`（目前規則版本）、`rulesError`（最近一次載入錯誤，沒有時為 null）與 `lastAlertEpochMs`（最後一次送出警報的 Unix 毫秒時間，沒有時為 0）；既有欄位的名稱與意義 MUST NOT 改變。

#### Scenario: 舊客戶端
- **WHEN** 只讀取既有欄位的客戶端呼叫 `GET /api/status`
- **THEN** 既有欄位照常回傳，不受新增欄位影響
