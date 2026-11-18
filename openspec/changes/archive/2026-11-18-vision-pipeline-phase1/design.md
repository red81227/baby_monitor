# Design

## Context

動機見 proposal.md；整體藍圖、時間對齊原則（D4）與分期見 `redesign-vision-architecture/design.md`。本設計只處理 Phase 1。

現況重點：
- `VisionPipeline::processLoop` 在一條執行緒內依序做擷取、`YoloDetector::detectPerson`、`MotionTracker::update`、畫框、`imencode`
- `MonitorState` 以 mutex 保護 `vector<uchar>`，`/api/frame.jpg` 每次整份複製
- `main.cpp` 寫死攝影機 index 0；攝影機中斷時呼叫 `app.stop()`，結束碼為 1
- Crow 1.2.1 header-only，支援 WebSocket；HTTP 回應的長時間串流能力未確認
- 硬體：i7-12700H + RTX 3070 Ti Laptop；YOLO26n 在 CUDA 上預期每次數毫秒

## Goals / Non-Goals

**Goals:**
- 擷取永遠以攝影機影格率運作，不被分析或客戶端拖慢
- 主碼流零解碼、零重新編碼；多個觀看者共用同一份資料
- 每個對外的結果都能追溯到 `seq` / `tCapture`
- 元件邊界與藍圖一致，Phase 2 只需在上面加節點

**Non-Goals:**
- 時序狀態估計器、watermark 融合（Phase 2；本期只產生時間戳）
- ROI 設定、亮度旗標、環形緩衝、事件片段
- 節點心跳的完整設計（本期只做 systemd watchdog 的最外層）
- 調整 `MotionTracker` 的判定邏輯

## 元件與資料流

```
                         +---------------------------------------+
 /dev/videoN (MJPG) ---> | CaptureThread (FrameSource 實作)       |
                         |  grab -> 原始 JPEG bytes + tCapture    |
                         |  seq++                                 |
                         |  斷線: 回報中斷 -> 退避重連 (<= 5s)     |
                         +-------------------+-------------------+
                                             | publish
                                             v
                         FrameSlot<shared_ptr<const RawFrame>>
                         RawFrame { seq, tCapture, jpeg, width, height }
                                             |
              +------------------------------+----------------------------+
              |                              |                            |
              v                              v                            v
   +---------------------+      +------------------------+     /api/frame.jpg
   | AnalysisThread      |      | LiveHub                |     (直接回傳 jpeg,
   |  取最新 RawFrame     |      |  每個客戶端一個          |      X-Frame-Seq)
   |  reduced decode     |      |  latest-wins 待送槽      |
   |  -> 子碼流 (gray +   |      |  沒有客戶端就什麼都不做   |
   |     BGR)            |      +-----------+------------+
   |  ActivityScorer     |                  ^
   |  DetectScheduler    |                  | metadata {seq, box, conf, motion}
   |  [到時間才] YOLO     |------------------+
   |  MotionTracker      |
   |  -> DetectionResult |----> MonitorState (不可變快照)
   +---------------------+                  |
                                            v
                                     /api/status
 Watchdog: CaptureThread 與 AnalysisThread 各自更新「最後進度時間」，
           notifier 執行緒檢查兩者都在週期內才送 WATCHDOG=1
```

執行緒：Capture 1 條、Analysis 1 條、watchdog notifier 1 條（只在有 `WATCHDOG_USEC` 時啟動）、Crow 的 HTTP worker。

## Decisions

### D1. 以 OpenCV V4L2 後端取得原始 MJPG，必要時改用 V4L2 ioctl

設定 `CAP_PROP_FOURCC=MJPG`、`CAP_PROP_CONVERT_RGB=0` 後，`retrieve()` 回傳的是 1xN 的原始 JPEG bytes；`CAP_PROP_POS_MSEC` 是 V4L2 buffer 的時間戳（核心 `CLOCK_MONOTONIC`）。`FrameSource` 介面隱藏這些細節：

