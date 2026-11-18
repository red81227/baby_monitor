#include "vision/CaptureThread.h"

#include <algorithm>
#include <iostream>

std::chrono::milliseconds reconnectBackoff(unsigned attempt) {
    constexpr long long kBaseMs = 250;
    constexpr long long kMaxMs = 5000;
    if (attempt >= 5) return std::chrono::milliseconds(kMaxMs);  // 250 << 5 = 8000 > 5000
    return std::chrono::milliseconds(std::min(kMaxMs, kBaseMs << attempt));
}

CaptureThread::CaptureThread(FrameSource& source, LatestSlot<RawFrame>& frames, MonitorState& state,
                             Heartbeat& heartbeat)
    : source_(source), frames_(frames), state_(state), heartbeat_(heartbeat) {}

CaptureThread::~CaptureThread() {
    stop();
}

void CaptureThread::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    worker_ = std::thread(&CaptureThread::run, this);
}

void CaptureThread::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    source_.close();
}

bool CaptureThread::sleepFor(std::chrono::milliseconds d) {
    std::unique_lock<std::mutex> lock(mutex_);
    return !cv_.wait_for(lock, d, [this] { return !running_; });
}

void CaptureThread::run() {
    const auto isRunning = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    };

    bool connected = true;
    unsigned attempt = 0;
    while (isRunning()) {
        if (!connected) {
            heartbeat_.beat();  // 重連嘗試也算進度
            if (!sleepFor(reconnectBackoff(attempt))) break;
            ++attempt;
            if (!source_.open()) continue;
            const auto d = source_.describe();
            std::cout << "[INFO] 攝影機已重新連線: " << d.device << " " << d.fourcc << " " << d.width << "x"
                      << d.height << "\n";
            connected = true;
            attempt = 0;
        }

        auto frame = source_.grab();
        if (!frame) {
            if (!isRunning()) break;
            std::cerr << "[WARN] 攝影機 " << source_.describe().device << " 中斷，嘗試重新連線\n";
            state_.setCameraConnected(false);
            source_.close();
            connected = false;
            continue;
        }

        frame->seq = seq_.next();
        auto shared = std::make_shared<const RawFrame>(std::move(*frame));
        state_.updateFrame(shared);
        frames_.publish(std::move(shared));
        heartbeat_.beat();
    }
}
