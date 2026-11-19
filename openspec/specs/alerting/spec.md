# alerting Specification

## Purpose
把狀況觸發與 LLM 判定的結果轉成給使用者的警報：決定送出、壓制或只記錄，並保留可追查的紀錄。

## Requirements

### Requirement: 產生警報
系統 SHALL 依路由產生警報：`direct` 在觸發時立即產生；`llm` 在判定為需要提醒，或判定失敗且 `on_timeout` 為 `alert` 時產生；`suppress` 不產生警報。巡檢觸發時 SHALL 立即產生警報；巡檢停止與恢復 SHALL 產生系統警報（`warning`）。每筆警報 MUST 包含狀況 `id`、嚴重程度、觸發時間、原因說明（若有）、是否經 LLM 判定、規則版本與事件片段 id。

#### Scenario: direct 狀況
- **WHEN** 路由為 `direct` 的狀況觸發
- **THEN** 1 秒內產生警報，不等待 LLM

#### Scenario: 巡檢觸發
- **WHEN** `airway_covered` 巡檢達到確認次數
- **THEN** 1 秒內產生 `critical` 警報，標示經 LLM 判定並附上原因說明

### Requirement: 壓制
`suppress` 狀況成立期間，以及不再成立後的 `linger` 時間內，嚴重程度低於其設定等級的警報 SHALL 不送出，但 MUST 記錄並標示為被壓制與壓制來源。巡檢觸發的警報依其嚴重程度適用同樣的規定。`critical` 警報與系統警報 MUST NOT 被壓制。

#### Scenario: 大人在旁邊
- **WHEN** `adult_present` 成立（壓制 `critical` 以下、linger 2 分鐘），期間 `awake_active`（`warning`）判定為需要提醒
- **THEN** 不送出該警報，紀錄顯示被 `adult_present` 壓制

#### Scenario: 大人在旁邊但臉被蓋住
- **WHEN** `adult_present` 成立期間，`face_covered`（`critical`）觸發
- **THEN** 照常送出警報

#### Scenario: 大人在旁邊時巡檢停止
- **WHEN** `adult_present` 成立期間，`airway_covered` 巡檢停止
- **THEN** 照常送出「巡檢停止」系統警報

### Requirement: 警報紀錄
系統 SHALL 把所有觸發與警報（含已送出、被壓制、影子模式、判定為不提醒）持久保存，服務重啟後仍可查詢。

#### Scenario: 重啟後查詢
- **WHEN** 產生幾筆警報後重啟服務，再查詢最近的警報
- **THEN** 重啟前的警報仍在，內容與重啟前相同
