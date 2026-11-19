# Spec Delta

## Purpose

以設定檔定義「狀況」：系統從人物偵測與移動分數算出固定的一組訊號，依規則判斷何時觸發狀況、交給誰處理；規則上線後只改檔案即可調整，且修改錯誤不會中斷監看或關掉重要提醒。

## ADDED Requirements

### Requirement: 訊號
系統 SHALL 從人物偵測結果與移動分數算出以下訊號，每個值都帶有來源影格的擷取時間：`person_present`、`person_count`、`person_conf`、`face_visible`、`body_visible`、`prone_likely`、`keypoint_motion`、`near_bed_edge`、`activity`。訊號的定義 SHALL 記錄在 README。偵測不到人物時，依賴人物的訊號 MUST 為「未知」，而不是 false 或 0。`face_visible` 只代表臉部關鍵點（鼻、雙眼）的可見度；README MUST 註明它無法偵測口鼻被蓋住（關鍵點模型會依眼睛與耳朵的位置推算被遮住的鼻子，並給出高可見度），口鼻被蓋住改由 VLM 巡檢偵測。

#### Scenario: 臉被遮住
- **WHEN** 偵測到人物，鼻與雙眼的可見度都低於關鍵點可見度門檻，雙肩可見度高於門檻
- **THEN** `face_visible` 為 false，`body_visible` 為 true

#### Scenario: 沒有人物
- **WHEN** 某次偵測沒有偵測到人物
- **THEN** `person_present` 為 false、`person_count` 為 0，`face_visible`、`body_visible`、`prone_likely`、`keypoint_motion`、`near_bed_edge` 為未知

#### Scenario: 純偵測模型
- **WHEN** 使用不提供關鍵點的模型且偵測到人物
- **THEN** 依賴關鍵點的訊號為未知，`person_present`、`person_count`、`person_conf`、`activity` 照常提供

### Requirement: 狀況規則檔
系統 SHALL 從規則檔讀取狀況定義。每個狀況 MUST 有唯一的 `id`、一個以上的條件、路由（`direct`、`llm` 或 `suppress`）、嚴重程度（`info`、`warning` 或 `critical`）與冷卻時間；規則檔 MUST 有整數 `version`。規則檔也 SHALL 能定義嬰兒床區域與關鍵點可見度門檻。格式 SHALL 記錄在 README，並隨附一份預設規則檔。

#### Scenario: 預設規則檔
- **WHEN** 以隨附的預設規則檔啟動
- **THEN** 載入成功，包含臉被蓋住、趴睡、醒來持續活動、不在床上、有大人進入、靠近床邊六個狀況，以及口鼻被蓋住（`airway_covered`）一個巡檢

### Requirement: 條件語意
每個條件 SHALL 由訊號名稱、比較方式（`is`、`above`、`below`）與值組成，並可指定持續時間 `for`；同一狀況的多個條件 MUST 全部成立狀況才成立。指定 `for` 的條件 MUST 依擷取時間連續成立達該時間才算成立；訊號為未知時條件不成立，且其連續計時歸零。條件 SHALL 可另外指定容許中斷 `grace`（預設 0）：條件不成立或訊號未知的時間連續短於 `grace` 時，`for` 的計時 MUST NOT 歸零（中斷期間的時間計入 `for`），且已經成立的條件在這段中斷期間 MUST 仍視為成立；中斷達到 `grace` 時，條件不成立且計時歸零。

#### Scenario: 持續時間未達
- **WHEN** 條件為 `face_visible is false for 10s`，臉部連續 6 秒看不到後又看到
- **THEN** 該狀況不成立

#### Scenario: 持續時間達到
- **WHEN** 同一條件下，臉部連續 10 秒以上看不到且身體可見
- **THEN** 該狀況在連續第 10 秒時成立

#### Scenario: 中途訊號未知
- **WHEN** 條件為 `face_visible is false for 10s`，第 5 秒起有一次偵測不到人物，之後臉部繼續看不到
- **THEN** 計時從再次偵測到人物時重新開始

#### Scenario: 中斷短於容許時間
- **WHEN** 條件為 `face_visible is false for 10s grace 1s`，臉部看不到的第 4 秒起有 0.3 秒被判定為看得到，之後繼續看不到
- **THEN** 計時不歸零，該狀況在第 10 秒時成立

#### Scenario: 中斷超過容許時間
- **WHEN** 同一條件下，臉部看不到的第 4 秒起連續 1.5 秒被判定為看得到，之後繼續看不到
- **THEN** 計時從再次看不到時重新開始

#### Scenario: 成立中的短暫閃爍
- **WHEN** 冷卻時間為 30 秒、條件帶 `grace: 1s` 的狀況已觸發，之後條件短暫不成立 0.3 秒又成立
- **THEN** 該狀況維持成立，不會因為這次閃爍再觸發一次

### Requirement: 觸發與冷卻
狀況從不成立變成成立時 SHALL 觸發一次；持續成立時，距離上一次觸發達到冷卻時間 SHALL 再觸發一次；冷卻時間為 0 時持續成立期間 MUST NOT 重複觸發。每次觸發 MUST 記錄狀況 `id`、觸發時間、當時的訊號值、規則檔 `version` 與內容雜湊值。

#### Scenario: 持續成立
- **WHEN** 冷卻時間為 60 秒的狀況持續成立 150 秒
- **THEN** 共觸發 3 次（第 0、60、120 秒）

