#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// 影像管線與 HTTP 執行緒共享的監視狀態 (執行緒安全)
class MonitorState {
public:
    struct Status {
        bool motionDetected;
        bool personDetected;
        long long lastMotionEpochMs; // 從未發生移動時為 0
        std::size_t latestFrameBytes;
    };

    // 每處理完一張影格呼叫一次
    void updateDetection(bool personDetected, bool motionDetected);
    void updateFrameJpeg(std::vector<unsigned char>&& jpeg);

    Status getStatus() const;
    std::vector<unsigned char> getFrameJpegCopy() const;

private:
    mutable std::mutex mutex_;
    bool motionDetected_ = false;
    bool personDetected_ = false;
    std::chrono::system_clock::time_point lastMotionAt_{};
    std::vector<unsigned char> latestFrameJpeg_;
};
