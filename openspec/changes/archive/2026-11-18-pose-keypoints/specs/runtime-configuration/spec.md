# Spec Delta

## MODIFIED Requirements

### Requirement: 模型路徑設定
系統 SHALL 從 `BM_MODEL_PATH` 讀取 YOLO26n 系列 ONNX 模型路徑（姿態模型或純偵測模型），預設為 `models/yolo26n-pose.onnx`（相對於工作目錄）。

#### Scenario: 預設模型
- **WHEN** 沒有設定 `BM_MODEL_PATH` 就啟動
- **THEN** 系統從 `models/yolo26n-pose.onnx` 載入模型

#### Scenario: 自訂模型路徑
- **WHEN** 以 `BM_MODEL_PATH=/opt/bm/yolo26n.onnx` 啟動
- **THEN** 系統從該路徑載入模型
