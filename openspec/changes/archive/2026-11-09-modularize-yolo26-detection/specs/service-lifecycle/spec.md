# Spec Delta

## MODIFIED Requirements

### Requirement: 收到終止訊號時正常關閉
系統 SHALL 在收到 SIGINT 或 SIGTERM 後停止處理新影格、關閉網頁服務與現有連線、釋放攝影機與推論資源，並在 5 秒內以結束碼 0 結束。

#### Scenario: 按下 Ctrl+C
- **WHEN** 程式執行中且有瀏覽器開著監視頁面時，使用者按下 Ctrl+C
- **THEN** 程式在 5 秒內以結束碼 0 結束，且網頁 port 隨即可被重新綁定

#### Scenario: systemd 停止服務
- **WHEN** 對程式送出 SIGTERM
- **THEN** 程式在 5 秒內以結束碼 0 結束

## ADDED Requirements

### Requirement: 模型無法載入時明確失敗
系統 SHALL 在模型檔不存在或無法載入時，於開啟攝影機與網路服務前輸出包含模型路徑的錯誤訊息，並以非 0 結束碼結束。

#### Scenario: 模型檔不存在
- **WHEN** 以 `BM_MODEL_PATH=/nonexistent.onnx` 啟動
- **THEN** 程式輸出包含 `/nonexistent.onnx` 的錯誤訊息，且結束碼不為 0，也沒有開始監聽任何 port
