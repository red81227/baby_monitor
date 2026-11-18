#pragma once

#include <chrono>
#include <deque>

// 依移動分數決定人物偵測頻率 (時間由呼叫端注入，方便測試)：
//   IDLE (minHz) --分數 > 門檻--> ACTIVE (maxHz)，立即安排一次偵測
//   ACTIVE --連續 kIdleAfter 都沒有超過門檻--> IDLE
// 偵測時間以「預定時間 += 間隔」推進，影格時間離散時平均頻率仍接近目標。
class DetectScheduler {
public:
    using Clock = std::chrono::steady_clock;
    enum class Mode { Idle, Active };

    static constexpr std::chrono::seconds kIdleAfter{5};
    static constexpr std::chrono::seconds kRateWindow{3};

    DetectScheduler(double minHz, double maxHz, double activeThreshold);

    // 每張分析影格呼叫一次
    void onScore(double score, Clock::time_point now);
    // 現在是否該執行人物偵測
    bool shouldDetect(Clock::time_point now) const;
    // 完成一次偵測後呼叫
    void onDetected(Clock::time_point now);

    Mode mode() const { return mode_; }
    double targetHz() const { return mode_ == Mode::Active ? maxHz_ : minHz_; }
    // 最近 kRateWindow 內實際完成的偵測頻率
    double actualHz(Clock::time_point now);

private:
    Clock::duration interval() const;

    double minHz_;
    double maxHz_;
    double threshold_;
    Mode mode_ = Mode::Idle;
    Clock::time_point lastActive_{};
    Clock::time_point nextDue_{};  // 預設為 epoch：第一張影格立即偵測
    std::deque<Clock::time_point> history_;
};
