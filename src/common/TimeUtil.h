#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

inline int64_t nowMonoUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 把 monotonic 時間 (擷取時間) 換算成 Unix 毫秒 (只用於顯示與紀錄)
inline int64_t monoUsToEpochMs(int64_t monoUs) {
    return nowEpochMs() - (nowMonoUs() - monoUs) / 1000;
}

// 當地時間，格式依 strftime (例如 "%Y%m%d-%H%M%S")
inline std::string formatLocalTime(int64_t epochMs, const char* fmt) {
    const std::time_t sec = static_cast<std::time_t>(epochMs / 1000);
    std::tm tm{};
    localtime_r(&sec, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}
