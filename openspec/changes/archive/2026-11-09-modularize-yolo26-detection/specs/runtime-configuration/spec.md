# Spec Delta

## ADDED Requirements

### Requirement: 模型路徑設定
系統 SHALL 從 `BM_MODEL_PATH` 讀取 YOLO26n ONNX 模型路徑，預設為 `models/yolo26n.onnx`（相對於工作目錄）。

#### Scenario: 自訂模型路徑
- **WHEN** 以 `BM_MODEL_PATH=/opt/bm/yolo26n.onnx` 啟動
- **THEN** 系統從該路徑載入模型

### Requirement: 網頁服務 port 設定
系統 SHALL 從 `BM_PORT` 讀取網頁介面與 API 的 port，預設為 `18080`；值無效（非 1–65535 的整數）時 SHALL 在開啟攝影機前輸出包含 `BM_PORT` 的錯誤訊息，並以非 0 結束碼結束。

#### Scenario: 預設 port
- **WHEN** 沒有設定 `BM_PORT` 就啟動
- **THEN** 網頁介面在 port 18080 提供

#### Scenario: 無效 port
- **WHEN** 以 `BM_PORT=abc` 啟動
- **THEN** 程式輸出包含 `BM_PORT` 的錯誤訊息，且結束碼不為 0

### Requirement: 偵測門檻設定
系統 SHALL 從 `BM_MOTION_PX` 讀取移動位移門檻（正數像素，預設 `20`），從 `BM_PERSON_CONF` 讀取人物信心門檻（0 到 1 之間，預設 `0.45`）；值無效時 SHALL 輸出含變數名稱的錯誤並以非 0 結束碼結束。

#### Scenario: 調高靈敏度
- **WHEN** 以 `BM_MOTION_PX=10` 啟動
- **THEN** 人物位移超過 10 像素就判定為移動

#### Scenario: 無效門檻
- **WHEN** 以 `BM_PERSON_CONF=1.5` 啟動
- **THEN** 程式輸出包含 `BM_PERSON_CONF` 的錯誤訊息，且結束碼不為 0

## REMOVED Requirements

### Requirement: 串流 port 設定
**Reason**: MJPEG 串流伺服器已移除（見 `video-streaming`）。
**Migration**: 改用 `BM_PORT` 設定唯一的網頁服務 port；`BM_STREAM_PORT` 會被忽略。
