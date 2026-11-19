# Tasks

## 1. 依賴與設定

- [x] 1.1 CMake 以 FetchContent 加入固定版本的 yaml-cpp，並以 `find_package(CURL REQUIRED)` 連結 libcurl；README 的需求表補上 libcurl（`libcurl4-openssl-dev`）與首次 configure 會下載 yaml-cpp；乾淨的 `build/` 重新 configure 與 build 成功
- [x] 1.2 `Config` 新增 `BM_RULES_PATH`、`BM_VLM_URL`、`BM_VLM_MODEL`、`BM_RING_SECONDS`、`BM_EVENT_DIR`、`BM_EVENT_MAX_MB`、`BM_LOG_DIR` 的讀取與驗證（含 `BM_VLM_URL` 只接受 loopback 或私有網路位址），`tests/test_config.cpp` 涵蓋預設值與每個無效值的錯誤訊息；README 環境變數表同步更新；`ctest -R config` 通過

## 2. 人物數量與訊號

- [x] 2.1 `decodeBestDetection` 一併回傳達到門檻的人物數，`DetectionResult` 加上 `personCount`（D8）；`tests/test_yolo_decoder.cpp` 以手造多筆資料驗證數量與門檻，`tests/test_yolo_inference.cpp` 以 `bus.jpg` 驗證人數大於 1；`/ws/live` 輸出不變（`ctest -R live_protocol` 仍通過）
- [x] 2.2 實作 `SignalExtractor`：依 D1 的定義從 `DetectionResult` 與 activity score 產生 `SignalFrame`，偵測值超過 3 秒視為未知（D3）；`tests/test_signal_extractor.cpp` 驗證臉被遮住、沒有人物、純偵測模型、嬰兒床區域內外、`keypoint_motion` 的正規化與過期；README 新增「訊號」一節說明每個訊號的定義

## 3. 規則檔

- [x] 3.1 實作規則檔解析與驗證（D2）：必填欄位、預設值、時間字串、訊號名稱與比較方式、`id` 不重複、prompt 檔存在、`clip` 前段不超過 `BM_RING_SECONDS`、底線保護（`critical` 不可為 `llm` / `suppress`、`suppress.below` 只接受 `warning` / `critical`），錯誤訊息含檔案路徑與狀況 `id` 或行號；`tests/test_rules.cpp` 每條驗證規則各至少一個失敗案例，`ctest -R rules` 通過
- [x] 3.2 新增 `config/situations.yaml` 與 `config/prompts/*.md`：六個初始狀況（臉被蓋住 `critical`/`direct`、趴睡 `warning`/`llm`、醒來持續活動 `warning`/`llm`、不在床上 `warning`/`llm`、有大人進入 `info`/`suppress`、靠近床邊 `warning`/`llm`），每個 prompt 要求只回覆 `{"alert", "cause"}`；在 `test_rules` 中載入此檔驗證通過；CMake 在 build 後把 `config/` 複製到執行檔旁，README 新增「狀況規則」一節說明格式與調整流程（改檔 → 重播 → 影子模式 → 正式）
- [x] 3.3 更新預設規則與 prompt：`face_covered` 加 `grace: 1s`、`awake_active` 加 `grace: 2s`，新增 `airway_covered` 巡檢（`critical`、`every: 30s`、`frames: 2`、`confirm: 2`、無 `while`）與 `prompts/airway_covered.md`；所有 prompt 改為只描述判斷標準，不列出可照抄的原因句子；`test_rules` 載入預設檔驗證通過；README 的狀況規則一節說明 `grace` 與 `patrols` 格式，訊號一節註明 `face_visible` 無法偵測口鼻被蓋住

## 4. 規則引擎

