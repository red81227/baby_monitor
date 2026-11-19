# Tasks

## 1. 設定與 ROI 檔載入

- [x] 1.1 `Config` 新增 `BM_OVERRIDES_DIR`（預設 `overrides`）；`tests/test_config.cpp` 涵蓋預設值與自訂路徑；README 環境變數表補上說明（含容器部署必須可寫入）；`ctest -R config` 通過
- [x] 1.2 把 `bed_roi` 的驗證抽成共用函式，`loadRules` 新增 `overridesPath` 參數（D1、D2）：ROI 檔存在時覆蓋 `params.bedRoi`，記錄 `fileBedRoi` 與 `bedRoiSource`，ROI 檔路徑一律加入 `watchedFiles`，存在時內容納入 `hash`；ROI 檔無效時的錯誤訊息含檔案路徑。`tests/test_rules.cpp` 涵蓋：只有規則檔、只有 ROI 檔、兩者都有（ROI 檔優先）、ROI 檔無效、未知欄位忽略、ROI 改變但 `version` 不變而 `hash` 改變；`ctest -R "^rules$"` 通過
- [x] 1.3 `RulesStore` 帶入 `overridesPath`，新增 `reloadNow()`，並以 `reloadMu_` 序列化 `checkNow()` 與 `reloadNow()`（D3）；`tests/test_rules_store.cpp` 驗證：建立、修改、刪除 ROI 檔都會觸發重新載入；ROI 檔損壞時保留舊規則並回報錯誤；`reloadNow()` 不需等待 stamp 變化；`ctest -R rules_store` 通過
- [x] 1.4 `main.cpp` 把 `Config::overridesDir` 傳進 `RulesStore`；啟動時 ROI 檔無效會以非 0 結束碼結束。手動驗證：放一個無效的 `overrides/bed_roi.json` 啟動，訊息含路徑且結束碼不為 0

## 2. ROI 寫入 API

- [x] 2.1 `AlertManager` 新增 `Delivery::Logged`（`"logged"`）與 `recordSystem(...)`（D5）：寫入警報紀錄並列在 `recentJson()`，不更新 `lastAlertEpochMs`；`tests/test_alert_manager.cpp` 驗證 `roi_changed` 紀錄的欄位、送出狀態，以及 `lastAlertEpochMs` 不變；`ctest -R alert_manager` 通過
- [x] 2.2 實作不依賴 Crow 的 `RoiService`（D3）：`set(roi, ifMatch, remoteIp)` / `reset(ifMatch, remoteIp)`，包含 `writeMu_`、載入失敗時拒絕、`If-Match` 比對、最小 0.05 檢查、拒絕空值、四捨五入到小數 3 位、tmp + fsync + rename 原子寫入、`reloadNow()` 與修改紀錄；回傳結果碼（200/400/409/500）與目前的 ROI、來源、雜湊值。新增 `tests/test_roi_service.cpp`，涵蓋 `roi-configuration` spec 中除了跨站防護以外的每個 scenario，以及目錄唯讀時回 500 且規則不變；`ctest -R roi_service` 通過
- [x] 2.3 新增 `RoiController`（Crow）註冊 `PUT /api/roi`、`DELETE /api/roi`（D4）：Content-Type 必須是 JSON（否則 415）、`Origin` 與 `Host` 不同時回 403、body 大於 4 KiB 回 413，其餘交給 `RoiService`；跨站檢查抽成純函式，並在 `tests/test_roi_service.cpp` 中測試（同源、跨源、沒有 Origin、不同 port）。手動以 curl 驗證：正確請求 200、錯誤的 `Origin` 403、form 編碼 415、舊雜湊值 409
- [x] 2.4 `/api/situations` 新增 `bedRoi`、`bedRoiSource`、`fileBedRoi`、`bedEdgeMargin`；`SituationController` 的註解改成「唯讀；ROI 寫入在 RoiController」。以 curl 驗證三種來源的回應欄位，以及既有欄位不變
- [x] 2.5 README 新增「ROI 設定」一節：來源與優先順序、`PUT`/`DELETE /api/roi` 的格式與錯誤碼、還原方式、不能從網頁清除、ROI 不影響重播、目前沒有認證只擋跨站請求；README 中的 curl 範例實際執行過，結果與文件相符

## 3. 前端重構（ES modules）

- [x] 3.1 把 `app.js`、`situations.js` 拆成 D6 列出的模組（`common`、`store`、`live`、`overlay`、`situations`、`main`），`index.html` 改用 `<script type="module">`，`StaticController` 確認 `.js` 以正確的 MIME type 提供。行為不變的驗證：即時影像、人框、骨架、狀況表、巡檢、觸發紀錄、事件片段播放、警報橫幅都與重構前相同；瀏覽器 console 沒有錯誤

## 4. ROI 顯示與編輯

