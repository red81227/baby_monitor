# Design

## Context

動機見 proposal.md，行為要求見 `specs/`。和設計直接相關的現況：

- `loadRules(path, ringSeconds, error)` 解析規則檔，`bed_roi` 進到 `rules.params.bedRoi`；`rules.watchedFiles` 列出規則檔與 prompt 檔，`rules.hash` 是這些檔案內容的 SHA-256 前 12 碼。
- `RulesStore` 每 2 秒比對 `watchedFiles` 的 mtime 與 size，有變化就呼叫 `loadRules`。`Stamp` 能表示「檔案不存在」（mtime = -1），所以監看一個尚未建立的檔案不需要額外處理。`checkNow()` 目前沒有序列化，只由背景執行緒呼叫。
- `SituationMonitor::bedRoi()` 從目前規則取 ROI，`ActivityScorer` 在 ROI 改變時重建基準（那一格分數為 0），`SignalExtractor` 依 ROI 算 `near_bed_edge`。
- HTTP 用 Crow；`SituationController` 的註解寫明「這裡不提供任何寫入」，目前所有 API 都是 GET。
- 觀測紀錄存的是 `SignalExtractor` 已經算好的訊號，重播工具不會重算 `near_bed_edge`，所以 ROI 與重播無關。
- 前端沒有建置步驟：`app.js`（即時影像、canvas 疊圖、`/api/status` 每秒）與 `situations.js`（`/api/situations`、`/api/alerts` 每 2 秒）兩支一般 script，各自定義工具函式。
- Compose 的 `./config` 以 `:ro` 掛載；資料目錄以 `logs/`、`events/` 兩個子目錄分別掛載，必須事先 `mkdir -p`，否則 Docker 會以 root 建立。
- 頁面在區網以 http 開啟，**不是安全環境**：`Notification`、`navigator.clipboard` 等 API 不能用；Fullscreen API 和 Web Audio 不受這個限制。

## Goals / Non-Goals

**Goals:**
- 網頁設定的 ROI 和手改規則檔走同一條生效路徑：同一套驗證、熱重載、錯誤處理與雜湊值
- 儲存後在 HTTP 回應前就已生效，頁面不用等下一次輪詢才知道結果
- 前端維持沒有建置步驟

**Non-Goals:**
- 通用的「網頁覆寫任意規則」機制（ROI 檔的格式只認 `bed_roi`）
- 防禦同網段的惡意使用者或 DNS rebinding（要等認證）

## Decisions

### D1. ROI 檔在 `loadRules` 合併，不另建 store

`loadRules` 新增參數 `overridesPath`（可省略）。解析完規則檔後，若 ROI 檔存在就讀取並驗證，用它取代 `params.bedRoi`。同時記下 `fileBedRoi`（規則檔的值）與 `bedRoiSource`（`override` / `rules` / `none`）。ROI 檔路徑**一律**加入 `watchedFiles`，存在時其內容也納入 `hash`。

```
 situations.yaml --+
 prompts/*.md -----+--> loadRules --> Rules { params.bedRoi, fileBedRoi, bedRoiSource, hash }
 overrides/bed_roi.json --+              (ROI 檔不存在：stamp = -1，被建立時觸發重新載入)
```

- 好處：熱重載、載入失敗時沿用舊規則、啟動時檢查、雜湊值、`/api/situations` 的錯誤顯示全部沿用，不需要另寫。警報紀錄的 `rulesHash` 自然就能分辨 ROI 是否改過。
- 替代方案：另寫 `RoiStore`，在 `SituationMonitor::bedRoi()` 決定優先順序。缺點是會有兩個版本來源，警報紀錄分辨不出 ROI 改過，錯誤處理和監看也要重寫一次。不採用。
- `version` 仍只取規則檔，ROI 改變時 `version` 不變、`hash` 變。頁面顯示 `version 2（a1b2c3…）· ROI：網頁設定`。
- 重播工具不傳 `overridesPath`：觀測紀錄裡的訊號已含當時的 ROI 效果，傳了也不影響結果。

### D2. ROI 檔格式與位置

`${BM_OVERRIDES_DIR}/bed_roi.json`：

```json
{ "bed_roi": [0.123, 0.150, 0.600, 0.750], "updatedAtEpochMs": 1795000000000, "updatedFrom": "192.168.1.23" }
```

- 程式寫、程式讀，用 JSON；yaml-cpp 可以直接解析 JSON（JSON 是 YAML 的子集），不需要新的依賴。
- 只有 `bed_roi` 有意義，其餘欄位只是資訊，未知欄位忽略（方便人工查看）。驗證規則與規則檔的 `bed_roi` 相同（共用同一個函式），再加上網頁設定要求的「寬、高 ≥ 0.05」。這個下限只在寫入端點檢查，因為人工修改的 ROI 檔不應該因此讓服務起不來。
- 數值四捨五入到小數 3 位（1080p 時約 2 px），讓雜湊值不會因為浮點雜訊而改變。

