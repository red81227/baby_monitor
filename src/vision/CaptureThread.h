#pragma once

#include "common/Heartbeat.h"
#include "common/LatestSlot.h"
#include "models/MonitorState.h"
#include "vision/Frame.h"
#include "vision/FrameSource.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// 第 attempt 次 (從 0 起) 重連前的等待時間：250ms、500ms、1s、2s、4s、5s、5s…
std::chrono::milliseconds reconnectBackoff(unsigned attempt);

// 影格序號：從 1 開始，每張 +1；重連不重設
class SeqCounter {
public:
    uint64_t next() { return ++last_; }
    uint64_t last() const { return last_; }

private:
    uint64_t last_ = 0;
};

// 擷取執行緒：FrameSource → 指定 seq → 發布到 LatestSlot 與 MonitorState。
// 攝影機中斷時回報 cameraConnected=false 並以退避間隔重開同一個裝置；stop() 可中斷等待。
class CaptureThread {
public:
    using FrameSink = std::function<void(const std::shared_ptr<const RawFrame>&)>;

    // source 必須已經 open() 成功 (啟動時開不了攝影機由 main 處理)
    // sink：每張影格額外送一份 (例如環形緩衝)；可為空
    CaptureThread(FrameSource& source, LatestSlot<RawFrame>& frames, MonitorState& state, Heartbeat& heartbeat,
                  FrameSink sink = nullptr);
    ~CaptureThread();

    void start();
    void stop();

private:
    void run();
    bool sleepFor(std::chrono::milliseconds d);  // 被 stop() 中斷時回傳 false

    FrameSource& source_;
    LatestSlot<RawFrame>& frames_;
    MonitorState& state_;
    Heartbeat& heartbeat_;
    FrameSink sink_;
    SeqCounter seq_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    std::thread worker_;
};