#### Scenario: 冷卻時間為 0
- **WHEN** 冷卻時間為 0 的狀況持續成立 10 分鐘，之後不成立再成立
- **THEN** 共觸發 2 次（兩次變成成立時各一次）

#### Scenario: 觸發紀錄
- **WHEN** 任一狀況觸發
- **THEN** 觸發紀錄中可查到該狀況、時間、訊號值與當時的規則版本

### Requirement: 規則驗證
系統 SHALL 在載入規則檔時驗證格式、訊號名稱、比較方式與值的型別、`id` 不重複、引用的 prompt 檔存在。啟動時規則檔不存在或驗證失敗 SHALL 輸出含檔案路徑與錯誤位置的訊息，並以非 0 結束碼結束。

#### Scenario: 拼錯訊號名稱
- **WHEN** 規則檔中有條件使用不存在的訊號 `face_visable`，以此啟動
- **THEN** 程式輸出包含規則檔路徑與 `face_visable` 的錯誤訊息，結束碼不為 0

### Requirement: 底線保護
嚴重程度為 `critical` 的狀況，路由 MUST NOT 為 `llm` 或 `suppress`，且 MUST NOT 被任何 `suppress` 狀況壓制；違反時規則驗證 SHALL 失敗。巡檢的嚴重程度 MAY 為 `critical`：巡檢的 VLM 結果只能讓提醒發生，不存在撤回提醒的路徑；`critical` 巡檢產生的警報同樣 MUST NOT 被壓制。

#### Scenario: critical 交給 LLM
- **WHEN** 規則檔把 `critical` 狀況的路由設為 `llm`
- **THEN** 驗證失敗，錯誤訊息指出該狀況 `id` 與原因

#### Scenario: 壓制 critical
- **WHEN** 規則檔中的 `suppress` 狀況設定為壓制 `critical` 以下（含 `critical`）的警報
- **THEN** 驗證失敗

### Requirement: 巡檢規則
規則檔 SHALL 可以在 `patrols` 區塊定義巡檢：每隔固定時間把最近的影格交給 VLM 判斷某個狀況是否發生，用於第一段訊號無法偵測的狀況。每個巡檢 MUST 有唯一的 `id`（不可與狀況重複）、嚴重程度、間隔 `every`、prompt 與冷卻時間，並可指定送出的影格數 `frames`（預設 2，相隔 1 秒）、確認次數 `confirm`（預設 1）、事件片段範圍 `clip`、前提條件 `while`（格式同 `when`，省略時一律巡檢）、`timeout`、`shadow` 與 `enabled`。巡檢的格式 SHALL 記錄在 README，驗證、重新載入與規則版本的規定與狀況相同。

#### Scenario: 預設巡檢
- **WHEN** 以隨附的預設規則檔啟動
- **THEN** `airway_covered` 巡檢為 `critical`，沒有前提條件，即使偵測不到人物也照常巡檢

#### Scenario: 巡檢 id 重複
- **WHEN** 規則檔中巡檢與狀況使用相同的 `id`
- **THEN** 驗證失敗，錯誤訊息指出重複的 `id`

#### Scenario: 前提條件不成立
- **WHEN** 巡檢設定 `while: [{ signal: person_present, is: true }]`，畫面中沒有人物
- **THEN** 該巡檢暫停，不呼叫 VLM，也不視為巡檢失敗

### Requirement: 執行中重新載入
系統 SHALL 在規則檔或其引用的 prompt 檔變更後 5 秒內重新載入。新規則驗證成功時改用新規則；驗證失敗時 MUST 繼續使用目前的規則，並在 log 與狀態查詢中回報錯誤，直到下一次成功載入。重新載入 MUST NOT 中斷擷取、偵測或即時影像。

#### Scenario: 修改門檻
- **WHEN** 服務執行中，把某條件的 `for` 從 10s 改為 15s 並存檔
- **THEN** 5 秒內新門檻生效，狀態查詢顯示新的規則版本

#### Scenario: 改壞規則檔
- **WHEN** 服務執行中，存入一個 YAML 語法錯誤的規則檔
- **THEN** 服務繼續以原本的規則運作，狀態查詢回報載入錯誤；修正後再存檔，錯誤消失並套用新規則

### Requirement: 影子模式
狀況 SHALL 可以標示為影子模式：照常判斷、觸發，路由為 `llm` 時也照常送 LLM 判定，但產生的警報 MUST 只記錄、不送出，且紀錄中標示為影子模式。

#### Scenario: 試用新規則
- **WHEN** 新增一個 `shadow: true` 的狀況，該狀況成立
- **THEN** 觸發紀錄與警報紀錄出現該狀況並標示為影子模式，監視頁面不會跳出提醒

### Requirement: 觀測紀錄與離線重播
系統 SHALL 把規則判斷用到的訊號連同擷取時間寫入觀測紀錄檔，並提供命令列工具：輸入規則檔與觀測紀錄檔，以紀錄中的擷取時間（而不是實際時間）重新判斷，輸出每次觸發的狀況與時間。相同輸入 MUST 產生相同輸出。巡檢需要 VLM，重播工具 MUST NOT 執行巡檢，輸出中 SHALL 註明已略過的巡檢。

#### Scenario: 調整前先重播
- **WHEN** 以修改後的規則檔與前一晚的觀測紀錄執行重播工具兩次
- **THEN** 兩次輸出相同，且與服務即時判斷所用的條件語意一致
