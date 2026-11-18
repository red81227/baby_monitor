#include "check.h"
#include "vision/DetectScheduler.h"

#include <chrono>
#include <functional>

using namespace std::chrono;
using Clock = DetectScheduler::Clock;

namespace {

constexpr double kThreshold = 0.01;
const auto kFrame = microseconds(33333);  // 30 fps

// 以 30 fps 模擬 duration 長的影格，score(t) 決定每張影格的分數；回傳該期間的偵測次數
int simulate(DetectScheduler& s, Clock::time_point& now, Clock::duration duration,
             const std::function<double(Clock::time_point)>& score,
             Clock::time_point* firstDetect = nullptr) {
    int detections = 0;
    const auto end = now + duration;
    while (now < end) {
        s.onScore(score(now), now);
        if (s.shouldDetect(now)) {
            s.onDetected(now);
            if (detections == 0 && firstDetect) *firstDetect = now;
            ++detections;
        }
        now += kFrame;
    }
    return detections;
}

const auto still = [](Clock::time_point) { return 0.0; };
const auto moving = [](Clock::time_point) { return 0.2; };

} // namespace

int main() {
    Clock::time_point now{seconds(1000)};

    // 情境「靜止時」：約以下限頻率 (1 Hz) 偵測
    DetectScheduler s(1.0, 10.0, kThreshold);
    CHECK(s.shouldDetect(now));  // 第一張影格立即偵測
    int n = simulate(s, now, seconds(10), still);
    CHECK(s.mode() == DetectScheduler::Mode::Idle);
    CHECK(n >= 10 && n <= 11);
    CHECK_NEAR(s.actualHz(now), 1.0, 0.34);

    // 情境「開始有動靜」：1 秒內提高到上限頻率，且第一次偵測在下一張影格
    Clock::time_point first{};
    const auto activeStart = now;
    n = simulate(s, now, seconds(1), moving, &first);
    CHECK(s.mode() == DetectScheduler::Mode::Active);
    CHECK(first == activeStart);
    CHECK(n >= 9 && n <= 11);
    n = simulate(s, now, seconds(3), moving);
    CHECK(n >= 28 && n <= 31);  // 約 10 Hz (影格時間離散，平均仍接近目標)
    CHECK_NEAR(s.actualHz(now), 10.0, 0.7);
    CHECK(s.targetHz() == 10.0);

    // 情境「動靜結束」：5 秒內仍維持上限；超過 5 秒後回到下限
    n = simulate(s, now, milliseconds(4900), still);
    CHECK(s.mode() == DetectScheduler::Mode::Active);
    CHECK(n >= 46);
    simulate(s, now, milliseconds(300), still);
    CHECK(s.mode() == DetectScheduler::Mode::Idle);
    CHECK(s.targetHz() == 1.0);
    simulate(s, now, seconds(4), still);  // 讓 3 秒視窗只剩 IDLE 期間
    n = simulate(s, now, seconds(10), still);
    CHECK(n >= 9 && n <= 11);
    CHECK_NEAR(s.actualHz(now), 1.0, 0.34);

    // 中途有短暫動靜會延長 ACTIVE：計時從最後一次超過門檻起算
    simulate(s, now, milliseconds(100), moving);
    simulate(s, now, seconds(3), still);
    simulate(s, now, milliseconds(100), moving);
    simulate(s, now, seconds(4), still);
    CHECK(s.mode() == DetectScheduler::Mode::Active);
    simulate(s, now, seconds(2), still);
    CHECK(s.mode() == DetectScheduler::Mode::Idle);

    // 固定頻率：min == max 時不論有無動靜都約 5 Hz
    DetectScheduler fixed(5.0, 5.0, kThreshold);
    n = simulate(fixed, now, seconds(10), still);
    CHECK(n >= 49 && n <= 51);
    n = simulate(fixed, now, seconds(10), moving);
    CHECK(n >= 49 && n <= 51);
    CHECK_NEAR(fixed.actualHz(now), 5.0, 0.4);

    // 偵測比目標間隔慢 (影格每 300ms 才來一張，目標 10 Hz)：每張都偵測，不會累積欠帳
    DetectScheduler slow(10.0, 10.0, kThreshold);
    int detected = 0;
    for (int i = 0; i < 20; ++i) {
        slow.onScore(0.0, now);
        if (slow.shouldDetect(now)) {
            slow.onDetected(now);
            ++detected;
        }
        now += milliseconds(300);
    }
    CHECK(detected == 20);
    // 之後影格恢復 30 fps：頻率回到約 10 Hz，不會為了補欠帳連續偵測
    n = simulate(slow, now, seconds(1), still);
    CHECK(n >= 9 && n <= 11);

    // 沒有任何偵測：頻率為 0
    DetectScheduler empty(1.0, 10.0, kThreshold);
    CHECK(empty.actualHz(now) == 0.0);

    return 0;
}
