#include "alerts/AlertManager.h"
#include "common/Heartbeat.h"
#include "common/LatestSlot.h"
#include "common/StallWatchdog.h"
#include "common/SystemdNotifier.h"
#include "config/Config.h"
#include "controllers/ApiController.h"
#include "controllers/LiveHub.h"
#include "controllers/RoiController.h"
#include "controllers/SituationController.h"
#include "controllers/StaticController.h"
#include "events/EventRecorder.h"
#include "events/RingBuffer.h"
#include "judge/PatrolScheduler.h"
#include "judge/VlmJudge.h"
#include "models/MonitorState.h"
#include "situation/ObservationLog.h"
#include "situation/RulesStore.h"
#include "situation/SituationMonitor.h"
#include "vision/AnalysisThread.h"
#include "vision/CaptureThread.h"
#include "vision/FrameSource.h"
#include "vision/YoloDetector.h"

#include <crow.h>

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

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

    // 2. 狀況規則：啟動時不存在或驗證失敗就明確失敗 (執行中的修改由 RulesStore 熱重載)
    // 網頁設定的 ROI 檔優先於規則檔的 bed_roi，無效時同樣啟動失敗
    RulesStore rules(cfg->rulesPath, cfg->ringSeconds, std::chrono::milliseconds(2000),
                     bedRoiFilePath(cfg->overridesDir));
    if (!rules.loadInitial(error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    std::cout << "[INFO] 狀況規則: " << cfg->rulesPath << " (version " << rules.current()->version << ", "
              << rules.current()->situations.size() << " 個狀況, " << rules.current()->patrols.size() << " 個巡檢, ROI "
              << toString(rules.current()->bedRoiSource) << ")" << std::endl;

    // 3. 模型：失敗時還沒開攝影機、也還沒監聽任何 port
    std::unique_ptr<YoloDetector> detector;
    try {
        detector = std::make_unique<YoloDetector>(cfg->modelPath, cfg->personConf);
    } catch (const std::exception& e) {
        std::cerr << "錯誤: " << e.what() << std::endl;
        return 1;
    }

    // 4. 攝影機：啟動時開不了就明確失敗 (執行中的中斷才由 CaptureThread 重連)
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

    // 5. 環形緩衝與事件片段
    RingBuffer ring(int64_t(cfg->ringSeconds) * 1'000'000);
    EventRecorder recorder(ring, cfg->eventDir, cfg->eventMaxMb * 1024 * 1024);

    // 6. 狀況判斷：第一段 (訊號 → 觀測紀錄 → 規則) 觸發後交給警報管理；llm 路由由第二段 VLM 判定
    VlmJudge::Options vlmOptions;
    vlmOptions.url = cfg->vlmUrl;
    vlmOptions.model = cfg->vlmModel;
    VlmJudge judge(vlmOptions);
    if (judge.enabled()) {
        std::cout << "[INFO] VLM: " << cfg->vlmUrl << (cfg->vlmModel.empty() ? "" : " 模型 " + cfg->vlmModel)
                  << std::endl;
    } else {
        std::cout << "[INFO] VLM 停用 (未設定 BM_VLM_URL)；llm 路由的狀況依 on_timeout 處理" << std::endl;
        if (!rules.current()->patrols.empty()) {
            std::cout << "[WARN] 規則檔有巡檢，但 VLM 未設定 (BM_VLM_URL)：巡檢無法進行，將送出「巡檢停止」警報"
                      << std::endl;
        }
    }

    ObservationLog observations(cfg->logDir);
    AlertLog alertLog(cfg->logDir);
    PatrolLog patrolLog(cfg->logDir);
    std::unique_ptr<SituationMonitor> situations;
    AlertManager::Deps alertDeps;
    alertDeps.rules = [&rules] { return rules.current(); };
    alertDeps.activeStates = [&situations] {
        return situations ? situations->activeStates() : std::map<std::string, SituationMonitor::ActiveState>{};
    };
    alertDeps.requestClip = [&recorder](const EventRecorder::Request& r, EventRecorder::ClipCallback cb) {
        return recorder.request(r, std::move(cb));
    };
    if (judge.enabled()) alertDeps.submitJudge = [&judge](VlmJudge::Job j) { judge.submit(std::move(j)); };
    alertDeps.log = &alertLog;
    AlertManager alerts(std::move(alertDeps));
    situations = std::make_unique<SituationMonitor>(rules, &observations, [&alerts](const Trigger& t) {
        std::cout << "[INFO] 狀況觸發: " << t.situationId << (t.shadow ? " (影子模式)" : "") << " (seq " << t.seq
                  << ", 規則 version " << t.rulesVersion << ")" << std::endl;
        alerts.onTrigger(t);
    });

    // VLM 定期巡檢：第一段訊號偵測不到的狀況 (口鼻被蓋住)，結果交給警報管理
    PatrolScheduler::Deps patrolDeps;
    patrolDeps.rules = [&rules] { return rules.current(); };
    patrolDeps.signals = [&situations] { return situations->lastSignals(); };
    patrolDeps.ring = &ring;
    if (judge.enabled()) patrolDeps.submit = [&judge](VlmJudge::Job j) { judge.submit(std::move(j)); };
    patrolDeps.onEvent = [&alerts](const PatrolScheduler::Event& e) { alerts.onPatrolEvent(e); };
    patrolDeps.log = &patrolLog;
    PatrolScheduler patrols(std::move(patrolDeps));

    // 網頁設定 ROI：寫入 ROI 檔並立即重新載入規則，修改記入觸發紀錄
    RoiService roiService(rules, [&alerts](const std::string& cause) {
        alerts.recordSystem("roi_changed", Severity::Info, cause);
    });

    // 7. HTTP (Crow 預設處理 SIGINT / SIGTERM：收到後 app 停止並關閉 WebSocket 連線)
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    ApiController::registerRoutes(app, state, [&rules, &alerts](crow::json::wvalue& j) {
        const auto st = rules.status();
        j["rulesVersion"] = st.rules ? st.rules->version : 0;
        if (st.lastError) j["rulesError"] = *st.lastError;
        else j["rulesError"] = nullptr;
        j["lastAlertEpochMs"] = alerts.lastAlertEpochMs();
    });
    SituationController::registerRoutes(app, {rules, *situations, alerts, recorder, judge, patrols});
    RoiController::registerRoutes(app, roiService);
    StaticController::registerRoutes(app);
    live.registerRoutes(app);

    auto server = app.port(static_cast<uint16_t>(cfg->port)).multithreaded().run_async();
    app.wait_for_server_start();
    std::cout << "BabyMonitor running on http://0.0.0.0:" << cfg->port << std::endl;

    // 8. 擷取 / 分析 / 即時影像 / systemd 通知
    CaptureThread capture(source, frames, state, captureBeat,
                          [&ring](const std::shared_ptr<const RawFrame>& f) { ring.push(f); });
    AnalysisThread analysis(
        frames, *detector, state, analysisBeat, cfg->motionPx, cfg->detectMinHz, cfg->detectMaxHz,
        [&live](const DetectionResult& r) { live.publishDetection(r); },
        [&situations](uint64_t seq, int64_t t, double activity, const DetectionResult* d) {
            situations->onAnalysis(seq, t, activity, d);
        },
        [&situations] { return situations->bedRoi(); });
    rules.start();
    recorder.start();
    judge.start();
    patrols.start();
    capture.start();
    analysis.start();
    live.start();

    SystemdNotifier notifier;
    notifier.notify("READY=1");
    notifier.startWatchdog({&captureBeat, &analysisBeat});

    // 停滯時自行結束 (BM_STALL_EXIT_SECONDS > 0)：先嘗試正常關閉，3 秒內沒結束就強制以 2 結束
    // (停滯的執行緒可能無法回應關閉)。與 systemd watchdog 互不影響。
    std::atomic<bool> stalled{false};
    StallWatchdog stallWatchdog(
        {{"擷取", &captureBeat}, {"分析", &analysisBeat}}, std::chrono::seconds(cfg->stallExitSeconds),
        [&app, &stalled](const std::string& name, std::chrono::seconds age) {
            std::cerr << "[ERROR] " << name << "停滯 " << age.count() << " 秒 (超過 BM_STALL_EXIT_SECONDS)，結束程式"
                      << std::endl;
            stalled = true;
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                std::cerr << "[ERROR] 正常關閉逾時，強制結束" << std::endl;
                ::_exit(2);
            }).detach();
            app.stop();
        });
    if (cfg->stallExitSeconds > 0) {
        std::cout << "[INFO] 停滯自行結束: " << cfg->stallExitSeconds << " 秒" << std::endl;
        stallWatchdog.start();
    }

    server.wait();
    std::cout << "Shutting down..." << std::endl;
    stallWatchdog.stop();
    notifier.notify("STOPPING=1");
    notifier.stop();
    frames.close();  // 喚醒所有等待影格的執行緒
    live.stop();
    analysis.stop();
    capture.stop();
    rules.stop();
    patrols.stop();   // 不再送出新的巡檢
    recorder.stop();  // 未完成的片段以現有影格產生，交給判定
    judge.stop();     // 中斷進行中的判定；未判定的依 on_timeout 處理
    observations.flush();
    return stalled ? 2 : 0;
}
