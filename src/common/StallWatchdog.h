#pragma once

#include "common/Heartbeat.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 停滯時自行結束 (BM_STALL_EXIT_SECONDS)：沒有 systemd watchdog 的環境 (例如容器) 中，
// 每秒檢查所有 Heartbeat，任一停滯超過 limit 時呼叫一次 onStall (之後不再檢查)。
// 結束程式的方式 (正常關閉、逾時強制結束) 由呼叫端的 onStall 決定。
class StallWatchdog {
public:
    struct Watched {
        std::string name;  // 錯誤訊息中的名稱，例如「擷取」
        const Heartbeat* heartbeat;
    };
    // name：停滯的部分；age：停了多久
    using StallCallback = std::function<void(const std::string& name, std::chrono::seconds age)>;

    StallWatchdog(std::vector<Watched> watched, std::chrono::seconds limit, StallCallback onStall,
                  std::chrono::milliseconds interval = std::chrono::seconds(1));
    ~StallWatchdog();

    StallWatchdog(const StallWatchdog&) = delete;
    StallWatchdog& operator=(const StallWatchdog&) = delete;

    void start();
    void stop();

    // 以指定時間檢查一次 (測試用；執行緒每個 interval 以目前時間呼叫)。觸發時回傳 true
    bool check(Heartbeat::Clock::time_point now);
    bool fired() const { return fired_; }

private:
    void loop();

    std::vector<Watched> watched_;
    std::vector<const Heartbeat*> heartbeats_;
    std::chrono::seconds limit_;
    StallCallback onStall_;
    std::chrono::milliseconds interval_;

    bool fired_ = false;  // 只在檢查的執行緒 (或測試中呼叫 check 的執行緒) 存取
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread worker_;
};
