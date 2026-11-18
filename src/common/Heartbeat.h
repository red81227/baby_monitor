#pragma once

#include <atomic>
#include <chrono>

// 執行緒的「最後進度時間」，給 watchdog 判斷是否停滯 (lock-free)
class Heartbeat {
public:
    using Clock = std::chrono::steady_clock;

    Heartbeat() { beat(); }

    void beat() { last_.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed); }

    Clock::duration age(Clock::time_point now = Clock::now()) const {
        return now - Clock::time_point(Clock::duration(last_.load(std::memory_order_relaxed)));
    }

private:
    std::atomic<Clock::rep> last_{0};
};
