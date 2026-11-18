# Spec Delta

## ADDED Requirements

### Requirement: 偵測 metadata 包含關鍵點
系統 SHALL 在推送的偵測 metadata 中附上人物的關鍵點（每個包含 x、y 與可見度，座標以主碼流像素為單位）；沒有偵測到人物或模型不提供關鍵點時，該欄位 MUST 明確標示為沒有。新增欄位 MUST NOT 改變既有欄位的名稱與意義。欄位格式 SHALL 記錄在 README。

#### Scenario: 姿態模型偵測到人物
- **WHEN** 使用姿態模型，系統對 `seq` = N 的影格偵測到人物
- **THEN** 客戶端收到 `seq` = N 的 metadata，其中有 17 個關鍵點，座標都落在主碼流的寬高範圍內

#### Scenario: 沒有關鍵點
- **WHEN** 沒有偵測到人物，或使用純偵測模型
- **THEN** metadata 的關鍵點欄位標示為沒有，其餘欄位與原本相同

#### Scenario: 只認得舊欄位的客戶端
- **WHEN** 只讀取框、信心值與移動欄位的客戶端接收 metadata
- **THEN** 客戶端照常取得這些欄位，不受新增欄位影響
