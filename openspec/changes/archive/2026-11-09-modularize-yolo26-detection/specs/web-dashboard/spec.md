# Spec Delta

## Purpose

提供一個用瀏覽器就能開啟的監視頁面，讓家長在同一個區網內直接查看即時畫面與移動狀態。

## ADDED Requirements

### Requirement: 監視頁面
系統 SHALL 在網頁 port 的 `/` 提供監視頁面，頁面持續顯示最新影格（至少每秒更新 2 次），並顯示目前是否偵測到移動。

#### Scenario: 開啟頁面
- **WHEN** 使用者用瀏覽器開啟 `http://<host>:<BM_PORT>/`
- **THEN** 頁面顯示持續更新的畫面，以及「有移動 / 沒有移動」的狀態

### Requirement: 後端中斷時提示
頁面 SHALL 在無法取得狀態時顯示後端無法連線的訊息，而不是停在最後一個狀態。

#### Scenario: 程式停止
- **WHEN** 頁面開著時監視器程式被停止
- **THEN** 頁面在幾秒內顯示後端無法連線

### Requirement: 與啟動目錄無關
系統 SHALL 不論從哪個工作目錄啟動，都能提供頁面所需的靜態檔案。

#### Scenario: 從其他目錄啟動
- **WHEN** 在 repo 根目錄執行 `./build/BabyMonitor`
- **THEN** 開啟 `/` 會正常顯示頁面，不會回應 404