- [x] 4.1 `overlay.js` 依 store 中的 `bedRoi` 畫出 ROI（實線）與 `bed_edge_margin` 內縮範圍（虛線）；頁面顯示 ROI 來源，未設定時提示 `near_bed_side` 不會觸發。驗證：規則檔設定 `bed_roi` 後，矩形對齊影像中的比例位置；改規則檔存檔後，幾秒內頁面更新，不需要重新整理
- [x] 4.2 實作 `roi-editor.js`（D7）：編輯模式按鈕、Pointer Events + `setPointerCapture`、建立 / 移動 / 八個控制點調整大小、夾在畫面內且寬高 ≥ 5%、ROI 外調暗、只在編輯模式時設定 `touch-action: none`、`Esc` 取消、`Enter` 儲存。驗證：桌機用滑鼠完成建立、移動、調整、取消；以 Chrome DevTools 的手機模擬（390px）拖拉控制點時頁面不捲動；離開編輯模式後手機頁面可以正常捲動
- [x] 4.3 儲存與還原：`PUT`/`DELETE /api/roi` 帶 `If-Match`；成功時用回應更新 store；409 時顯示「已被其他人修改」並套用目前值；403/415/500 顯示錯誤；「還原為規則檔」只在來源為網頁設定時顯示，確認對話框說明還原後的值。驗證：開兩個分頁，先在 A 儲存，B 以舊值儲存會看到 409 提示；還原後來源變回規則檔；觸發紀錄出現 `roi_changed` 且沒有跳出警報橫幅

## 5. 監視頁面優化

- [x] 5.1 狀態文字中文化（連線中、後端無法連線、攝影機中斷、有移動 / 沒有移動、即時影像中斷、seq/偵測頻率/顯示影格率）。驗證：逐一觸發這些狀態（停止服務、拔攝影機），文字都是中文
- [x] 5.2 響應式版面（D10）：≥ 1100px 兩欄、影像 sticky；窄螢幕單欄，表格改成卡片式。驗證：在 1440px 與 390px 寬度下截圖，兩者都沒有水平捲動
- [x] 5.3 只看影像模式（D10）：警報橫幅移到 `.viewer` 內，支援 Fullscreen API，不支援時改用 CSS 填滿視窗。驗證：進入此模式後以影子模式以外的狀況觸發警報，橫幅顯示在影像上；`Esc` 或按鈕可以離開
- [x] 5.4 疊圖開關：人框、骨架、ROI 各一個開關，記在 localStorage（讀寫包 try/catch）。驗證：關閉骨架後重新整理，仍然不畫骨架；在無痕視窗停用儲存時頁面照常運作
- [x] 5.5 警報提示音（D8）：「啟用提示音」按鈕、Web Audio 產生的 critical / 其他兩種聲音、只對新出現的 `sent` 警報響、重新載入後顯示「點一下恢復提示音」。驗證：頁面開著時觸發 critical 與 warning 各一次，聲音不同；`roi_changed` 不會響；剛開頁面時既有的警報不會響
- [x] 5.6 觸發紀錄依嚴重程度篩選（全部 / critical / warning / info）。驗證：選擇 critical 後只列出 critical 紀錄，篩選在輪詢更新後仍然保持
- [x] 5.7 背景分頁（D9）：hidden 時關閉 WebSocket、停止 status/situations 輪詢，alerts 繼續；visible 時立即恢復。驗證：DevTools Network 中切到其他分頁後沒有影像訊息與 status 請求，但仍有 alerts 請求；切回後幾秒內恢復畫面

## 6. 部署

- [x] 6.1 `compose.yaml` 為 `monitor` 新增 `BM_OVERRIDES_DIR` 與 `${BM_DATA_DIR:-./data}/overrides` 掛載（D11）；`.env.example` 與 README 的部署步驟改成 `mkdir -p data/logs data/events data/overrides`，疑難排解補上 overrides 目錄屬於 root 的情況。驗證：`docker compose config` 通過；在容器中從網頁設定 ROI 後，主機上的 `data/overrides/bed_roi.json` 屬於 `BM_UID`；`docker compose up -d --force-recreate monitor` 後 ROI 仍然生效

## 7. 整合驗證

- [x] 7.1 端對端：在實際攝影機畫面上框選嬰兒床並儲存。確認 `/api/situations` 的 `hash` 改變而 `version` 不變；人物移到 ROI 邊緣 5 秒後 `near_bed_side` 觸發，警報紀錄的 `rulesHash` 是新的雜湊值；按還原後回到規則檔的 ROI（或未設定）；`ctest` 全部通過

## Workflow follow-up

- 本 change 的 `web-dashboard` MODIFIED 依賴 `cascade-alert-architecture` 先封存；`compose.yaml` 與 README 的修改也要和 `containerize-services` 合併。封存順序：cascade → containerize → 本 change
- 認證與權限另開 change，屆時 `RoiController` 是唯一需要保護的寫入端點