- [x] 4.1 實作 `RuleEngine`（D3）：AND 條件、`for` 持續時間、未知值使計時歸零、觸發與冷卻、`enabled`、`shadow` 標記，觸發帶狀況 `id`、時間、訊號值、版本與雜湊；`tests/test_rule_engine.cpp` 以合成時間序列涵蓋 spec 中「條件語意」與「觸發與冷卻」的所有 scenario，`ctest -R rule_engine` 通過
- [x] 4.2 實作 `RulesStore` 熱重載（D4）：每 2 秒檢查規則檔與 prompt 的 mtime / 大小，驗證失敗保留舊規則並記錄錯誤，替換時保留同 `id` 狀況的計時；啟動與重載時對影子模式或停用的 `critical` 狀況印出警告；測試以暫存目錄改寫檔案驗證成功替換、失敗保留、修正後恢復，`ctest -R rules_store` 通過
- [x] 4.3 實作條件的 `grace`（D10）：解析與驗證（非負時間）、`RuleEngine` 的中斷計時與成立中閃爍不重複觸發；`tests/test_rule_engine.cpp` 涵蓋 spec 新增的三個 scenario；以 2026-11-18 的觀測紀錄重播，確認加上 `grace` 前後的觸發次數差異合理；`ctest -R "rule"` 通過

## 5. 觀測紀錄與離線重播

- [x] 5.1 實作觀測紀錄（D7）：每個 `SignalFrame` 寫一行 JSONL，依日期分檔、保留 14 天；`AnalysisThread` 改為把每張分析影格的結果送給 `SignalExtractor` → `RuleEngine` 與紀錄；單元測試驗證寫出的行可讀回成相同的 `SignalFrame`
- [x] 5.2 新增 `bm_replay_rules` 命令列工具：輸入規則檔與觀測紀錄檔，以紀錄的擷取時間執行同一個 `RuleEngine`，輸出每次觸發；測試以固定的小型紀錄檔與規則檔比對預期輸出，連續執行兩次結果相同；README 說明用法

## 6. 環形緩衝與事件片段

- [x] 6.1 實作主碼流環形緩衝（依 `tCapture` 保留 `BM_RING_SECONDS` 秒，`shared_ptr<const RawFrame>` 共用不複製），由 `CaptureThread` 寫入；測試驗證超時捨棄、依時間範圍取出、範圍超出緩衝時從最早開始
- [x] 6.2 實作事件片段寫出（D6）：觸發後等到 `t + post` 再寫出目錄與 `meta.json`，總大小超過 `BM_EVENT_MAX_MB` 時從最舊刪除；測試以假影格驗證片段內容、排序、攝影機中斷時不補影格與刪除順序

## 7. VLM 判定

- [ ] 7.1 Spike：在本機以 `llama-server` 或 Ollama 試跑 2–3 個 3B–7B 的 VLM，量測單次判定延遲、GPU 記憶體、與 BabyMonitor 同時執行時 YOLO 的推論時間變化，選定預設模型；把結果與選擇寫進本 change 的 design.md「實測結果」段落，並在 README 說明如何啟動 VLM 服務
- [x] 7.2 實作 `VlmJudge`（D5）：worker 執行緒、佇列上限 4、取樣最多 6 張並縮至長邊 640、OpenAI 相容的 chat 請求、JSON 回覆解析、逾時、`on_timeout` 預設 `alert`、`GET /v1/models` 可用性檢查；測試以本機假 HTTP 伺服器驗證正常判定、逾時、連線失敗、回覆格式錯誤、佇列滿，`ctest -R vlm_judge` 通過
- [x] 7.3 實作 VLM 巡檢（D11）：規則檔 `patrols` 的解析與驗證（`id` 與狀況不重複、必填欄位、`while` 同 `when` 格式）；`PatrolScheduler` 執行緒依 `every` / `while` 從環形緩衝取影格；`VlmJudge` 巡檢優先佇列；`confirm` 連續確認與立即重新巡檢；觸發交給 `AlertManager` 立即送出並寫出片段；巡檢紀錄 `patrols-YYYYMMDD.jsonl`；停止與恢復的系統警報（不可壓制）；未設定 VLM 時啟動警告；`bm_replay_rules` 略過巡檢並註明；熱重載時保留同 `id` 巡檢的計時；測試以假 VLM 伺服器與假時鐘涵蓋 spec 中「VLM 定期巡檢」與「巡檢停止時要讓人知道」的所有 scenario

