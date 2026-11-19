# Design

## Context

動機見 proposal.md 的 Why，行為要求見 `specs/`。和設計直接相關的現況：

- `AnalysisThread` 每張分析影格算移動分數，依 `DetectScheduler` 以 1–10 Hz 執行 YOLO26n-pose，產生 `DetectionResult`（`seq`、`tCapture`、信心值最高的一個人物框與 17 個關鍵點、`motion`），透過回呼送到 `LiveHub`。
- `YoloDecoder` 目前只取信心值最高的一筆；沒有人數。
- `DetectScheduler` 的時間由呼叫端傳入，但 `AnalysisThread::run()` 直接用 `steady_clock::now()`。
- 沒有環形緩衝、沒有持久紀錄、沒有 YAML 解析器；HTTP 只有 Crow（伺服器）。系統已裝有 `libcurl4-openssl-dev`。
- 硬體：i7-12700H + RTX 3070 Ti Laptop（8 GB），YOLO26n-pose 在 CUDA 上只占很小的 GPU 資源。
- 網頁沒有認證，只限本機網路使用；本設計維持這個限制。

## Goals / Non-Goals

**Goals:**
- 第一段（YOLO + 規則）便宜、一直在跑，追求不漏觸發；第二段（VLM）只在觸發時執行，負責減少誤報與說明原因
- 狀況的條件、門檻、路由、prompt 上線後只改檔案就能調整，改錯不會停止監看
- 同一套規則判斷邏輯同時用在即時服務與離線重播，結果一致
- VLM 失效、變慢或給出錯誤判斷時，第一段偵測得到的 `critical` 狀況（如 `face_covered`）仍會即時提醒；只能由 VLM 巡檢偵測的狀況（口鼻被蓋住），在 VLM 失效時以「巡檢停止」系統警報讓使用者知道，不會默默失效

**Non-Goals:**
- 通用的運算式語言或 OR / NOT 組合（需要時以多個狀況表達）
- 在網頁上修改規則
- 嬰兒專用模型、VLM 微調
- 準確率保證：門檻在有真實嬰兒影像前只是初始值

## 整體架構

```
 CaptureThread --> LatestSlot --> AnalysisThread (既有)
      |                              |  DetectionResult (+ personCount)
      |                              |  activity score
      v                              v
 +------------+            +--------------------+       +------------------+
 | RingBuffer |            | SignalExtractor    |------>| ObservationLog   |
 | 主碼流 JPEG |            |  -> SignalFrame    |       | (JSONL, 每日一檔) |
 | 依 tCapture |            +---------+----------+       +---------+--------+
 +-----+------+                      |                            |
       |                             v                            | 離線
       |                  +---------------------+                  v
       |                  | RuleEngine          |      bm_replay_rules (CLI)
       |                  |  (純邏輯, 時間由輸入 |      同一個 RuleEngine
       |                  |   的 tCapture 決定)  |
       |                  +----------+----------+
       |                             | Trigger (situation, t, signals, version)
       |       片段 [t-pre, t+post]   v
       +---------------------> +-------------------+   llm 路由   +-------------+
                               | AlertManager       |------------>| VlmJudge    |
                               |  路由 / 冷卻 / 壓制 |<------------| (worker 執行緒, |
                               |  影子 / 底線保護    |   判定結果   |  libcurl)    |
                               +---------+---------+             +------+------+
                                         |                              |
                                         v                              v
                               AlertLog (JSONL)               本機 VLM 服務 (HTTP)
                               /api/alerts, /api/situations   llama-server / Ollama 等
                               監視頁面 (輪詢, 唯讀)

 PatrolScheduler (D11): 每隔 every 從 RingBuffer 取最近影格 --> VlmJudge (優先) --> AlertManager
                        連續失敗 / 太久沒有成功 --> 「巡檢停止」系統警報；每次結果寫 PatrolLog (JSONL)

 RulesStore: 載入 / 驗證 / 每 2 秒檢查檔案 mtime / 原子替換 shared_ptr<const Rules>
```

## Decisions

### D1. 訊號在 C++ 固定，規則在 YAML

訊號（spec `situation-rules` 的「訊號」）由 `SignalExtractor` 從 `DetectionResult` 與 activity score 算出，形成一個 `SignalFrame`（每個訊號是 `optional<double|bool>`，未知即 `nullopt`）。規則只能引用這些訊號名稱。

