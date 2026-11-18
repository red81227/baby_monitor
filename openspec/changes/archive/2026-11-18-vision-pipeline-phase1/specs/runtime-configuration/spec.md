# Spec Delta

## ADDED Requirements

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
