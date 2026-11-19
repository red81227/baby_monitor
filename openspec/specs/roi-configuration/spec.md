# roi-configuration Specification

## Purpose
定義嬰兒床 ROI（`bed_roi`）從哪裡來、誰優先，以及如何從監視頁面安全地設定與還原，讓 ROI 能在現場直接框選並立即生效，又不會在沒有認證的期間被跨站請求或多個頁面同時修改弄亂。

## Requirements

### Requirement: ROI 來源與優先順序
生效的 ROI SHALL 依序取自：網頁設定的 ROI 檔、規則檔的 `bed_roi`；兩者都沒有時為未設定。ROI 檔存在且有效時，規則檔的 `bed_roi` MUST NOT 生效。系統 MUST NOT 改寫規則檔。

#### Scenario: 兩者都有
- **WHEN** 規則檔設定 `bed_roi: [0.2, 0.15, 0.6, 0.75]`，網頁設定的 ROI 為 `[0.1, 0.1, 0.5, 0.5]`
- **THEN** activity 與 `near_bed_edge` 都以 `[0.1, 0.1, 0.5, 0.5]` 計算，`/api/situations` 回報來源為網頁設定

#### Scenario: 只有規則檔
- **WHEN** 沒有 ROI 檔，規則檔設定了 `bed_roi`
- **THEN** 生效的 ROI 是規則檔的值，來源為規則檔

#### Scenario: 規則檔不被改動
- **WHEN** 從網頁設定 ROI
- **THEN** 規則檔的內容與修改時間都不變

### Requirement: 從網頁設定 ROI
系統 SHALL 提供設定 ROI 的寫入端點，接受以畫面比例表示的 `[x, y, w, h]` 與送出前讀到的規則雜湊值。值 MUST 符合規則檔 `bed_roi` 的驗證，且寬、高都不小於 0.05；成功時 SHALL 在回應前套用，並回傳新的規則雜湊值。

#### Scenario: 設定成功
- **WHEN** 以目前的規則雜湊值送出 `[0.1, 0.1, 0.5, 0.5]`
- **THEN** 回應成功，附上新的雜湊值；立即呼叫 `/api/situations` 時看到新的 ROI 與雜湊值

#### Scenario: 超出畫面
- **WHEN** 送出 `[0.8, 0.1, 0.5, 0.5]`（x + w 大於 1）
- **THEN** 回應 400 並說明原因，生效的 ROI 不變

#### Scenario: 太小
- **WHEN** 送出寬度為 0.01 的 ROI
- **THEN** 回應 400，生效的 ROI 不變

### Requirement: 不能從網頁清除 ROI
寫入端點 MUST NOT 接受「沒有 ROI」的值。要讓 ROI 變成未設定，只能刪除規則檔的 `bed_roi` 並還原網頁設定。

#### Scenario: 送出空值
- **WHEN** 送出 `null` 或空陣列
- **THEN** 回應 400，生效的 ROI 不變

### Requirement: 還原為規則檔
系統 SHALL 提供還原端點，刪除網頁設定的 ROI，使生效的 ROI 回到規則檔的 `bed_roi`（或未設定）；同樣需要送出前讀到的規則雜湊值，成功時在回應前套用。

#### Scenario: 還原
- **WHEN** 網頁設定了 ROI，規則檔有 `bed_roi`，呼叫還原端點
- **THEN** 生效的 ROI 回到規則檔的值，來源為規則檔

#### Scenario: 沒有可還原的設定
- **WHEN** 沒有網頁設定的 ROI 時呼叫還原端點
- **THEN** 回應成功，生效的 ROI 不變

### Requirement: 防止覆蓋他人的修改
寫入與還原時送出的規則雜湊值和目前的值不同時，系統 MUST 拒絕（409），並回傳目前的 ROI 與雜湊值。

#### Scenario: 兩個頁面同時編輯
- **WHEN** 頁面 A 與 B 讀到相同的雜湊值，A 先儲存成功，B 接著以舊雜湊值儲存
- **THEN** B 收到 409 與 A 儲存後的 ROI，A 的設定不被覆蓋

### Requirement: 規則載入失敗時拒絕寫入
規則目前處於載入失敗狀態時，寫入與還原 MUST 被拒絕（409），並說明需要先修正規則檔。

#### Scenario: 規則檔改壞後設定 ROI
- **WHEN** 規則檔存入語法錯誤的內容後，從網頁設定 ROI
- **THEN** 回應 409 並附上規則載入錯誤，ROI 檔不變

### Requirement: ROI 隨規則重新載入
ROI 檔 SHALL 與規則檔一起被監看與重新載入，並納入規則內容雜湊值。ROI 檔內容無效時 SHALL 與規則檔驗證失敗相同處理：啟動時以非 0 結束碼結束；執行中則沿用目前的規則並回報錯誤。

#### Scenario: 雜湊值反映 ROI
- **WHEN** 只修改網頁設定的 ROI，規則檔不變
- **THEN** 規則雜湊值改變，之後的觸發與警報紀錄帶有新的雜湊值；`version` 不變

#### Scenario: ROI 檔損壞
- **WHEN** 執行中 ROI 檔被改成無法解析的內容
- **THEN** 系統繼續使用原本的規則與 ROI，狀態查詢回報包含 ROI 檔路徑的載入錯誤

### Requirement: ROI 設定保存
網頁設定的 ROI SHALL 存在 `BM_OVERRIDES_DIR` 目錄中，服務或容器重啟後 MUST 仍然有效。寫入 MUST 是原子的：寫到一半當機時，不會留下半個檔案。

#### Scenario: 重啟後
- **WHEN** 從網頁設定 ROI 後重啟服務
- **THEN** 生效的 ROI 仍是網頁設定的值

### Requirement: 跨站請求防護
在沒有認證的期間，寫入與還原端點 MUST 只接受 `Content-Type: application/json`，且帶有 `Origin` 時必須與請求的主機相同，否則拒絕（403）；請求內容大於 4 KiB 時 MUST 拒絕。

#### Scenario: 其他網站送出請求
- **WHEN** 從另一個網站的頁面對寫入端點送出請求，`Origin` 是該網站
- **THEN** 回應 403，生效的 ROI 不變

#### Scenario: 表單送出
- **WHEN** 以 `application/x-www-form-urlencoded` 送出寫入請求
- **THEN** 回應 4xx，生效的 ROI 不變

### Requirement: ROI 修改紀錄
每次成功設定或還原 ROI，系統 SHALL 在觸發與警報紀錄中新增一筆系統紀錄，內容包含舊的 ROI 與來源、新的 ROI 與來源，以及請求的來源 IP；嚴重程度為 `info`。

#### Scenario: 查詢修改紀錄
- **WHEN** 從網頁設定 ROI 後呼叫 `GET /api/alerts`
- **THEN** 最新一筆是 ROI 修改的系統紀錄，含新舊值與來源 IP
