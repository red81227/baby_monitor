# service-lifecycle Specification

## Purpose

定義監視器服務的啟動失敗處理與正常關閉行為，讓它能被 systemd 或手動操作可靠地啟停。

## Requirements

### Requirement: 攝影機無法開啟時明確失敗
系統 SHALL 在啟動時攝影機無法開啟，輸出指出攝影機裝置路徑的錯誤訊息，並以非 0 結束碼結束。

#### Scenario: 沒有攝影機
- **WHEN** 在沒有可用攝影機的機器上啟動程式
- **THEN** 程式輸出攝影機開啟失敗的錯誤訊息，且結束碼不為 0

#### Scenario: 指定的裝置不存在
- **WHEN** 以 `BM_CAMERA_DEVICE=/dev/video9` 啟動，而該裝置不存在
- **THEN** 程式輸出包含 `/dev/video9` 的錯誤訊息，且結束碼不為 0

### Requirement: 收到終止訊號時正常關閉
系統 SHALL 在收到 SIGINT 或 SIGTERM 後停止處理新影格、關閉網頁服務與現有連線（包含即時影像連線）、停止攝影機重連、釋放攝影機與推論資源，並在 5 秒內以結束碼 0 結束。

#### Scenario: 按下 Ctrl+C
- **WHEN** 程式執行中且有瀏覽器開著監視頁面時，使用者按下 Ctrl+C
- **THEN** 程式在 5 秒內以結束碼 0 結束，且網頁 port 隨即可被重新綁定

#### Scenario: systemd 停止服務
- **WHEN** 對程式送出 SIGTERM
- **THEN** 程式在 5 秒內以結束碼 0 結束

#### Scenario: 攝影機中斷時停止服務
- **WHEN** 攝影機處於中斷、系統正在重連時送出 SIGTERM
- **THEN** 程式在 5 秒內以結束碼 0 結束

### Requirement: 模型無法載入時明確失敗
系統 SHALL 在模型檔不存在或無法載入時，於開啟攝影機與網路服務前輸出包含模型路徑的錯誤訊息，並以非 0 結束碼結束。

#### Scenario: 模型檔不存在
- **WHEN** 以 `BM_MODEL_PATH=/nonexistent.onnx` 啟動
- **THEN** 程式輸出包含 `/nonexistent.onnx` 的錯誤訊息，且結束碼不為 0，也沒有開始監聽任何 port

### Requirement: systemd watchdog 通知
系統 SHALL 在環境中有 `NOTIFY_SOCKET` 時，於服務可以接受連線後送出 `READY=1`；若同時設定了 `WATCHDOG_USEC`，SHALL 在影像擷取（或攝影機重連）與偵測排程都持續運作時，以不超過該週期一半的間隔送出 `WATCHDOG=1`，任一停滯超過該週期時 MUST 停止送出。沒有 `NOTIFY_SOCKET` 時 SHALL 照常運作。

#### Scenario: 以 Type=notify 啟動
- **WHEN** 以 `Type=notify`、`WatchdogSec=30` 的 systemd 服務啟動
- **THEN** 服務在網頁可以連線後進入 active 狀態，正常運作期間不會被 watchdog 重啟

#### Scenario: 處理停滯
- **WHEN** 影像擷取或偵測排程停滯超過 watchdog 週期
- **THEN** 系統不再送出 `WATCHDOG=1`，由 systemd 判定逾時並重啟服務

#### Scenario: 手動執行
- **WHEN** 不透過 systemd 直接執行程式
- **THEN** 程式照常運作，不輸出 watchdog 相關錯誤

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
