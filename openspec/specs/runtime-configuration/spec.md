# runtime-configuration Specification

## Purpose

定義監視器執行期設定的來源、預設值與驗證規則，讓設定錯誤能在啟動階段明確失敗。

## Requirements

### Requirement: 模型路徑設定
系統 SHALL 從 `BM_MODEL_PATH` 讀取 YOLO26n 系列 ONNX 模型路徑（姿態模型或純偵測模型），預設為 `models/yolo26n-pose.onnx`（相對於工作目錄）。

#### Scenario: 預設模型
- **WHEN** 沒有設定 `BM_MODEL_PATH` 就啟動
- **THEN** 系統從 `models/yolo26n-pose.onnx` 載入模型

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

### Requirement: 攝影機設定
系統 SHALL 從 `BM_CAMERA_DEVICE` 讀取攝影機裝置路徑（預設 `/dev/video0`），從 `BM_CAPTURE_WIDTH`、`BM_CAPTURE_HEIGHT` 讀取要求的擷取解析度（正整數，預設 1280 與 720）；值無效時 SHALL 在開啟攝影機前輸出含變數名稱的錯誤並以非 0 結束碼結束。

#### Scenario: 指定第二支攝影機
- **WHEN** 以 `BM_CAMERA_DEVICE=/dev/video2` 啟動
- **THEN** 系統從 `/dev/video2` 擷取影像

#### Scenario: 無效解析度
- **WHEN** 以 `BM_CAPTURE_WIDTH=0` 啟動
- **THEN** 程式輸出包含 `BM_CAPTURE_WIDTH` 的錯誤訊息，且結束碼不為 0

### Requirement: 偵測頻率設定
系統 SHALL 從 `BM_DETECT_MIN_HZ` 與 `BM_DETECT_MAX_HZ` 讀取人物偵測的下限與上限頻率（正數，預設 1 與 10）；值無效或下限大於上限時 SHALL 在開啟攝影機前輸出含變數名稱的錯誤並以非 0 結束碼結束。

#### Scenario: 固定頻率
- **WHEN** 以 `BM_DETECT_MIN_HZ=5`、`BM_DETECT_MAX_HZ=5` 啟動
- **THEN** 人物偵測不論有無動靜都大約每秒 5 次

#### Scenario: 下限大於上限
- **WHEN** 以 `BM_DETECT_MIN_HZ=10`、`BM_DETECT_MAX_HZ=2` 啟動
- **THEN** 程式輸出包含 `BM_DETECT_MIN_HZ` 的錯誤訊息，且結束碼不為 0
