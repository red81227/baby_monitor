#include "common/StallWatchdog.h"

StallWatchdog::StallWatchdog(std::vector<Watched> watched, std::chrono::seconds limit, StallCallback onStall,
                             std::chrono::milliseconds interval)
    : watched_(std::move(watched)), limit_(limit), onStall_(std::move(onStall)), interval_(interval) {
    for (const auto& w : watched_) heartbeats_.push_back(w.heartbeat);
}

StallWatchdog::~StallWatchdog() { stop(); }

void StallWatchdog::start() {
    if (limit_.count() <= 0 || worker_.joinable()) return;
    worker_ = std::thread(&StallWatchdog::loop, this);
}

void StallWatchdog::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool StallWatchdog::check(Heartbeat::Clock::time_point now) {
    if (fired_ || limit_.count() <= 0) return false;
    const auto stale = firstStaleHeartbeat(heartbeats_, limit_, now);
    if (!stale) return false;
    fired_ = true;
    const auto& w = watched_[*stale];
    if (onStall_) onStall_(w.name, std::chrono::duration_cast<std::chrono::seconds>(w.heartbeat->age(now)));
    return true;
}

void StallWatchdog::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (cv_.wait_for(lock, interval_, [this] { return stopping_; })) break;
        lock.unlock();
        check(Heartbeat::Clock::now());  // onStall 可能觸發關閉流程，不持有鎖
        lock.lock();
    }
}