```
class FrameSource {
  open() / close() / grab() -> optional<RawFrame>
  describe() -> {device, fourcc, width, height}
}
```

**Spike S1** 先驗證：(a) 拿到的 bytes 是否為完整 JPEG（部分 UVC 攝影機省略 Huffman table，需要補 DHT）、(b) 時間戳是否單調且與 `steady_clock` 同一時間軸、(c) `CAP_PROP_BUFFERSIZE` 能否降到 1–2。失敗時改寫為直接使用 V4L2 ioctl（mmap + `VIDIOC_DQBUF`）的 `V4l2FrameSource`；介面不變，所以不影響其他元件。

時間戳若不是 monotonic，退回在 `grab()` 返回時取 `steady_clock::now()`。

**S1 結論（2026-11-11，HP Wide Vision HD Camera / uvcvideo，OpenCV 4 V4L2 後端）**：採用 OpenCV，不需要改寫 ioctl。
- (a) `CONVERT_RGB=0` 時 `retrieve()` 回傳 1xN `CV_8U` 原始 JPEG（`FFD8` 開頭），100/100 張都含 DHT（`FFC4`），`cv::imdecode` 全部成功；不需要補 Huffman table（實作仍保留「缺 DHT 時補標準表」的檢查，換攝影機時不會出錯）
- (b) `CAP_PROP_POS_MSEC` 100 張皆單調遞增，與 `CLOCK_MONOTONIC`（即 `steady_clock`）的差固定在 32–36 ms（約一張影格的曝光 / 傳輸時間），確認為同一時間軸；直接採用為 `tCapture`
- (c) `CAP_PROP_BUFFERSIZE` 可設定；實作時發現設為 1 會讓影格率減半（30 → 15 fps，讀取期間驅動沒有 buffer 可寫），所以使用 2
- (d) MJPG 支援 1280x720、640x360、640x480、352x288、320x240、176x144，全部 30 fps；1280x720 實測 28.9 fps、每張約 80–140 KB；YUYV 最高只到 640x480
- 設定順序：FOURCC → 寬高 → BUFFERSIZE → `CONVERT_RGB=0`（spike 以此順序驗證）

### D2. latest-wins slot

```
template<class T> class LatestSlot {
  publish(shared_ptr<const T>)       // 覆蓋舊值，notify
  waitNewerThan(seq, deadline) -> shared_ptr<const T>
}
```

讀取端只拿到指標，不複製影像；舊的 `RawFrame` 在最後一個讀取端放開後才釋放。`MonitorState` 也改為發布 `shared_ptr<const Snapshot>`，`/api/status` 與 `/api/frame.jpg` 讀快照，不再在鎖內複製 vector。

替代方案：有界佇列——消費者慢時延遲累積；每個讀取端複製一份——多人觀看時記憶體與 CPU 成本線性增加。

### D3. 子碼流用 reduced decode

選擇 `IMREAD_REDUCED_COLOR_{2,4,8}` 中「長邊仍 >= 640」的最大縮小倍率（1280x720 → 1/2 = 640x360；1920x1080 → 1/2 = 960x540）。libjpeg 在 DCT 階段直接縮小，成本約為全尺寸解碼的 1/4 以下。主碼流長邊小於 1280 時不縮小（倍率 1）。

YOLO 的 letterbox 輸入仍為 640，所以子碼流長邊約 640 時幾乎不損失偵測精度。框座標乘上縮小倍率換回主碼流座標，`BM_MOTION_PX` 以主碼流像素解讀。

### D4. 移動分數

在子碼流的灰階縮圖（再縮到約 160x90）上，計算與上一張分析影格的絕對差，`|diff| > 25` 的像素比例即為分數（0–1）。每張分析的影格都算，成本約數十微秒。門檻預設為 0.01（1% 像素變化），先寫成常數，Phase 2 有評估資料後再決定是否開放設定。

