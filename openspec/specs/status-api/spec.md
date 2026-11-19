# status-api Specification

## Purpose

以 HTTP 對外提供監視器目前的狀態與最新影格，供網頁介面或其他程式查詢。

## Requirements

### Requirement: 狀態查詢
系統 SHALL 在網頁 port 上以 `GET /api/status` 回傳 JSON，包含 `motionDetected`（boolean）、`personDetected`（boolean）、`lastMotionEpochMs`（最後一次移動的 Unix 毫秒時間，沒有過移動時為 0）、`cameraConnected`（boolean）、`seq`（最新影格序號，尚無影格時為 0）、`detectHz`（最近的人物偵測頻率）。

#### Scenario: 剛啟動
- **WHEN** 啟動後還沒有發生移動時呼叫 `GET /api/status`
- **THEN** 回應狀態碼 200，`motionDetected` 為 false，`lastMotionEpochMs` 為 0

#### Scenario: 發生移動後
- **WHEN** 畫面中的人移動後呼叫 `GET /api/status`
- **THEN** `lastMotionEpochMs` 為接近當下的時間，且 `personDetected` 為 true

#### Scenario: 攝影機中斷
- **WHEN** 攝影機被拔除後呼叫 `GET /api/status`
- **THEN** 回應狀態碼 200，`cameraConnected` 為 false，`seq` 停止增加

#### Scenario: 運作中
- **WHEN** 攝影機正常運作時連續兩次呼叫 `GET /api/status`，間隔 1 秒
- **THEN** `cameraConnected` 為 true，第二次的 `seq` 大於第一次，`detectHz` 大於 0

### Requirement: 最新影格
系統 SHALL 以 `GET /api/frame.jpg` 回傳最新影格的主碼流原圖（`Content-Type: image/jpeg`），影像中 MUST NOT 有任何標示，並以 `X-Frame-Seq` 標頭回報其 `seq`；尚無影格時 SHALL 回傳 503。

#### Scenario: 取得影格
- **WHEN** 系統運作中呼叫 `GET /api/frame.jpg`
- **THEN** 回應狀態碼 200，內容是可解碼的 JPEG，且帶有 `X-Frame-Seq` 標頭

#### Scenario: 人物標示
- **WHEN** 畫面中有人物時取得影格
- **THEN** 影格中沒有畫上框或文字（人物標示改由監視頁面依偵測 metadata 繪製）

#### Scenario: 尚未取得第一張影格
- **WHEN** 第一張影格擷取完成前呼叫 `GET /api/frame.jpg`
- **THEN** 回應狀態碼 503

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

### Requirement: 規則狀態包含 ROI
`GET /api/situations` SHALL 在既有欄位之外回傳生效的 ROI（以畫面比例表示的 `[x, y, w, h]`，未設定時為 null）、ROI 來源（網頁設定、規則檔、未設定）、規則檔中的 `bed_roi`（沒有時為 null）與 `bed_edge_margin`。既有欄位的名稱與意義 MUST NOT 改變，此端點 MUST 仍是唯讀的。

#### Scenario: 網頁設定覆蓋規則檔
- **WHEN** 規則檔與網頁都設定了 ROI 時呼叫 `GET /api/situations`
- **THEN** 生效 ROI 是網頁設定的值，來源為網頁設定，規則檔的 `bed_roi` 欄位仍回傳規則檔的值

#### Scenario: 未設定
- **WHEN** 規則檔與網頁都沒有設定 ROI 時呼叫 `GET /api/situations`
- **THEN** 生效 ROI 為 null，來源為未設定
