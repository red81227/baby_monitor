# Spec Delta

## ADDED Requirements

### Requirement: 顯示警報
監視頁面 SHALL 在警報送出後 3 秒內顯示該警報的狀況、嚴重程度、時間與原因說明；`critical` 警報 MUST 以與其他等級明顯不同的方式呈現。未經 LLM 判定的警報 SHALL 標示出來。

#### Scenario: 臉被蓋住
- **WHEN** 頁面開著時送出 `face_covered` 警報
- **THEN** 頁面在 3 秒內以 critical 樣式顯示該警報

### Requirement: 唯讀顯示規則與紀錄
監視頁面 SHALL 顯示目前規則版本、載入錯誤、每個狀況的路由、嚴重程度、是否為影子模式與是否成立，每個巡檢的間隔、最後一次成功的時間與最近一次的結果，以及最近的觸發紀錄與 LLM 判定結果，並可開啟事件片段查看。巡檢停止時 MUST 以與 `critical` 影子模式同等醒目的方式標示。頁面 MUST NOT 提供修改規則的功能。

#### Scenario: 規則檔改壞
- **WHEN** 執行中存入驗證失敗的規則檔
- **THEN** 頁面顯示規則載入錯誤與仍在使用的版本

#### Scenario: 影子模式的狀況
- **WHEN** 規則檔中有影子模式的狀況
- **THEN** 頁面在該狀況旁標示影子模式；若該狀況是 `critical`，標示 MUST 特別醒目

#### Scenario: 查看判定原因
- **WHEN** 使用者點開一筆判定為不提醒的觸發紀錄
- **THEN** 頁面顯示 LLM 的原因說明，並可播放該次事件片段

#### Scenario: 巡檢停止
- **WHEN** VLM 服務停止使 `airway_covered` 巡檢停止
- **THEN** 頁面醒目顯示該巡檢已停止與最後一次成功的時間；VLM 恢復且巡檢成功後標示消失
