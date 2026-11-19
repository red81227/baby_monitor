# Spec Delta

## ADDED Requirements

### Requirement: 停滯時自行結束
系統 SHALL 在設定了停滯結束時間（`BM_STALL_EXIT_SECONDS` 大於 0）時，持續檢查影像擷取（或攝影機重連）與偵測排程是否在運作；任一停滯超過該時間時，MUST 輸出指出停滯的部分與時間的錯誤訊息，並在 5 秒內以非 0 結束碼結束，讓外部的重啟機制（例如容器的重啟策略）重新啟動服務。攝影機中斷而系統持續重連的期間 MUST NOT 視為停滯。未設定或設為 0 時 SHALL 不做此檢查。此功能與 systemd watchdog 通知互不影響，可以同時啟用。

#### Scenario: 偵測停滯
- **WHEN** 以 `BM_STALL_EXIT_SECONDS=60` 執行，分析執行緒停住超過 60 秒
- **THEN** 程式輸出指出分析停滯的錯誤訊息，並以非 0 結束碼結束

#### Scenario: 攝影機拔除
- **WHEN** 以 `BM_STALL_EXIT_SECONDS=60` 執行，攝影機被拔除 3 分鐘後再接回
- **THEN** 程式在這段期間持續執行並重連，不會因此結束；攝影機接回後恢復擷取

#### Scenario: 未啟用
- **WHEN** 沒有設定 `BM_STALL_EXIT_SECONDS` 就執行
- **THEN** 程式不做停滯結束檢查，行為與原本相同
