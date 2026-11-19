#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

// 執行緒的「最後進度時間」，給 watchdog 判斷是否停滯 (lock-free)
class Heartbeat {
public:
    using Clock = std::chrono::steady_clock;

    Heartbeat() { beat(); }

    void beat(Clock::time_point now = Clock::now()) {
        last_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    }

    Clock::duration age(Clock::time_point now = Clock::now()) const {
        return now - Clock::time_point(Clock::duration(last_.load(std::memory_order_relaxed)));
    }

private:
    std::atomic<Clock::rep> last_{0};
};

// 第一個距今時間不小於 period 的 heartbeat 索引；全部都在 period 內時為 nullopt
// (systemd watchdog 與停滯結束共用的新鮮度判斷)
template <class Rep, class Period>
std::optional<std::size_t> firstStaleHeartbeat(const std::vector<const Heartbeat*>& heartbeats,
                                               std::chrono::duration<Rep, Period> period,
                                               Heartbeat::Clock::time_point now = Heartbeat::Clock::now()) {
    for (std::size_t i = 0; i < heartbeats.size(); ++i) {
        if (heartbeats[i]->age(now) >= period) return i;
    }
    return std::nullopt;
}