### D3. 寫入流程與序列化

```
 PUT /api/roi   (If-Match: <hash>, body {"roi":[x,y,w,h]})
   |
   +-- Content-Type / Origin / 大小檢查 ----------------- 415 / 403 / 413
   +-- 取得 writeMu_ (同時間只處理一個寫入)
   +-- rules.status().lastError ? --------------------- 409 規則載入失敗
   +-- If-Match != 目前 hash ? ------------------------ 409 + 目前 ROI/hash
   +-- 驗證 roi ---------------------------------------- 400
   +-- 寫 bed_roi.json.tmp -> fsync -> rename -> fsync 目錄 -- 500 (無法寫入)
   +-- RulesStore::reloadNow()  (不比對 stamp，直接重新載入)
   |      失敗 (極少：剛好有人同時改壞規則檔) --------- 409 + 錯誤；ROI 檔已寫入，規則修好後生效
   +-- AlertManager::recordSystem(roi_changed, 舊值 -> 新值, 來源 IP)
   +-- 200 {roi, source, hash}

 DELETE /api/roi (If-Match: <hash>)  -- 流程相同，寫檔改成 unlink (不存在視為成功)
```

- 以 `If-Match` 標頭帶雜湊值，`PUT` 和 `DELETE` 都適用（不需要 DELETE body）。雜湊值本身就代表「讀到的規則 + ROI」的狀態，不需要另外維護 ROI 的版本號。
- `RulesStore` 新增 `reloadMu_`，讓 `checkNow()` 與 `reloadNow()` 序列化。否則背景輪詢和 API 可能同時載入，造成 stamp 被舊結果覆蓋、多載入一次。
- 在回應前同步重新載入，所以「回應成功」等於「已生效」，頁面不需要猜。

### D4. 跨站請求防護（認證之前）

- 只接受 `Content-Type: application/json`：跨站的表單和 `no-cors` fetch 只能送簡單請求；送 JSON 或 `PUT`/`DELETE` 都需要 CORS preflight。伺服器不回 CORS 標頭，瀏覽器就會擋下。
- `Origin` 存在時，其 host:port 必須等於 `Host` 標頭；沒有 `Origin`（curl、腳本）時放行，方便維運。
- body 上限 4 KiB。
- 不防同網段的人直接用 curl 修改，也不防 DNS rebinding。前者靠修改紀錄（D5）留下痕跡，兩者都等認證處理。
- `SituationController` 維持唯讀；寫入路由放在新的 `RoiController`，讓「哪些端點會寫入」一目了然，之後加認證時也只需要包住這一個。

### D5. 修改紀錄用新的送出狀態 `logged`

`AlertManager` 新增 `recordSystem(...)`，產生一筆 `situationId = "roi_changed"`、`route = system`、`severity = info` 的紀錄，原因欄位寫「ROI 由網頁設定：[舊]（規則檔）→ [新]（網頁設定），來源 192.168.1.23」。送出狀態用新的 `Delivery::Logged`（`"logged"`）：

- 不是 `sent`，所以不會跳出警報橫幅、不會播提示音，也不更新 `lastAlertEpochMs`。改 ROI 的人不需要被自己的操作提醒。
- 照常寫入警報紀錄檔並列在 `/api/alerts`，觸發紀錄區看得到。
- 替代方案：用 `sent` + `info`。缺點是每次調整都會跳出橫幅，而且會汙染 `lastAlertEpochMs`。不採用。

### D6. 前端改為 ES modules，ROI 狀態單向流動

改用 `<script type="module">`，仍然不需要建置步驟：

```
 common.js     el(), fmtTime(), badge(), prefs (localStorage 包 try/catch)
 store.js      極小的狀態 + 訂閱：situations、alerts、visibility
 live.js       WebSocket、影格緩衝、render()           (原 app.js)
 overlay.js    畫框 / 骨架 / ROI / 編輯中的草稿
 roi-editor.js pointer 事件 -> 草稿 -> PUT/DELETE
 situations.js 規則、巡檢、紀錄、篩選、提示音          (原 situations.js)
 main.js       組裝、輪詢、visibilitychange
```

`situations.js` 輪詢到的 `bedRoi` 放進 store，`overlay.js` 訂閱後畫出來。編輯器只持有草稿，儲存成功後用回應裡的 ROI 更新 store。這樣「生效的 ROI」只有一個來源。

### D7. ROI 編輯互動

座標一律以**畫面比例**保存，只在繪製和處理 pointer 事件時換算：

```
 pointer (clientX, clientY)
   --> canvas 內 CSS px      = client - rect.left/top
   --> 比例                  = cssPx / rect.width|height    (canvas 以等比例縮放顯示)
 比例 --> 繪製用 canvas px    = 比例 * canvas.width|height   (canvas.width = 主碼流寬)
```

