# Spec Delta

## REMOVED Requirements

### Requirement: 提供 MJPEG 串流
**Reason**: 原本是給 Blynk App 的影像元件使用；Blynk 已棄用，即時畫面改由網頁介面提供，不再需要第二個 HTTP 服務。
**Migration**: 改用瀏覽器開啟 `http://<host>:<BM_PORT>/`（預設 18080）；需要單張影格的程式改呼叫 `GET /api/frame.jpg`。

### Requirement: 支援多個同時連線的客戶端
**Reason**: 隨 MJPEG 串流一併移除。
**Migration**: 網頁介面與 `/api/frame.jpg` 是一般的 HTTP 請求，本來就能同時服務多個瀏覽器。

### Requirement: 客戶端斷線不影響服務
**Reason**: 隨 MJPEG 串流一併移除。
**Migration**: 網頁服務沒有長連線；`service-lifecycle` 仍要求程式不因 SIGPIPE 終止。
