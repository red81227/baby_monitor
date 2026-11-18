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
