#pragma once

#include "vision/Frame.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// 主碼流環形緩衝：保留最近 durationUs 的影格 (原始 JPEG，以共用指標保存，不複製、不重新編碼)，
// 以擷取時間索引。超過時間的影格在 push 時捨棄，記憶體用量不隨執行時間增加。執行緒安全。
class RingBuffer {
public:
    using FramePtr = std::shared_ptr<const RawFrame>;

    explicit RingBuffer(int64_t durationUs) : durationUs_(durationUs) {}

    void push(FramePtr frame) {
        std::lock_guard<std::mutex> lock(mu_);
        const int64_t t = toMicros(frame->tCapture);
        frames_.push_back(std::move(frame));
        while (!frames_.empty() && t - toMicros(frames_.front()->tCapture) > durationUs_) frames_.pop_front();
    }

    // 擷取時間落在 [fromUs, toUs] 的影格，依時間排序；fromUs 早於緩衝範圍時從最早的影格開始
    std::vector<FramePtr> range(int64_t fromUs, int64_t toUs) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<FramePtr> out;
        for (const auto& f : frames_) {
            const int64_t t = toMicros(f->tCapture);
            if (t >= fromUs && t <= toUs) out.push_back(f);
        }
        return out;
    }

    // 最新一張影格的擷取時間；緩衝為空時為 nullopt
    std::optional<int64_t> newestUs() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (frames_.empty()) return std::nullopt;
        return toMicros(frames_.back()->tCapture);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return frames_.size();
    }

    int64_t durationUs() const { return durationUs_; }

private:
    const int64_t durationUs_;
    mutable std::mutex mu_;
    std::deque<FramePtr> frames_;
};
