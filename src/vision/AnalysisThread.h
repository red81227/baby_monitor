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
#include <thread>

// 分析執行緒：等待更新的 RawFrame (latest-wins) → reduced decode 子碼流 → 移動分數
// → 依排程執行 YOLO 與 MotionTracker → 發布 DetectionResult 並更新 MonitorState
class AnalysisThread {
public:
    using ResultCallback = std::function<void(const DetectionResult&)>;

    AnalysisThread(LatestSlot<RawFrame>& frames, YoloDetector& detector, MonitorState& state,
                   Heartbeat& heartbeat, float motionPx, double minHz, double maxHz, ResultCallback onResult);
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

    std::atomic<bool> running_{false};
    std::thread worker_;
};
