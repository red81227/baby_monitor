#pragma once

#include "common/Heartbeat.h"
#include "common/LatestSlot.h"
#include "models/MonitorState.h"
#include "vision/ActivityScorer.h"
#include "vision/DetectScheduler.h"
#include "vision/Frame.h"
#include "vision/MotionTracker.h"
#include "vision/YoloDetector.h"

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

// 分析執行緒：等待更新的 RawFrame (latest-wins) → reduced decode 子碼流 → 移動分數
// → 依排程執行 YOLO 與 MotionTracker → 發布 DetectionResult 並更新 MonitorState。
// 每張分析影格 (不論有沒有執行偵測) 都以 onAnalysis 回報移動分數與偵測結果 (沒有偵測時為 nullptr)。
class AnalysisThread {
public:
    using ResultCallback = std::function<void(const DetectionResult&)>;
    using AnalysisCallback =
        std::function<void(uint64_t seq, int64_t tCaptureUs, double activity, const DetectionResult* detection)>;
    // 移動分數只計算的區域 (以畫面比例表示)；回傳 nullopt 表示整個畫面
    using RoiProvider = std::function<std::optional<cv::Rect2f>()>;

    AnalysisThread(LatestSlot<RawFrame>& frames, YoloDetector& detector, MonitorState& state,
                   Heartbeat& heartbeat, float motionPx, double minHz, double maxHz, ResultCallback onResult,
                   AnalysisCallback onAnalysis = nullptr, RoiProvider activityRoi = nullptr);
    ~AnalysisThread();

    void start();
    void stop();  // 呼叫前或同時應關閉 frames，讓等待立即返回

private:
    void run();

    LatestSlot<RawFrame>& frames_;
    YoloDetector& detector_;
    MonitorState& state_;
    Heartbeat& heartbeat_;
    MotionTracker tracker_;
    ActivityScorer scorer_;
    DetectScheduler scheduler_;
    ResultCallback onResult_;
    AnalysisCallback onAnalysis_;
    RoiProvider activityRoi_;

    std::atomic<bool> running_{false};
    std::thread worker_;
};
