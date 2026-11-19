# Proposal

## Why

`bed_roi`（嬰兒床區域）決定 activity 只計算哪個區域，以及 `near_bed_side` 狀況能不能觸發。但現在只能在規則檔裡手寫畫面比例 `[x, y, w, h]`，頁面上也看不到區域實際在哪裡。結果隨附的規則檔一直沒有設定，`near_bed_side` 從未生效。在頁面上直接用滑鼠框選，是讓這個區域真正被設定、而且設對的最短路徑。順便處理監視頁面幾個影響日常使用的問題：狀態列中英混雜、手機版面、夜間只看影像、警報沒有聲音。

## What Changes

- 監視頁面在即時影像上畫出目前的 ROI 和床邊判定範圍（`bed_edge_margin`），並標示 ROI 的來源：網頁設定、規則檔或未設定
- 新增 ROI 編輯模式：在畫面上用滑鼠或觸控拖拉建立、移動和調整矩形，儲存後幾秒內生效；可以「還原為規則檔」
- 新增 ROI 寫入 API（設定 / 還原）。網頁設定的 ROI 存在資料目錄下的獨立檔案，**優先於**規則檔的 `bed_roi`；規則檔本身不會被程式改寫，`config/` 仍然唯讀掛載
- ROI 檔納入規則的熱重載與內容雜湊值：修改 ROI 會產生新的規則雜湊值，警報紀錄可以分辨 ROI 是否改過；ROI 檔損壞時視同規則載入失敗，沿用舊規則
- 沒有認證期間的防護：檢查 `Origin`、只接受 JSON、限制大小，並以規則雜湊值做樂觀鎖避免多個頁面互相覆蓋；每次修改都記入觸發紀錄（舊值、新值、來源 IP）
- 網頁只能設定或還原 ROI，**不能清除**。清除會讓 `near_bed_side` 靜默失效，必須改規則檔
- `/api/situations` 新增目前生效的 ROI、來源與 `bed_edge_margin`
- 監視頁面優化：狀態文字中文化；寬螢幕時影像和狀況並排；全螢幕 / 只看影像模式；人框、骨架、ROI 疊圖可以個別開關；警報提示音；觸發紀錄可依嚴重程度篩選；頁面不可見時暫停輪詢與影像
- **BREAKING（規格）**：推翻 `cascade-alert-architecture` 中「頁面 MUST NOT 提供修改規則的功能」。ROI 成為唯一例外，其他規則仍然只能改檔

### 不在範圍內

- 認證與權限（之後另開 change；本 change 的防護只擋跨站請求，不擋同網段的人）
- 多個 ROI、多邊形 ROI、在網頁編輯 `bed_edge_margin` 或其他規則
- HTTPS（因此瀏覽器的系統通知 API 在區網 http 下無法使用，見 design.md）

## Capabilities

### New Capabilities
- `roi-configuration`: ROI 的來源與優先順序、網頁設定的寫入、驗證、保存、熱重載與雜湊值、還原、併發保護、沒有認證期間的防護與修改紀錄

### Modified Capabilities
- `web-dashboard`: 新增 ROI 顯示與編輯、規則唯讀的 ROI 例外、狀態中文化、版面與全螢幕、疊圖開關、警報提示音、紀錄篩選、背景分頁暫停
- `status-api`: `/api/situations` 新增生效 ROI、ROI 來源與 `bed_edge_margin`
- `runtime-configuration`: 新增 `BM_OVERRIDES_DIR`（網頁設定檔目錄）

## Impact

- 程式碼：`Rules`/`loadRules` 合併 ROI 檔並納入雜湊值與監看檔案；`RulesStore` 新增同步重新載入供 API 使用；新增 ROI 寫入路由（Crow）；`AlertManager` 新增 ROI 修改的系統紀錄；`Config` 新增 `BM_OVERRIDES_DIR`
- 前端：`app.js` 加入 ROI 疊圖與編輯互動、疊圖開關、全螢幕；`situations.js` 加入 ROI 狀態、提示音、篩選；共用工具抽出；`index.html`/`style.css` 版面調整
- API：新增 `PUT /api/roi`、`DELETE /api/roi`；`/api/situations` 新增欄位，既有欄位不變
- 部署：`compose.yaml` 新增可寫入的 overrides 掛載與環境變數；`.env.example`、README 同步說明
- 依賴：`cascade-alert-architecture` 必須先封存，本 change 的 `web-dashboard` 修改才有對象；與 `containerize-services` 的 compose 變更可能衝突，後實作的一方要合併
- 重播：觀測紀錄存的是已經算好的訊號，ROI 變更不影響重播結果；README 註明 ROI 無法離線評估
