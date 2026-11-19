#include "check.h"
#include "common/StallWatchdog.h"
#include "common/SystemdNotifier.h"

#include <atomic>
#include <string>
#include <thread>

using namespace std::chrono;

int main() {
    using Clock = Heartbeat::Clock;
    const auto t0 = Clock::now();

    // 共用的新鮮度判斷
    {
        Heartbeat a, b;
        a.beat(t0);
        b.beat(t0 - seconds(30));
        CHECK(!firstStaleHeartbeat({&a, &b}, seconds(60), t0).has_value());
        CHECK(firstStaleHeartbeat({&a, &b}, seconds(30), t0) == 1u);
        CHECK(allHeartbeatsFresh({&a, &b}, duration_cast<microseconds>(seconds(31)), t0));
        CHECK(!allHeartbeatsFresh({&a, &b}, duration_cast<microseconds>(seconds(30)), t0));
    }

    // 持續 beat：不觸發
    {
        Heartbeat capture, analysis;
        int calls = 0;
        StallWatchdog wd({{"擷取", &capture}, {"分析", &analysis}}, seconds(60),
                         [&](const std::string&, seconds) { ++calls; });
        for (int s = 0; s <= 600; ++s) {
            const auto now = t0 + seconds(s);
            capture.beat(now);
            if (s % 30 == 0) analysis.beat(now);  // 每 30 秒一次也在 60 秒內
            CHECK(!wd.check(now));
        }
        CHECK(calls == 0 && !wd.fired());
    }

    // 分析停滯：超過時間觸發一次，指出停滯的部分與時間；之後不再觸發
    {
        Heartbeat capture, analysis;
        capture.beat(t0);
        analysis.beat(t0);
        int calls = 0;
        std::string stalled;
        seconds stalledFor{0};
        StallWatchdog wd({{"擷取", &capture}, {"分析", &analysis}}, seconds(60),
                         [&](const std::string& name, seconds age) {
                             ++calls;
                             stalled = name;
                             stalledFor = age;
                         });
        for (int s = 1; s <= 59; ++s) {
            capture.beat(t0 + seconds(s));
            CHECK(!wd.check(t0 + seconds(s)));
        }
        capture.beat(t0 + seconds(60));
        CHECK(wd.check(t0 + seconds(60)));
        CHECK(calls == 1 && stalled == "分析" && stalledFor == seconds(60) && wd.fired());
        for (int s = 61; s <= 120; ++s) CHECK(!wd.check(t0 + seconds(s)));
        analysis.beat(t0 + seconds(130));  // 恢復後也不會再觸發
        CHECK(!wd.check(t0 + seconds(300)));
        CHECK(calls == 1);
    }

    // 停用 (0 秒)：不檢查，start 不建立執行緒
    {
        Heartbeat hb;
        hb.beat(t0 - hours(1));
        int calls = 0;
        StallWatchdog wd({{"擷取", &hb}}, seconds(0), [&](const std::string&, seconds) { ++calls; });
        wd.start();
        CHECK(!wd.check(t0));
        wd.stop();
        CHECK(calls == 0);
    }

    // 執行緒：以實際時間每個 interval 檢查，停滯時呼叫一次；stop 立即返回
    {
        Heartbeat hb;
        hb.beat(Clock::now() - seconds(120));
        std::atomic<int> calls{0};
        StallWatchdog wd({{"擷取", &hb}}, seconds(60), [&](const std::string&, seconds) { ++calls; },
                         milliseconds(20));
        wd.start();
        std::this_thread::sleep_for(milliseconds(200));
        CHECK(calls == 1);
        const auto s0 = Clock::now();
        wd.stop();
        CHECK(Clock::now() - s0 < milliseconds(100));
    }

    return 0;
}