主要訊號的定義（數值門檻來自規則檔的 `keypoint_visibility`，預設 0.5）：

| 訊號 | 定義 |
|---|---|
| `face_visible` | 鼻、左眼、右眼任一可見度 >= 門檻。**無法偵測口鼻被蓋住**：COCO 17 點沒有嘴巴，被遮住的鼻子也會被推算出高可見度（見 D11 的實測），口鼻被蓋住由 VLM 巡檢負責 |
| `body_visible` | 雙肩或雙髖任一可見度 >= 門檻 |
| `prone_likely` | `body_visible` 且鼻、雙眼都低於門檻，且雙耳至少一個 >= 門檻（看得到後腦側面但看不到臉）。準確度未經驗證，因此初始規則的 `prone` 狀況使用 `warning` + `llm` 路由（見 Risks） |
| `keypoint_motion` | 與上一次偵測相比，兩次都可見的關鍵點平均位移，除以人物框對角線長度，再除以時間差（每秒） |
| `near_bed_edge` | 人物框中心落在嬰兒床區域外，或距區域邊界小於區域寬度的 `bed_edge_margin`（預設 0.1）；未設定區域時為未知 |
| `activity` | 既有 `ActivityScorer` 的分數；有嬰兒床區域時只計算區域內 |

替代方案：通用運算式語言（如 `face_visible == false && ...`）——需要自寫解析、型別檢查與除錯工具，且條件寫錯時較難驗證；固定格式條件已能表達草稿中的六個狀況。

### D2. 規則檔格式

```yaml
version: 3
keypoint_visibility: 0.5
bed_roi: [0.20, 0.15, 0.60, 0.75]     # 以畫面比例表示的 x, y, w, h；可省略
bed_edge_margin: 0.1
situations:
  - id: face_covered
    severity: critical
    route: direct
    when:
      - { signal: body_visible, is: true }
      - { signal: face_visible, is: false, for: 10s }
    cooldown: 60s
    clip: [-20s, 5s]
    prompt: prompts/face_covered.md     # direct 路由的 prompt 只產生補充說明
  - id: awake_active
    severity: warning
    route: llm
    when:
      - { signal: activity, above: 0.03, for: 30s }
    cooldown: 5m
    clip: [-20s, 5s]
    prompt: prompts/awake_active.md
    timeout: 20s
    on_timeout: alert
  - id: adult_present
    severity: info
    route: suppress
    when:
      - { signal: person_count, above: 1 }
    suppress: { below: critical, linger: 2m }
    cooldown: 0s
```

- prompt 路徑相對於規則檔所在目錄；預設規則檔與 prompt 放在 repo 的 `config/`，安裝時複製到執行環境
- 可省略的欄位：`for`（預設 0）、`clip`（預設 `[-20s, 5s]`）、`timeout`（預設 20s）、`on_timeout`（預設 `alert`）、`shadow`（預設 false）、`enabled`（預設 true）
- `suppress.below: critical` 表示壓制 `info` 與 `warning`；`below` 只接受 `warning` 或 `critical`，因此 `critical` 一定無法被壓制
- 解析使用 yaml-cpp（以 FetchContent 取得固定版本，與 Crow 相同方式）

替代方案：JSON（不必新增依賴，但不能寫註解，不適合需要手動調整的檔案）；TOML（巢狀的條件清單寫起來比 YAML 冗長）。

### D3. RuleEngine 是純邏輯，時間來自輸入

`RuleEngine::onSignals(const SignalFrame&) -> vector<Trigger>`，內部只用 `SignalFrame.tCapture` 計算 `for` 與冷卻，不讀系統時鐘。即時服務與 `bm_replay_rules` 共用同一個類別，因此重播結果與即時判斷一致，也能用合成的訊號序列寫單元測試。

`activity` 每張分析影格都會更新，關鍵點相關訊號只在偵測時更新；`SignalExtractor` 在兩者之間保留最近一次偵測的值（並以偵測時間判斷是否過期，超過 3 秒視為未知），讓每個 `SignalFrame` 都是完整的快照。

替代方案：在 `AnalysisThread` 直接用 `steady_clock::now()` 判斷——重播時無法重現。

### D4. 熱重載：輪詢 mtime + 原子替換

