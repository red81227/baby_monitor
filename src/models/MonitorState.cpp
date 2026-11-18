#include "models/MonitorState.h"

MonitorState::MonitorState() : current_(std::make_shared<const Snapshot>()) {}

template <class F>
void MonitorState::modify(F&& f) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = std::make_shared<Snapshot>(*current_);  // 複製的是欄位與 frame 指標，不是影像
    f(*next);
    current_ = std::move(next);
}

void MonitorState::updateFrame(std::shared_ptr<const RawFrame> frame) {
    modify([&](Snapshot& s) {
        s.seq = frame ? frame->seq : s.seq;
        s.frame = std::move(frame);
        s.cameraConnected = true;
    });
}

void MonitorState::setCameraConnected(bool connected) {
    modify([&](Snapshot& s) { s.cameraConnected = connected; });
}

void MonitorState::updateDetection(bool personDetected, bool motionDetected) {
    modify([&](Snapshot& s) {
        s.personDetected = personDetected;
        s.motionDetected = motionDetected;
        if (motionDetected) {
            s.lastMotionEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
        }
    });
}

void MonitorState::setDetectHz(double hz) {
    modify([&](Snapshot& s) { s.detectHz = hz; });
}

std::shared_ptr<const MonitorState::Snapshot> MonitorState::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}
