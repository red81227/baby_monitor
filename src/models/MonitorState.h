#pragma once

#include "vision/Frame.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

// 影像管線與 HTTP 執行緒共享的監視狀態 (執行緒安全)。
// 每次更新都發布一份新的不可變 Snapshot；讀取端只複製指標，不在鎖內複製影像。
class MonitorState {
public:
    struct Snapshot {
        bool motionDetected = false;
        bool personDetected = false;
        long long lastMotionEpochMs = 0;  // 從未發生移動時為 0
        bool cameraConnected = false;
        uint64_t seq = 0;                 // 最新影格序號；尚無影格時為 0
        double detectHz = 0.0;            // 最近的實際人物偵測頻率
        std::shared_ptr<const RawFrame> frame;  // 最新影格；尚無影格時為 nullptr
    };

    MonitorState();

    // 擷取執行緒：新影格 (同時代表攝影機已連線)
    void updateFrame(std::shared_ptr<const RawFrame> frame);
    void setCameraConnected(bool connected);

    // 分析執行緒：每完成一次偵測呼叫一次
    void updateDetection(bool personDetected, bool motionDetected);
    void setDetectHz(double hz);

    std::shared_ptr<const Snapshot> snapshot() const;

private:
    template <class F>
    void modify(F&& f);

    mutable std::mutex mutex_;
    std::shared_ptr<const Snapshot> current_;
};