替代方案：光流——更準但貴得多，對「是否要加速 YOLO」這種粗判斷不划算。

### D5. 偵測排程

```
            score > 門檻
   +------+ ------------> +--------+
   | IDLE |               | ACTIVE |   每次 score > 門檻都更新 lastActive
   | minHz| <------------ | maxHz  |
   +------+  now - lastActive > 5s
```

AnalysisThread 迴圈：等待比上一次更新的 RawFrame → reduced decode → 算分數 → 若 `now - lastDetect >= 1/hz` 就跑 YOLO 與 `MotionTracker`，發布結果。分數本身每張影格都算，所以從 IDLE 切到 ACTIVE 的延遲最多一張影格。`detectHz` 以最近 3 秒實際完成的偵測次數計算。

分析迴圈處理每張影格的成本（解碼 + 分數）低於影格間隔時，所有影格都會被評分；高於時自然跳格（latest-wins），不會累積。

### D6. 即時影像傳輸：spike 決定 multipart MJPEG 或 WebSocket

**Spike S2** 驗證 Crow 1.2.1 能否在一般 HTTP 路由上長時間寫出 `multipart/x-mixed-replace`（需要能持有連線並從其他執行緒寫入）。

| 方案 | 優點 | 缺點 |
|---|---|---|
| multipart MJPEG + 另一條 metadata 通道 | `<img>` 就能顯示、瀏覽器原生解碼 | 影像與 metadata 兩條連線；Crow 支援度未知 |
| WebSocket（binary: `seq` + JPEG；text: metadata JSON） | 同一條連線、Crow 原生支援、背壓可控 | 前端要用 `createImageBitmap` 解碼並畫到 canvas |

預設傾向 **WebSocket `/ws/live`**：一條連線同時送影像和 metadata，符合「同一個連線推送 metadata」的 spec，且 `seq` 對齊最簡單。binary 訊息格式：`[u32 magic][u64 seq][u64 tCaptureUs][jpeg...]`（little-endian）。

背壓：LiveHub 為每個連線保留一個「待送」槽；前一張還沒送完時新影格覆蓋待送槽（latest-wins），慢的客戶端只會跳格。

**S2 結論（2026-11-11）**：採用 **WebSocket `/ws/live`**。
- multipart MJPEG 不可行：Crow 1.2.1 的 `response::write()` 只是附加到 body，`end()` 時一次送出，沒有串流 / chunked 的 API
- WebSocket：`send_binary` / `send_text` 只做 `asio::post`，並以 `anchor_` weak_ptr 保護，可從其他執行緒呼叫；`check_destroy()` 會先呼叫 `onclose` 再 `delete`，所以 LiveHub 以同一把 mutex 保護「送出」與「`onclose` 移除連線」即可避免懸空指標；`app.stop()` 會對所有連線送 close
- Crow 沒有寫入完成的通知，送出佇列無上限，所以「送完了沒」改由**客戶端回 ack** 判定：每個連線最多 1 張影格在途，ack 前到的影格覆蓋待送槽；ack 是收到 binary 訊息就回（不等解碼），前端自己再做 latest-wins
- 實測（720p JPEG 約 80 KB、30 fps、30 秒）：不限速客戶端 30.1 fps、`seq` 無跳號；同時一個限速 400 KB/s 的客戶端 4.9 fps、`seq` 遞增但跳號（755 張被覆蓋），伺服器 RSS 維持約 5 MB；短連線斷開後正常清理；有連線時送 SIGINT，連線收到 close 且程式正常結束
- metadata（text JSON）很小，不受在途限制；為了讓完全停住的客戶端不會無限累積，在途影格超過 10 秒沒有 ack 時伺服器主動關閉該連線

