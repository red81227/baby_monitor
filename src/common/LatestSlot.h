#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

// 只保留最新值的單槽 (latest-wins)：
// - publish 覆蓋舊值並喚醒所有等待者；讀取端拿到的是共用指標，不複製內容
// - 每次 publish 版本號 +1 (從 1 開始)；讀取端用版本號判斷有沒有更新的值
// - close 之後 waitNewerThan 立刻回傳 (value 為 nullptr)
template <class T>
class LatestSlot {
public:
    using Ptr = std::shared_ptr<const T>;
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Ptr value;             // 逾時或已關閉時為 nullptr
        uint64_t version = 0;  // value 對應的版本；尚未有值時為 0
    };

    void publish(Ptr value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_ = std::move(value);
            ++version_;
        }
        cv_.notify_all();
    }

    // 目前的值 (不等待)
    Entry latest() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {value_, version_};
    }

    // 等到版本 > version 的值出現、deadline 到期或 close 為止
    Entry waitNewerThan(uint64_t version, Clock::time_point deadline) const {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool ready = cv_.wait_until(lock, deadline, [&] { return closed_ || version_ > version; });
        if (!ready || closed_) return {nullptr, version_};
        return {value_, version_};
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    Ptr value_;
    uint64_t version_ = 0;
    bool closed_ = false;
};
