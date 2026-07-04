#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class MonitorState {
public:
    struct Status {
        bool motionDetected;
        long long lastMotionEpochMs;
        std::size_t latestFrameBytes;
    };

    void setMotionDetected(bool detected);
    void updateFrameJpeg(std::vector<unsigned char>&& jpeg);

    Status getStatus() const;
    std::vector<unsigned char> getFrameJpegCopy() const;

private:
    mutable std::mutex mutex_;
    bool motionDetected_ = false;
    std::chrono::system_clock::time_point lastMotionAt_{};
    std::vector<unsigned char> latestFrameJpeg_;
};
