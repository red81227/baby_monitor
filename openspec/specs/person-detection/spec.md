# person-detection Specification

## Purpose

以物件偵測模型在每張影格中找出人物（寶寶）的位置與信心值，作為移動判定、畫面標示與狀態回報的依據。

## Requirements

### Requirement: 偵測影格中的人物
系統 SHALL 對每張處理的影格執行 YOLO26n 推論，只採用類別為 person（COCO class 0）且信心值不低於設定門檻的結果，並回報信心值最高的那一個人物框；座標 MUST 以原始影格的像素為單位。

#### Scenario: 畫面中有人
- **WHEN** 處理一張含有清楚人物、尺寸為 810x1080 的圖片
- **THEN** 系統回報至少一個人物，其框完整落在 0–810 × 0–1080 的範圍內，且框住該人物

#### Scenario: 畫面中沒有人
- **WHEN** 處理一張不含人物的影格
- **THEN** 系統回報「未偵測到人物」

#### Scenario: 非人物物體
- **WHEN** 影格中只有偵測信心很高的非人物物體（例如公車）
- **THEN** 該物體不被回報為人物

### Requirement: 支援任意輸入尺寸
系統 SHALL 以等比例縮放加補邊的方式，把任意尺寸的影格轉成模型輸入，並把結果正確對應回原始座標；影格尺寸在執行中改變時 MUST 照常處理。

#### Scenario: 直式與橫式影格
- **WHEN** 先後處理 640x480 與 810x1080 的影格
- **THEN** 兩者回報的人物框都對齊各自影格中的人物

### Requirement: 優先使用 GPU 推論
系統 SHALL 在可用時以 CUDA 執行推論；CUDA 無法使用時 SHALL 改用 CPU 繼續運作，並在啟動時輸出實際使用的執行裝置。

#### Scenario: 有 GPU
- **WHEN** 在有 NVIDIA GPU 和 CUDA 執行環境的機器上啟動
- **THEN** 啟動訊息顯示使用 CUDA

#### Scenario: 沒有 GPU
- **WHEN** 在無法使用 CUDA 的環境啟動
- **THEN** 啟動訊息顯示使用 CPU 的警告，且偵測仍會運作
