# Design

## Context

動機見 proposal.md 的 Why。和設計直接相關的現況與限制：

- 擷取、推論、畫框、JPEG 編碼都在 `VisionPipeline` 的同一條執行緒，`MonitorState` 以 mutex 保護一個 `vector<uchar>`，每次 `/api/frame.jpg` 都整份複製。
- 攝影機是 USB `/dev/video0`，640x480、未指定 FOURCC；沒有子碼流。預期之後會換成支援 RTSP 主 / 子碼流的專業攝影機。
- 硬體：i7-12700H + RTX 3070 Ti Laptop，ONNX Runtime 1.27（CUDA EP），HTTP 使用 Crow 1.2.1。
- 只在本機觀看、不連網；目前沒有任何認證，這個限制在本設計中維持不變。

## Goals / Non-Goals

**Goals:**
- 定下可分期擴充的資料流骨架，之後每期只是加節點，不重寫管線
- 直播畫質與分析成本脫鉤；任何一段都不累積延遲
- 所有來源（影像、移動、偵測、音訊）有共同時間基準，可正確融合
- 影像來源可替換：USB 本機模擬與未來 RTSP 攝影機共用同一個介面

**Non-Goals:**
- 連網、遠端觀看、推播、認證、TURN
- 在 C++ 內自行實作 WebRTC
- 嬰兒專用模型的訓練方法（含合成影像預訓練，待討論）
- 本系統不是醫療設備，不保證偵測到任何危險狀況

## 整體架構

```
+-----------------------------------------------------------------------------+
|  影像來源層 (FrameSource 介面)                                               |
|                                                                             |
|  [Phase 1] USB 攝影機 (V4L2, MJPG)        [未來] RTSP 攝影機                 |
|      |  一次擷取 + 核心時間戳                 |  主碼流  |  子碼流             |
|      +--> 主碼流: MJPEG 原樣轉發，不解碼       |          |                    |
|      +--> 子碼流: 解碼時直接縮小 (1/2, 1/4)    |          |                    |
+------|------------------|--------------------|----------|-------------------+
       | main             | sub                | main     | sub
       v                  v                    v          v
+-------------+   +---------------------------------------------+
| 環形緩衝     |   | (1) 擷取 + 前處理                             |
| (壓縮影格，  |   |   latest-wins slot / ROI 裁切 / 亮度旗標      |
|  依時間戳    |   +-----------+-------------------+-------------+
|  索引)       |               |                   |
+--+-------+--+               v                   |
   |       |        +-------------------+         |
   |       |        | (2) 移動分數       |         |
   |       |        |  ROI 影格差分      |         |
   |       |        +----+----------+---+         |
   |       |             |          | 調整頻率      v
   |       |             |          +-------> +-------------------------+
   |       |             |                    | (3) YOLO (+ pose, 之後)  |
   |       |             |                    |   1 fps 基準 / 動態提高   |
   |       |             |                    +-----------+-------------+
   |       |             |                                |
   |       |    [Phase 4] 麥克風 --> 哭聲偵測 ---+          |
   |       |             |                      |         |
   |       |             v                      v         v
   |       |   +-------------------------------------------------+
   |       |   | 時間對齊層 (TimeBase + 依事件時間排序 + watermark) |
   |       |   +-------------------------+-----------------------+
   |       |                             v
   |       |   +-------------------------------------------------+
   |       |   | (4) 時序狀態估計器                                 |
   |       |   |   ABSENT / SLEEPING / STIRRING / AWAKE / CRYING   |
   |       |   +----+-------------------+-------------------+-----+
   |       |        v                   v                   v
   |       |   (5) 規則 / 警報     狀態紀錄 DB       (6) 本機小 LLM 旁路
   |       |        |                   |              (摘要 / 問答，
   |       |        v                   |               不在警報路徑上)
   |       |   (7) 警報管理器 <-- 使用者回饋 --> (8) 資料收集
   |       |        |                                  (+ 合成影像，待討論)
   |       v        v
   |   事件片段 <-- 觸發 (取 [t_event - pre, t_event + post])
   v
 本機直播: 主碼流 MJPEG 原樣送到瀏覽器 + 偵測 metadata，框在前端 canvas 上畫
 [未來 RTSP]: 改由 go2rtc 負責 WebRTC，C++ 不再碰直播

 (9) Watchdog: 每個節點回報心跳 + systemd WatchdogSec (細節待討論)
```

