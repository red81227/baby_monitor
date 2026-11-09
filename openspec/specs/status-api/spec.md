# status-api Specification

## Purpose

以 HTTP 對外提供監視器目前的狀態與最新影格，供網頁介面或其他程式查詢。

## Requirements

### Requirement: 狀態查詢
系統 SHALL 在網頁 port 上以 `GET /api/status` 回傳 JSON，包含 `motionDetected`（boolean）、`personDetected`（boolean）、`lastMotionEpochMs`（最後一次移動的 Unix 毫秒時間，沒有過移動時為 0）。

#### Scenario: 剛啟動
- **WHEN** 啟動後還沒有發生移動時呼叫 `GET /api/status`
- **THEN** 回應狀態碼 200，`motionDetected` 為 false，`lastMotionEpochMs` 為 0

#### Scenario: 發生移動後
- **WHEN** 畫面中的人移動後呼叫 `GET /api/status`
- **THEN** `lastMotionEpochMs` 為接近當下的時間，且 `personDetected` 為 true

### Requirement: 最新影格
系統 SHALL 以 `GET /api/frame.jpg` 回傳最新的影格（`Content-Type: image/jpeg`），影格中的人物以框標示，判定為移動的影格中框的顏色與靜止時不同；尚無影格時 SHALL 回傳 503。

#### Scenario: 取得影格
- **WHEN** 系統運作中呼叫 `GET /api/frame.jpg`
- **THEN** 回應狀態碼 200，內容是可解碼的 JPEG

#### Scenario: 人物標示
- **WHEN** 畫面中有人物時取得影格
- **THEN** 影格中該人物周圍有框；人物移動的影格中，框的顏色與靜止時不同

#### Scenario: 尚未取得第一張影格
- **WHEN** 第一張影格處理完成前呼叫 `GET /api/frame.jpg`
- **THEN** 回應狀態碼 503
