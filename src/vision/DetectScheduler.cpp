#include "vision/DetectScheduler.h"

#include <algorithm>

DetectScheduler::DetectScheduler(double minHz, double maxHz, double activeThreshold)
    : minHz_(minHz), maxHz_(maxHz), threshold_(activeThreshold) {}

DetectScheduler::Clock::duration DetectScheduler::interval() const {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / targetHz()));
}

void DetectScheduler::onScore(double score, Clock::time_point now) {
    if (score > threshold_) {
        lastActive_ = now;
        if (mode_ == Mode::Idle) {
            mode_ = Mode::Active;
            nextDue_ = std::min(nextDue_, now);  // 有動靜就立刻偵測一次
        }
    } else if (mode_ == Mode::Active && now - lastActive_ > kIdleAfter) {
        mode_ = Mode::Idle;
        if (!history_.empty()) nextDue_ = history_.back() + interval();
    }
}

bool DetectScheduler::shouldDetect(Clock::time_point now) const {
    return now >= nextDue_;
}

void DetectScheduler::onDetected(Clock::time_point now) {
    history_.push_back(now);
    nextDue_ += interval();
    // 落後 (偵測太慢、跳格或剛啟動) 時不補做，從現在重新起算
    if (nextDue_ <= now) nextDue_ = now + interval();
}

double DetectScheduler::actualHz(Clock::time_point now) {
    while (!history_.empty() && now - history_.front() > kRateWindow) history_.pop_front();
    return static_cast<double>(history_.size()) / std::chrono::duration<double>(kRateWindow).count();
}