### 分期對照

```
 Phase 1  影像骨架           FrameSource / (1) / (2) / (3) 動態頻率 / 時間戳
                             本機直播 / 攝影機重連 / WatchdogSec
 Phase 2  狀態與事件          (4) 只用影像 / 環形緩衝 + 事件片段 / 狀態紀錄 DB
                             評估用重播測試
 Phase 3  警報               (5) (7) 本機通知 + 使用者回饋
 Phase 4  擴充               哭聲偵測 / pose / (6) 本機小 LLM / (8) 資料收集
 未來                        RTSP 攝影機 + go2rtc / WebRTC
```

## Decisions

### D1. 以 `FrameSource` 介面隔離影像來源；Phase 1 用 USB 在本機模擬雙碼流

USB 攝影機設定為 MJPG（解析度依攝影機能力，目標 720p 以上），每張 V4L2 buffer 同時產生：

- 主碼流：原始 MJPEG bytes（`CAP_PROP_CONVERT_RGB=0` 取得原始 buffer），不解碼、不重新編碼
- 子碼流：以 libjpeg DCT 縮放在解碼時直接取 1/2 或 1/4 大小（`IMREAD_REDUCED_COLOR_2/4`）

主碼流與子碼流來自同一張影格，共用 `seq` 與 `t_capture`。

替代方案：
- 解碼全尺寸後再縮小：多一次全尺寸解碼與 resize，CPU 成本高
- 解碼後重新編碼主碼流：浪費 CPU 且損失畫質
- 直接改用 ffmpeg / GStreamer 管線：功能強但依賴重；先以 OpenCV 驗證，`FrameSource` 介面讓之後可換

### D2. latest-wins slot 取代佇列

每個節點之間只保留「最新一份」不可變快照（`shared_ptr<const T>` + `seq`），消費者跟不上就直接跳過舊的。多個觀看者共用同一份資料，不需要複製。

替代方案：有界佇列——會在消費端慢時累積延遲，對即時監視沒有價值。

### D3. 移動分數控制 YOLO 頻率

(2) 在子碼流的 ROI 內做影格差分，算出便宜的移動分數。(3) 以 1 fps 為基準；移動分數超過門檻時提高頻率（上限可設定），回到靜止後逐步降回。目前的 `BM_MOTION_PX` 中心點位移判定保留為狀態估計器的一個輸入，而不再是唯一依據。

替代方案：固定頻率——靜止時浪費 GPU，有動靜時反應又不夠快。

### D4. 時間對齊：單一 monotonic 時鐘 + 擷取時間戳 + watermark

**時鐘域**

```
   時鐘域                    來源                         轉換到內部時間
  ------------------------------------------------------------------------
   V4L2 buffer 時間戳       核心 CLOCK_MONOTONIC         直接使用
   (USB 影像)                (CAP_PROP_POS_MSEC)
   ALSA 音訊 (Phase 4)       snd_pcm htstamp             直接使用; 之後用
                             (設為 MONOTONIC)             樣本計數 t0 + n/fs 推算
   RTSP (未來)               RTP ts + RTCP SR (NTP)      每路串流估計 offset
   牆鐘 (system_clock)       只用在顯示與 DB              寫入時記下 (mono, wall) 對照
```

程式內部一律使用 `CLOCK_MONOTONIC`，牆鐘只在顯示與寫入 DB 時轉換，避免 NTP 校時或手動改時間打亂判斷。

**觀測值格式**

```
Observation {
  source      : VIDEO_MAIN | VIDEO_SUB | MOTION | YOLO | AUDIO
  seq         : 該來源內的遞增序號
  t_capture   : 擷取當下的 monotonic 時間 (不是處理完成的時間)
  t_processed : 處理完成的時間 (只用來量延遲)
  payload     : ...
}
```

**watermark 融合**

```
 時間軸 (t_capture) ------------------------------------------------>

 MOTION  |x  x  x  x  x  x  x  x  x  x  x  x|      (快，幾乎沒有延遲)
 YOLO    |x        x        x        x      |  .   (1 fps 或動態，約 30ms)
 AUDIO   |[----win----][----win----]        |  ..  (0.5s 一窗，約 500ms)
                                     ^
                          watermark = min(各來源最後收到的 t_capture)
                                      - 允許的延遲上限

 (4) 只處理 t_capture <= watermark 的觀測值，依時間順序融合
 某來源超過 T 秒沒有新觀測值時不再等它，並回報給 (9)
```