`RulesStore` 每 2 秒檢查規則檔與所有引用 prompt 檔的 mtime 與大小，有變化就重新解析與驗證；成功時以 `std::atomic<shared_ptr<const Rules>>`（或 mutex 保護的 `shared_ptr`）替換，`RuleEngine` 在下一個 `SignalFrame` 取用新規則。規則替換時，`id` 相同的狀況保留其 `for` 計時與冷卻狀態，新增的狀況從零開始，刪除的狀況直接移除。版本號取自檔案的 `version`，另外記錄內容的 SHA-256 前 12 碼，避免忘記改 `version` 時無法分辨。

替代方案：inotify（編輯器以「寫入暫存檔再改名」存檔時要另外處理，輪詢更簡單可靠）；只用 SIGHUP（容易忘記送訊號，改了卻沒生效）。

### D5. VLM 以 OpenAI 相容的 chat API 呼叫本機服務

`VlmJudge` 在獨立 worker 執行緒中，以 libcurl POST 到 `BM_VLM_URL` 的 `/v1/chat/completions`，影像以 base64 data URL 放在 `image_url` 中。llama.cpp 的 `llama-server`、Ollama、vLLM 都支援這個格式，因此換模型或換執行環境不需要改程式。

- 送出的影格：片段中平均取樣最多 6 張，縮到長邊 640（由主碼流 JPEG 以 reduced decode 解碼後重新編碼，只用於送給 VLM）
- prompt 組成：系統訊息要求只回覆 JSON `{"alert": bool, "cause": string}`；使用者訊息為規則中的 prompt 檔內容 + 觸發時的訊號值 + 影格
- 回覆解析：取第一個 JSON 物件，缺少 `alert` 即視為解析失敗
- 佇列上限 4 筆；滿時新觸發直接依 `on_timeout` 處理
- 啟動時與每 30 秒以 `GET /v1/models` 檢查可用性，結果放在 `/api/situations`

VLM 以獨立行程執行，不嵌入 BabyMonitor：VLM 當掉或載入很慢不會拖垮監看本身，也可以放到同一網路中的另一台機器。

替代方案：在程式內以 llama.cpp 函式庫直接推論——依賴重、與 YOLO 共用行程的記憶體與錯誤處理；雲端 API——違反「影像不離開本機網路」。

### D6. 事件片段格式

每個片段是 `BM_EVENT_DIR/<YYYYMMDD-HHMMSS>-<situation>-<n>/`，內含 `000001.jpg` 等原始 JPEG 與 `meta.json`（每張影格的 `seq`、`tCaptureUs`，狀況 `id`、觸發時間、規則版本）。JPEG 不重新編碼，直接由環形緩衝寫出；目錄形式方便直接用檔案總管或圖片檢視器查看，也方便日後作為評估資料。

替代方案：MJPEG AVI（需要容器寫入器，OpenCV `VideoWriter` 會重新編碼）；自訂二進位格式（無法直接查看）。

### D7. 觀測與警報紀錄用 JSONL

觀測紀錄每個 `SignalFrame` 一行，警報紀錄每個觸發 / 判定 / 警報狀態變化一行，都依日期分檔放在 `BM_LOG_DIR`。啟動時讀回最近的警報紀錄建立記憶體中的最近 100 筆清單，供 `/api/alerts` 使用。觀測紀錄的保留天數先固定為 14 天。

替代方案：SQLite——查詢能力較好，但現在只需要附加寫入與讀最近 N 筆；之後需要統計查詢時再導入（`redesign-vision-architecture` 原本的狀態紀錄 DB）。

### D8. 人物數量在解碼時一併計算

`decodeBestDetection` 改為同時回傳達到信心門檻的筆數（模型已內含 NMS，同一個人不會重複計數）。`DetectionResult` 加上 `personCount`；`/ws/live` 的 metadata 不變。

### D9. 監視頁面以輪詢取得規則與警報

頁面每 2 秒呼叫 `/api/situations` 與 `/api/alerts`，比對最新警報 id 決定是否跳出提醒。不另外在 `/ws/live` 加訊息類型，避免把警報與「有人在看直播」綁在一起。

### D10. 條件的容許中斷 `grace`

