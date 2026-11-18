#include "common/Heartbeat.h"
#include "common/LatestSlot.h"
#include "common/SystemdNotifier.h"
#include "config/Config.h"
#include "controllers/ApiController.h"
#include "controllers/LiveHub.h"
#include "controllers/StaticController.h"
#include "models/MonitorState.h"
#include "vision/AnalysisThread.h"
#include "vision/CaptureThread.h"
#include "vision/FrameSource.h"
#include "vision/YoloDetector.h"

#include <crow.h>

#include <csignal>
#include <iostream>
#include <memory>

int main() {
    // 寫入已斷線的連線時不要讓整個程式結束
    std::signal(SIGPIPE, SIG_IGN);

    // 1. 設定
    std::string error;
    const auto cfg = loadConfig(error);
    if (!cfg) {
        std::cerr << error << std::endl;
        return 1;
    }

    // 2. 模型：失敗時還沒開攝影機、也還沒監聽任何 port
    std::unique_ptr<YoloDetector> detector;
    try {
        detector = std::make_unique<YoloDetector>(cfg->modelPath, cfg->personConf);
    } catch (const std::exception& e) {
        std::cerr << "錯誤: " << e.what() << std::endl;
        return 1;
    }

    // 3. 攝影機：啟動時開不了就明確失敗 (執行中的中斷才由 CaptureThread 重連)
    V4l2FrameSource source(cfg->cameraDevice, cfg->captureWidth, cfg->captureHeight);
    if (!source.open()) {
        std::cerr << "錯誤: " << source.lastError() << std::endl;
        return 1;
    }
    const auto cam = source.describe();
    std::cout << "[INFO] 攝影機: " << cam.device << " 格式 " << cam.fourcc << " " << cam.width << "x" << cam.height
              << " @" << cam.fps << "fps";
    if (cam.width != cfg->captureWidth || cam.height != cfg->captureHeight) {
        std::cout << " (要求 " << cfg->captureWidth << "x" << cfg->captureHeight << " 不支援，使用協商結果)";
    }
    std::cout << std::endl;

    MonitorState state;
    LatestSlot<RawFrame> frames;
    Heartbeat captureBeat, analysisBeat;
    LiveHub live(frames);

    // 4. HTTP (Crow 預設處理 SIGINT / SIGTERM：收到後 app 停止並關閉 WebSocket 連線)
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    ApiController::registerRoutes(app, state);
    StaticController::registerRoutes(app);
    live.registerRoutes(app);

    auto server = app.port(static_cast<uint16_t>(cfg->port)).multithreaded().run_async();
    app.wait_for_server_start();
    std::cout << "BabyMonitor running on http://0.0.0.0:" << cfg->port << std::endl;

    // 5. 擷取 / 分析 / 即時影像 / systemd 通知
    CaptureThread capture(source, frames, state, captureBeat);
    AnalysisThread analysis(frames, *detector, state, analysisBeat, cfg->motionPx, cfg->detectMinHz,
                            cfg->detectMaxHz, [&live](const DetectionResult& r) { live.publishDetection(r); });
    capture.start();
    analysis.start();
    live.start();

    SystemdNotifier notifier;
    notifier.notify("READY=1");
    notifier.startWatchdog({&captureBeat, &analysisBeat});

    server.wait();
    std::cout << "Shutting down..." << std::endl;
    notifier.notify("STOPPING=1");
    notifier.stop();
    frames.close();  // 喚醒所有等待影格的執行緒
    live.stop();
    analysis.stop();
    capture.stop();
    return 0;
}
