# Spec Delta

## Purpose

定義監視器執行期設定的來源、預設值與驗證規則，讓設定錯誤能在啟動階段明確失敗。

## ADDED Requirements

### Requirement: 串流 port 設定
系統 SHALL 以環境變數 `BM_STREAM_PORT` 設定串流 port，未設定時預設為 `8080`；值無效（非 1–65535 的整數）時 SHALL 在開啟攝影機前輸出包含 `BM_STREAM_PORT` 的錯誤訊息，並以非 0 結束碼結束。

#### Scenario: 使用預設值
- **WHEN** 不設定任何環境變數就啟動程式
- **THEN** 串流在 port 8080 提供

#### Scenario: 自訂 port
- **WHEN** 以 `BM_STREAM_PORT=9090` 啟動程式
- **THEN** 串流在 port 9090 提供

#### Scenario: 無效 port
- **WHEN** 以 `BM_STREAM_PORT=abc` 啟動程式
- **THEN** 程式輸出包含 `BM_STREAM_PORT` 的錯誤訊息，且結束碼不為 0
