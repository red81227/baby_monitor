#include "config/Config.h"
#include "controllers/ApiController.h"
#include "controllers/StaticController.h"
#include "models/MonitorState.h"
#include "vision/VisionPipeline.h"
#include "vision/YoloDetector.h"

#include <crow.h>

#include <csignal>
#include <iostream>
#include <memory>

int main() {
    // 寫入已斷線的連線時不要讓整個程式結束
    std::signal(SIGPIPE, SIG_IGN);

    std::string error;
    const auto cfg = loadConfig(error);
    if (!cfg) {
        std::cerr << error << std::endl;
        return 1;
    }

    // 模型先載入：失敗時還沒開攝影機、也還沒監聽任何 port
    std::unique_ptr<YoloDetector> detector;
    try {
        detector = std::make_unique<YoloDetector>(cfg->modelPath, cfg->personConf);
    } catch (const std::exception& e) {
        std::cerr << "錯誤: " << e.what() << std::endl;
        return 1;
    }

    MonitorState state;
    crow::SimpleApp app; // 預設處理 SIGINT / SIGTERM：收到後 app 停止
    app.loglevel(crow::LogLevel::Warning);
    ApiController::registerRoutes(app, state);
    StaticController::registerRoutes(app);

    VisionPipeline pipeline(state, *detector, cfg->motionPx, [&app]() { app.stop(); });

    const int cameraIndex = 0;
    if (!pipeline.openCamera(cameraIndex)) {
        std::cerr << "錯誤: 無法開啟攝影機 (index " << cameraIndex << ")" << std::endl;
        return 1;
    }

    // 等伺服器啟動後才開始擷取，確保攝影機中斷時 app.stop() 一定有效
    auto server = app.port(static_cast<uint16_t>(cfg->port)).multithreaded().run_async();
    app.wait_for_server_start();
    std::cout << "BabyMonitor running on http://0.0.0.0:" << cfg->port << std::endl;
    pipeline.start();

    server.wait();
    std::cout << "Shutting down..." << std::endl;
    pipeline.stop();

    return pipeline.cameraLost() ? 1 : 0;
}