訊息格式（伺服器 → 客戶端）：
- binary：`[u32 magic = 0x314D4246 "FBM1"][u64 seq][u64 tCaptureUs][JPEG bytes]`，little-endian，標頭共 20 bytes
- text：`{"type":"detection","seq":N,"tCaptureUs":T,"person":bool,"box":[x,y,w,h]|null,"confidence":c,"motion":bool,"width":W,"height":H}`

客戶端 → 伺服器：每收到一個 binary 訊息回一個 text `"ack"`。

### D7. 前端以 seq 對齊畫框

前端保留最近約 1 秒的 `seq -> ImageBitmap`。收到 metadata 時，若對應的 `seq` 還在緩衝內，就在該影格上畫框；影格顯示時使用「`seq` 小於等於目前影格的最新一筆 metadata」，並且只在 `seq` 差距在約 1 秒內時畫框，避免舊框殘留。狀態列與攝影機中斷訊息仍由 `/api/status` 輪詢（1 秒）。

### D8. 攝影機重連

CaptureThread 在 `grab()` 失敗時：發布 `cameraConnected=false` → 關閉裝置 → 以 250ms、500ms、1s、2s、4s、5s、5s… 退避重開同一個裝置路徑。重開成功後沿用原本的協商參數，`seq` 延續。關閉時用旗標 + 可中斷的等待，確保 5 秒內結束。

啟動時第一次開啟失敗仍直接結束（保留「明確失敗」的行為），重連只在執行中觸發。

### D9. systemd 通知不依賴 libsystemd

直接對 `NOTIFY_SOCKET` 送 datagram（支援抽象 namespace 的 `@` 前綴），約 30 行。Capture 與 Analysis 各自記錄最後進度時間（重連嘗試也算 Capture 的進度）；notifier 每 `WATCHDOG_USEC/2` 檢查一次，兩者都在週期內才送 `WATCHDOG=1`。

替代方案：連結 libsystemd——多一個依賴，只為了兩個字串。

## Risks / Trade-offs

- [部分 UVC 攝影機的 MJPG 缺少 DHT，瀏覽器無法解碼] → S1 檢查；缺少時在主碼流前補標準 Huffman table（不重新編碼）
- [OpenCV V4L2 的 buffer 時間戳在某些核心 / 驅動不是 monotonic] → 檢查後退回 `steady_clock`
- [解析度提高後 `BM_MOTION_PX=20` 相對變得更敏感（以主碼流像素計）] → README 說明需要依解析度調整；Phase 2 的狀態估計器會改用正規化座標
- [1 fps 基準下，人物在畫面裡移動得很慢、移動分數一直低於門檻] → `MotionTracker` 是累積位移，1 fps 仍會累積到門檻；只是反應延遲最多 1 秒
- [WebSocket 送 720p MJPEG（約 3–6 MB/s）佔用本機網路] → 只在本機網路觀看；有需要時可以在端點加降頻參數
- [前端解碼與畫面更新在低階裝置上跟不上] → 前端也採 latest-wins，解碼中收到的影格直接丟棄
- [移除「攝影機中斷就結束」後，攝影機永久故障時服務會一直重連] → 狀態 API 與頁面會顯示中斷；長時間中斷的通知交給 Phase 3 的警報

## Migration Plan

1. 先完成 S1 / S2，結果寫回本文件的 D1 / D6
2. 新的元件與舊的 `VisionPipeline` 並存，直到新管線通過測試後，再刪除舊的 `VisionPipeline` 與 `MonitorState::updateFrameJpeg`
3. 前端改用 `/ws/live`；`/api/frame.jpg` 保留（改成沒有框）
4. README：新環境變數、`/ws/live` 格式、攝影機中斷行為、systemd 範例改成 `Type=notify` + `WatchdogSec=30`
5. 回復：git 回到前一版本；沒有持久資料需要遷移

## Open Questions

- 移動分數的門檻與 IDLE 回復時間（5 秒）是否要開放成環境變數——等 Phase 2 有評估資料再決定
- 即時影像是否需要降頻 / 降解析度參數，給低頻寬的觀看端使用
