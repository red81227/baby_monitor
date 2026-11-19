# Spec Delta

## ADDED Requirements

### Requirement: 網頁設定目錄
系統 SHALL 從 `BM_OVERRIDES_DIR` 讀取網頁設定（目前只有 ROI）的保存目錄，預設為 `overrides`（相對於工作目錄）。目錄不存在時 SHALL 在第一次寫入時建立；無法建立或寫入時，寫入請求 SHALL 失敗並回報原因，MUST NOT 影響監看。README 的環境變數表 SHALL 說明此設定，以及容器部署時必須掛載為可寫入的主機目錄。

#### Scenario: 預設目錄
- **WHEN** 沒有設定 `BM_OVERRIDES_DIR` 就啟動，從網頁設定 ROI
- **THEN** ROI 檔寫在工作目錄下的 `overrides/`

#### Scenario: 目錄唯讀
- **WHEN** `BM_OVERRIDES_DIR` 指向唯讀的目錄，從網頁設定 ROI
- **THEN** 寫入請求回應錯誤並說明無法寫入，監看與偵測照常進行