```
          pointerdown                pointermove                 pointerup
 [idle] ------------> 命中測試 -----> [creating|moving|resizing] -----> [draft]
                       |  控制點 (角/邊，半徑 12 CSS px) -> resizing      |  儲存 -> PUT
                       |  矩形內部                    -> moving         |  取消 -> idle
                       |  矩形外 (或沒有 ROI)          -> creating        v
                                                                      [saving] -> 200: idle / 409: 顯示目前值
```

- 使用 Pointer Events + `setPointerCapture`，滑鼠和觸控共用同一套程式。只在編輯模式時對 canvas 設定 `touch-action: none`，平常手機仍可捲動頁面。
- 夾在畫面內、寬高 ≥ 0.05；在 pointermove 時直接 `render()` 草稿，不等下一張影格。
- 繪製：生效的 ROI 用實線，`bed_edge_margin` 的內縮範圍用虛線；編輯時把 ROI 外的區域調暗，畫出 8 個控制點。
- 鍵盤：`Esc` 取消、`Enter` 儲存（桌機方便用）。

### D8. 提示音用 Web Audio 產生，不用系統通知

- `Notification` 在非安全環境（區網 http）無法使用，因此不採用；之後有 HTTPS 再加。
- 用 `AudioContext` + oscillator 產生提示音，不需要音檔：`critical` 用高低交替、重複 3 次，其他等級短響一次。
- 瀏覽器要求使用者操作後才能播放聲音，所以頁面顯示「啟用提示音」按鈕。偏好記在 localStorage，但每次載入頁面仍需要點一下才能恢復 `AudioContext`；這時按鈕改顯示「點一下恢復提示音」。
- 只對「新出現」的 `sent` 警報響：以已看過的警報 id 判斷，頁面剛載入時的既有警報不響。

### D9. 背景分頁

`visibilitychange` 為 hidden 時：關閉 WebSocket（伺服器在沒有觀看者時本來就會停止直播處理），停止 `/api/status` 與 `/api/situations` 輪詢；`/api/alerts` 繼續輪詢，維持提示音與標題提醒。回到 visible 時重新連線並立即輪詢一次。

### D10. 版面與只看影像模式

- 寬度 ≥ 1100px 時，用 CSS grid 分成兩欄：左欄影像（`position: sticky`），右欄狀況與紀錄；窄螢幕時單欄，影像在最上方。表格在窄螢幕時改成卡片式，避免水平捲動。
- 只看影像模式：對 `.viewer`（含 canvas 與警報橫幅）呼叫 Fullscreen API；不支援時（iPhone Safari 不支援元素全螢幕）改用 CSS 讓 `.viewer` 以 fixed 填滿整個視窗。警報橫幅移到 `.viewer` 內，兩種模式下都看得到。

### D11. 部署

- `compose.yaml` 的 `monitor` 新增 `BM_OVERRIDES_DIR: /var/lib/babymonitor/overrides`，以及 `"${BM_DATA_DIR:-./data}/overrides:/var/lib/babymonitor/overrides"` 掛載。
- README 的 `mkdir -p data/logs data/events` 改成也建立 `data/overrides`，並在疑難排解補上 overrides 目錄屬於 root 的情況。
- 非容器執行時預設為工作目錄下的 `overrides/`，第一次寫入時建立。

## Risks / Trade-offs

- [同網段任何人都能改 ROI，改錯會讓 `near_bed_side` 在錯誤的區域判斷] → 修改紀錄含來源 IP、頁面明確標示來源與還原按鈕、不能清除 ROI；正式的解法是之後的認證 change
- [ROI 改變時 `ActivityScorer` 重建基準，那一格 activity 為 0，可能讓 `awake_active` 之類的 `for` 計時中斷] → 有 `grace` 吸收單格中斷；修改 ROI 本來就是低頻操作，在 design 中記錄即可
- [背景分頁的計時器會被瀏覽器節流，Chrome 在背景超過 5 分鐘後可能一分鐘才輪詢一次，提示音因此延遲] → README 建議床邊裝置使用只看影像模式並保持在前景；真正可靠的通知管道仍是之後的通知 change
- [`version` 不變但 `hash` 改變，習慣只看 version 的人可能沒發現 ROI 改過] → 頁面在版本旁顯示 ROI 來源；修改紀錄列在觸發紀錄
- [與 `containerize-services` 同時修改 `compose.yaml`、README] → 該 change 先完成；本 change 實作時再合併
- [`cascade-alert-architecture` 尚未封存，本 change 的 `web-dashboard` MODIFIED 在那之前無法封存] → 封存順序：cascade → containerize → 本 change

## Migration Plan

1. 先封存 `cascade-alert-architecture`（與 `containerize-services`）
2. 部署：`mkdir -p data/overrides`，更新 `compose.yaml` 與 `.env.example`，`docker compose up -d --build`
3. 沒有 ROI 檔時行為與現在完全相同，既有部署不受影響
4. 回復：刪除 `data/overrides/bed_roi.json`（或在頁面按還原）就回到規則檔的 ROI；退回舊版程式時 ROI 檔會被忽略
