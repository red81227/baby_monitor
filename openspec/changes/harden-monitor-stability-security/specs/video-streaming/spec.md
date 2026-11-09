# Spec Delta

## Purpose

定義系統對外提供即時 MJPEG 影像串流的行為，讓瀏覽器或播放器可以觀看標示後的監視畫面。

## ADDED Requirements

### Requirement: 提供 MJPEG 串流
系統 SHALL 在設定的串流 port 上以 HTTP 回應 `multipart/x-mixed-replace` 的 JPEG 串流，內容為最新且已標示移動區域的影格，每個客戶端的更新頻率上限約為 25 FPS。

#### Scenario: 瀏覽器連線
- **WHEN** 瀏覽器開啟 `http://<host>:<port>/`
- **THEN** 頁面顯示持續更新的監視畫面

### Requirement: 支援多個同時連線的客戶端
系統 SHALL 允許多個客戶端同時觀看串流，每個客戶端都收到持續更新的影格。

#### Scenario: 兩個客戶端
- **WHEN** 兩個瀏覽器分頁同時開啟串流
- **THEN** 兩個分頁的畫面都持續更新

### Requirement: 客戶端斷線不影響服務
系統 SHALL 在客戶端中途斷線時只結束該連線；程式 MUST NOT 因此終止，其他客戶端與影像處理 MUST 持續運作。

#### Scenario: 客戶端關閉連線
- **WHEN** 一個正在觀看串流的客戶端關閉連線
- **THEN** 程式持續執行，其他客戶端的畫面持續更新，新客戶端仍可連線