實測（2026-11-18，自己坐在攝影機前）發現逐格訊號會閃爍：揮手時 `activity` 在換方向的瞬間掉到 0 約 0.3 秒；手遮臉時 `face_visible` 偶爾被判定為看得到。`for` 要求連續成立，任何一格中斷就歸零，使 `face_covered`（10 秒）與 `awake_active`（30 秒）實際上幾乎無法成立；另外「不成立再成立」會被當成新的觸發，繞過冷卻時間（實測 1 秒內觸發 2 次）。

條件新增 `grace`：每個條件記錄 `since`（開始成立的時間）與 `gapSince`（目前這段中斷的開始時間）。

- 成立：`gapSince` 清除；`since` 未設定時設為現在
- 不成立或未知：`gapSince` 未設定時設為現在；`now - gapSince >= grace` 時清除 `since` 與 `gapSince`（歸零）
- 條件成立 ⇔ `since` 有值、`now - since >= for`，且（沒有中斷，或中斷未達 `grace`）

中斷期間的時間計入 `for`（`since` 不變），語意最簡單，也與「人看起來一直是這個狀態」的直覺一致。`grace` 預設 0，即原本的行為；`RuleEngine` 是純邏輯，重播工具自動得到相同語意。

替代方案：累計中斷時間（較難解釋與預測）；在 `SignalExtractor` 對訊號做移動平均（只能改善數值訊號，修不好 `face_visible` 這類布林訊號，也會讓觀測紀錄不再是原始值）。

### D11. VLM 定期巡檢

口鼻被蓋住是最危險的狀況，但第一段偵測不到。實測（2026-11-18，`kp-probe` 記錄臉部 5 個關鍵點可見度）：

| 動作 | nose | l_eye | r_eye |
|---|---|---|---|
| 正常 | 0.99–1.00 | 1.00 | 1.00 |
| 手 / 布經過臉前（移動中） | 0.33–0.49 | 0.19–0.37 | 0.22–0.60 |
| 口鼻被蓋住、靜止 | **0.98–1.00** | 0.03–1.00 | 0.98–1.00 |

COCO 17 個關鍵點沒有嘴巴；鼻子被蓋住時模型依眼睛與耳朵推算出位置並給出高可見度。可見度代表「位置有多確定」而不是「有沒有露出來」，因此任何以關鍵點為基礎的規則都無法偵測口鼻被蓋住。VLM 能理解「被子蓋到鼻子」這種語意，所以由 VLM 定期看畫面。

- **排程**：`PatrolScheduler` 執行緒每個巡檢各自計時，時間到且 `while` 成立時，從環形緩衝取最近 `frames` 張（相隔 1 秒）提交給 `VlmJudge`。前一次巡檢尚未完成時不重複提交
- **優先**：`VlmJudge` 佇列分兩級，巡檢工作排在觸發判定之前；巡檢工作不占觸發判定的 4 筆上限，但同一巡檢最多 1 筆在佇列中
- **確認**：連續 `confirm` 次 `alert: true` 才觸發；第一次為 true 時立即再巡檢一次。`every: 30s, confirm: 2` 時，最壞延遲約 30 + 2 × 2 秒。用來過濾小模型的單次誤判，代價是多一次判定的延遲
- **觸發**：交給 `AlertManager` 視同 `direct` 路由（立即送出），原因說明取自最後一次判定；同時向 `EventRecorder` 要求片段。冷卻、影子模式、壓制規定與狀況相同
- **停止偵測**：連續 3 次失敗或 `while` 成立期間超過 `3 × every` 沒有成功結果 → 產生系統警報（`situation` 為 `patrol_stalled:<id>`，`warning`，不可壓制）；恢復時記錄 `patrol_recovered:<id>`
- **紀錄**：每次結果寫 `BM_LOG_DIR/patrols-YYYYMMDD.jsonl`（與觀測紀錄相同保留 14 天），供統計 VLM 誤報率與延遲；不寫入警報紀錄，避免每 30 秒一筆擠掉真正的警報
- **成本**：2 張 640 寬的影格約 650 tokens，llama-server 上約 0.6 秒；每 30 秒一次約占 GPU 時間 2%

巡檢只會讓提醒發生，VLM 沒有撤回任何提醒的路徑，因此 `critical` 巡檢與底線保護（D2）一致。巡檢不能重播（需要 VLM），`bm_replay_rules` 略過並註明。

