#include "models/MonitorState.h"

void MonitorState::updateDetection(bool personDetected, bool motionDetected) {
    std::lock_guard<std::mutex> lock(mutex_);
    personDetected_ = personDetected;
    motionDetected_ = motionDetected;
    if (motionDetected) {
        lastMotionAt_ = std::chrono::system_clock::now();
    }
}

void MonitorState::updateFrameJpeg(std::vector<unsigned char>&& jpeg) {
    std::lock_guard<std::mutex> lock(mutex_);
    latestFrameJpeg_ = std::move(jpeg);
}

MonitorState::Status MonitorState::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);

    long long lastMotionEpochMs = 0;
    if (lastMotionAt_.time_since_epoch().count() != 0) {
        lastMotionEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            lastMotionAt_.time_since_epoch())
            .count();
    }

    return Status{motionDetected_, personDetected_, lastMotionEpochMs, latestFrameJpeg_.size()};
}

std::vector<unsigned char> MonitorState::getFrameJpegCopy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latestFrameJpeg_;
}
