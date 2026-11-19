# Spec Delta

## ADDED Requirements

### Requirement: 停滯結束時間設定
系統 SHALL 從 `BM_STALL_EXIT_SECONDS` 讀取停滯結束時間（秒），預設為 0（停用）。值必須是 0，或介於 10 與 3600 之間的整數；值無效時 SHALL 在開啟攝影機前輸出包含 `BM_STALL_EXIT_SECONDS` 的錯誤訊息，並以非 0 結束碼結束。README 的環境變數表 SHALL 說明此設定與其在容器部署中的用途。

#### Scenario: 預設停用
- **WHEN** 沒有設定 `BM_STALL_EXIT_SECONDS` 就啟動
- **THEN** 停滯結束時間為 0，不做停滯結束檢查

#### Scenario: 無效值
- **WHEN** 以 `BM_STALL_EXIT_SECONDS=5` 啟動
- **THEN** 程式輸出包含 `BM_STALL_EXIT_SECONDS` 的錯誤訊息，結束碼不為 0，也沒有開啟攝影機