替代方案：以鼻子關鍵點可見度判斷（上表證明無效）；另加臉部偵測 / 口罩分類模型（多一個模型、對包被子的嬰兒同樣未經驗證，之後若 VLM 誤報率太高再評估）；只在第一段觸發後才問 VLM（口鼻被蓋住時第一段沒有任何觸發）。

## Risks / Trade-offs

- [YOLO 對包被子的嬰兒偵測不到，`person_present` 長時間為 false] → `not_in_bed` 走 `llm` 路由讓 VLM 判斷；躺床蓋被子實驗（另開 change）會先量出偵測率，必要時改以 `activity` 為主的規則
- [`prone_likely` 的關鍵點啟發式不可靠] → 初始規則把 `prone` 設為 `warning` + `llm` 路由，等實驗資料確認後再決定是否改為 `critical` + `direct`
- [關鍵點模型會推算被遮住的部位，`face_visible` / `prone_likely` 在部分遮蔽時仍為「看得到」] → 口鼻被蓋住改由 VLM 巡檢偵測（D11）；`face_covered` 保留為整張臉被蓋住時較快的第一道防線
- [口鼻被蓋住只靠 VLM 偵測，VLM 失效即失去偵測] → 巡檢停止系統警報（不可壓制）；README 說明 VLM 服務必須常駐並以 systemd 監看
- [VLM 巡檢誤報] → `confirm` 連續確認；巡檢紀錄可統計誤報率，調整 prompt、`confirm` 或模型
- [小模型照抄 prompt 中的選項，使 `alert` 與 `cause` 互相矛盾（實測 Qwen2.5-VL-3B 回覆 `{"alert": true, "cause": "只是轉頭、低頭或離開畫面"}`）] → prompt 只描述判斷標準，不列出可照抄的句子；模型比較（7.1）納入一致性
- [環境中的物品被誤認為人（實測 `person_count` 87% 的影格為 2），使 `adult_present` 長時間壓制所有 `warning`] → 先記錄；躺床實驗時以畫面確認來源，必要時以 `bed_roi` 或最小框尺寸排除
- [`critical` 狀況被設成影子模式或 `enabled: false`，等於關掉提醒] → 這是明確的設定動作，保留彈性；啟動與重載時印出警告，監視頁面醒目標示
- [VLM 與 YOLO 共用 8 GB GPU，VLM 推論時 YOLO 變慢] → 先以 spike 量測；必要時 VLM 用較小模型或放到另一台機器（D5 已支援）
- [VLM 給出錯誤的「不需要提醒」] → `critical` 不經 VLM 決定；所有判定都保留原因與片段，可在頁面上檢查後調整 prompt
- [觸發門檻偏敏感造成 VLM 呼叫過多] → 冷卻時間與佇列上限；觀測紀錄可離線計算每小時觸發次數
- [事件片段占用磁碟] → `BM_EVENT_MAX_MB` 上限，從最舊的刪除
- [網頁沒有認證，警報與片段畫面在同網路內可被任何人查看] → 與既有即時影像相同的風險，維持「只在信任網路使用」的說明

## Migration Plan

1. 新功能都是新增；既有 API 欄位與 `/ws/live` 格式不變，舊前端照常運作
2. 部署時需要新增 `config/situations.yaml` 與 `config/prompts/`；規則檔不存在時服務無法啟動，README 與 systemd 範例需同步說明
3. 沒有 VLM 時服務照常運作（`llm` 路由依 `on_timeout`，預設為提醒），可以先上線第一段再加 VLM
4. 回復：以 git 回到前一版本；新增的 `events/`、`logs/` 可直接刪除

## Open Questions

- 選用哪個 VLM 與量化版本：以 spike 在本機實測 3B–7B 級的模型（延遲、GPU 記憶體、與 YOLO 並行時的影響）後決定，只影響 `BM_VLM_URL` / `BM_VLM_MODEL` 的值
- 監視頁面以外的通知管道（例如提示音、桌面通知、本機網路內的其他裝置）：之後新增時是額外的送出方式，不改變警報的產生與紀錄
- 六個初始狀況的門檻數值與 `grace`：先用預設，待躺床實驗與真實嬰兒影像校正
- 巡檢間隔與確認次數（預設 `every: 30s`、`confirm: 2`）：以巡檢紀錄的誤報率與延遲調整；是否需要只在夜間巡檢
