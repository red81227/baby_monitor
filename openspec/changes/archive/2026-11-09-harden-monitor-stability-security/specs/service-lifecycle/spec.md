# Spec Delta

## Purpose

定義監視器服務的啟動失敗處理與正常關閉行為，讓它能被 systemd 或手動操作可靠地啟停。

## ADDED Requirements

### Requirement: 攝影機無法開啟時明確失敗
系統 SHALL 在攝影機無法開啟時輸出指出攝影機索引的錯誤訊息，並以非 0 結束碼結束。

#### Scenario: 沒有攝影機
- **WHEN** 在沒有可用攝影機的機器上啟動程式
- **THEN** 程式輸出攝影機開啟失敗的錯誤訊息，且結束碼不為 0

### Requirement: 收到終止訊號時正常關閉
系統 SHALL 在收到 SIGINT 或 SIGTERM 後停止處理新影格、關閉串流伺服器與現有連線、釋放攝影機與 GPU 資源，並在 5 秒內以結束碼 0 結束。

#### Scenario: 按下 Ctrl+C
- **WHEN** 程式執行中且有一個串流客戶端連線時，使用者按下 Ctrl+C
- **THEN** 程式在 5 秒內以結束碼 0 結束，且串流 port 隨即可被重新綁定

#### Scenario: systemd 停止服務
- **WHEN** 對程式送出 SIGTERM
- **THEN** 程式在 5 秒內以結束碼 0 結束

### Requirement: 攝影機串流結束時退出
系統 SHALL 在攝影機無法再提供影格時，輸出說明訊息並依「正常關閉」的步驟結束，結束碼不為 0。

#### Scenario: 攝影機被拔除
- **WHEN** 程式執行中 USB 攝影機被拔除
- **THEN** 程式輸出攝影機中斷的訊息，釋放資源後以非 0 結束碼結束
