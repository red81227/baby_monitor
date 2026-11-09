#pragma once

#include "models/MonitorState.h"
#include "vision/MotionTracker.h"
#include "vision/YoloDetector.h"

#include <opencv2/videoio.hpp>

#include <atomic>
#include <functional>
#include <thread>

// 擷取執行緒：攝影機 -> 人物偵測 -> 移動判定 -> 畫框 -> 更新 MonitorState
class VisionPipeline {
public:
    VisionPipeline(MonitorState& state, YoloDetector& detector, float motionPx,
                   std::function<void()> onCameraLost);
    ~VisionPipeline();

    bool openCamera(int cameraIndex);
    void start();
    void stop();

    bool cameraLost() const { return cameraLost_.load(); }

private:
    void processLoop();

    MonitorState& state_;
    YoloDetector& detector_;
    MotionTracker tracker_;
    std::function<void()> onCameraLost_;

    cv::VideoCapture cap_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cameraLost_{false};
    std::thread worker_;
};