## 8. 警報管理

- [x] 8.1 實作 `AlertManager`：依路由產生警報、`suppress` 壓制（含 `linger`，`critical` 不壓制）、影子模式只記錄、`direct` 路由的 VLM 補充說明附加在既有警報上而不撤回；`tests/test_alert_manager.cpp` 涵蓋 `alerting` spec 與 `situation-judgement` spec 中「direct 路由的補充說明」的所有 scenario
- [x] 8.2 實作警報紀錄 JSONL（D7），啟動時讀回最近 100 筆；測試驗證重啟（重新建立物件）後查詢結果相同

## 9. API 與監視頁面

- [x] 9.1 新增 `GET /api/situations`、`GET /api/alerts`、`GET /api/events/<id>`（含讀取片段中的單張 JPEG，拒絕含 `..` 的 id），`/api/status` 新增 `rulesVersion`、`rulesError`、`lastAlertEpochMs`；以 curl 對實機服務確認回應格式與 404 / 400 的處理，README 的 HTTP 介面表同步更新
- [x] 9.2 `frontend/` 新增狀況與警報區塊（D9）：每 2 秒輪詢、新警報 3 秒內出現、`critical` 醒目樣式、未經 LLM 判定的標示、規則版本與載入錯誤、影子模式（`critical` 特別醒目）、觸發紀錄與 LLM 原因、片段播放；沒有任何編輯功能；以瀏覽器手動確認每項並確認 console 沒有錯誤
- [x] 9.3 `/api/situations` 新增 `patrols[]`（`id`、嚴重程度、間隔、影子模式、最後成功時間、最近結果與原因、`stalled`）；監視頁面顯示巡檢狀態，巡檢停止時以與 `critical` 影子模式同等醒目的方式標示；README 的 HTTP 介面表同步更新；以瀏覽器確認並確認 console 沒有錯誤

## 10. 整合驗證

- [ ] 10.1 不啟動 VLM，在實機以手遮臉（身體保持在畫面中）超過 10 秒：`face_covered` 警報在條件成立後 1 秒內出現在頁面，`/api/alerts` 有對應紀錄與事件片段；離開畫面讓 `not_in_bed` 觸發，確認依 `on_timeout` 送出並標示未經 LLM 判定
- [ ] 10.2 啟動選定的 VLM 後重複 10.1，並做一次會觸發 `awake_active` 的持續動作：確認判定結果與原因出現在頁面、即時影像與 `detectHz` 在判定期間不受影響；第二人進入畫面時 `awake_active` 被壓制但 `face_covered` 照常送出
- [x] 10.3 執行中把規則檔改壞再修好、以及把某狀況改為 `shadow: true`，確認服務不中斷、頁面顯示錯誤與恢復、影子狀況只記錄不提醒；以當次觀測紀錄執行 `bm_replay_rules`，觸發結果與即時紀錄一致
- [ ] 10.5 啟動 VLM 後在實機以毛巾或衣服蓋住口鼻（眼睛露出）：`airway_covered` 在約 35 秒內送出 `critical` 警報並附原因與片段；不蓋時連續 10 分鐘沒有巡檢警報（巡檢紀錄約 20 筆）；停掉 llama-server 約 90 秒後頁面出現巡檢停止，重新啟動後恢復
- [x] 10.4 `ctest` 全部通過，執行 `openspec validate cascade-alert-architecture --strict` 並修正所有問題

## Workflow follow-up

- 本 change 範圍較大，apply 時可依群組分批進行；第 1–6 群組不依賴 VLM，可先完成並上線第一段
- 躺床蓋被子實驗另開 change，其結果用來校正 `config/situations.yaml` 的初始門檻與 `prone_likely` 的路由
- `redesign-vision-architecture` 已在本次同步修訂（D7、架構圖、分期、capability 清單）
