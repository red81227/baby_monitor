#pragma once

#include "common/Heartbeat.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 直接對 NOTIFY_SOCKET 送 datagram 的 systemd 通知 (不依賴 libsystemd)。
// - 沒有 NOTIFY_SOCKET：停用，所有呼叫都是 no-op，不輸出任何訊息
// - 支援一般路徑與以 '@' 開頭的抽象 namespace
// - 有 WATCHDOG_USEC (且 WATCHDOG_PID 未設定或等於本程式) 時，startWatchdog 以週期的一半檢查
//   所有 Heartbeat，全部都在週期內才送 WATCHDOG=1
class SystemdNotifier {
public:
    SystemdNotifier();  // 從環境變數讀取設定
    ~SystemdNotifier();

    SystemdNotifier(const SystemdNotifier&) = delete;
    SystemdNotifier& operator=(const SystemdNotifier&) = delete;

    bool enabled() const { return fd_ >= 0; }
    std::chrono::microseconds watchdogPeriod() const { return watchdog_; }

    bool notify(const std::string& message);

    void startWatchdog(std::vector<const Heartbeat*> heartbeats);
    void stop();

private:
    void watchdogLoop();

    int fd_ = -1;
    std::vector<char> addr_;  // sockaddr_un 的實際內容
    std::chrono::microseconds watchdog_{0};

    std::vector<const Heartbeat*> heartbeats_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread worker_;
};

// 所有 heartbeat 的距今時間都小於 period 時為 true
bool allHeartbeatsFresh(const std::vector<const Heartbeat*>& heartbeats, std::chrono::microseconds period,
                        Heartbeat::Clock::time_point now = Heartbeat::Clock::now());