**其他對齊點**
- 環形緩衝以 `t_capture` 索引，事件片段直接取 `[t_event - pre, t_event + post]`，與 (4) 的判定時間一致
- 前端收到的 metadata 帶 `seq`，框畫在對應 `seq` 的影格上，而不是最新影格
- 未來 RTSP：主 / 子碼流共用攝影機 NTP 時鐘，以 RTCP SR 換算；沒有 SR 時退回「抵達時間 + 抖動濾波」

替代方案：以處理完成時間排序——音訊與 YOLO 延遲不同，會把同一時刻的事件判成先後發生。

### D5. 前端畫框，影像保持乾淨

後端只送 metadata（`seq`、框、狀態），前端用 canvas 畫。直播、環形緩衝、事件片段都用沒有框的原始影像；標示樣式改前端即可。

### D6. 本機直播：Phase 1 原樣轉發 MJPEG；RTSP 之後改用 go2rtc

Phase 1 由 C++ 把主碼流原樣送到瀏覽器（multipart MJPEG 或 WebSocket binary，由 spike 決定），只在有人觀看時才送。接上 RTSP 攝影機後，直播交給 go2rtc 輸出 WebRTC，C++ 只吃子碼流做分析。

替代方案：在 C++ 內實作 WebRTC（libwebrtc / GStreamer webrtcbin）——依賴與複雜度過高。

### D7. LLM 旁路使用本機小模型

(6) 只讀取狀態紀錄 DB 與事件片段，產生摘要、第二意見與問答，不在警報路徑上；它延遲或失效都不影響 (5)(7)。不送任何資料到雲端。

### D8. 攝影機中斷在程式內重連，watchdog 分兩層

`FrameSource` 斷線時退避重連，模型與 HTTP 服務不重啟。各節點回報心跳給 (9)；外層使用 systemd `WatchdogSec`，程式整體卡住時由 systemd 重啟。心跳的細節設計待討論。

## Risks / Trade-offs

- [OpenCV V4L2 後端是否能穩定取得原始 MJPG buffer 與 buffer 時間戳] → Phase 1 先做 spike 驗證；不可行時 `FrameSource` 改用直接呼叫 V4L2 ioctl 的實作
- [Crow 是否支援長時間的 multipart 串流回應] → spike 驗證；不行就改用 Crow 的 WebSocket 推送 binary JPEG
- [1 fps 基準頻率下，突發狀況的反應延遲] → 由移動分數即時提高頻率；移動分數本身每張子碼流影格都算
- [COCO 模型對包被子、趴睡的嬰兒偵測不穩] → `BM_PERSON_CONF` 可調；pose 在有資料前只當參考，不觸發警報；合成影像預訓練待討論
- [不同來源延遲差異大，watermark 等待增加判定延遲] → 每個來源設定允許延遲上限，超過就不等
- [環形緩衝的記憶體用量（720p MJPEG 約數 MB/s）] → 緩衝長度可設定；只存壓縮影格
- [使用者誤以為系統能保證安全] → 文件與頁面明確說明本系統不是醫療設備

## Migration Plan

1. Phase 1 以新管線取代 `VisionPipeline`；`/api/status` 保留既有欄位、新增欄位，舊前端在過渡期仍可運作
2. 攝影機中斷從「以 1 結束」改為「程式內重連」，README 與 systemd 範例同步更新（加入 `WatchdogSec`）
3. 回復方式：以 git 回到前一版本；本變更不改動資料格式，沒有需要遷移的持久資料（DB 在 Phase 2 才導入）

## Open Questions

以下確定延後討論，不影響 Phase 1 的骨架：

- 以合成影像預訓練 / fine-tune 嬰兒偵測與 pose 模型的方法
- 評估方式：錄製片段的資料集、重播測試的流程與指標
- 心跳系統細節：心跳週期、各節點逾時、異常時的處理與回報方式
- 狀態估計器各狀態的轉換條件與遲滯參數（Phase 2 撰寫 specs 時決定）
