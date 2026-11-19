# Spec Delta

## ADDED Requirements

### Requirement: 規則狀態包含 ROI
`GET /api/situations` SHALL 在既有欄位之外回傳生效的 ROI（以畫面比例表示的 `[x, y, w, h]`，未設定時為 null）、ROI 來源（網頁設定、規則檔、未設定）、規則檔中的 `bed_roi`（沒有時為 null）與 `bed_edge_margin`。既有欄位的名稱與意義 MUST NOT 改變，此端點 MUST 仍是唯讀的。

#### Scenario: 網頁設定覆蓋規則檔
- **WHEN** 規則檔與網頁都設定了 ROI 時呼叫 `GET /api/situations`
- **THEN** 生效 ROI 是網頁設定的值，來源為網頁設定，規則檔的 `bed_roi` 欄位仍回傳規則檔的值

#### Scenario: 未設定
- **WHEN** 規則檔與網頁都沒有設定 ROI 時呼叫 `GET /api/situations`
- **THEN** 生效 ROI 為 null，來源為未設定
