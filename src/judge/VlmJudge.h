#pragma once

#include "events/RingBuffer.h"
#include "situation/Signals.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// 第二段：把事件片段的取樣影格、觸發時的訊號與 prompt 送到本機 VLM 服務 (OpenAI 相容的
// /v1/chat/completions)，解析出「是否提醒」與「原因說明」。
// 判定在獨立的 worker 執行緒進行，不阻塞擷取、偵測與規則判斷；等待中的判定數量有上限。
class VlmJudge {
public:
    struct Options {
        std::string url;    // BM_VLM_URL；空字串 = 停用
        std::string model;  // BM_VLM_MODEL
        size_t queueLimit = 4;
        int maxImages = 6;
        int maxSide = 640;
        std::chrono::milliseconds healthInterval{30'000};
    };

    struct Result {
        enum class Status { Ok, Timeout, Unavailable, BadReply, QueueFull };
        Status status = Status::Unavailable;
        bool alert = false;      // status == Ok 時有效
        std::string cause;       // status == Ok 時為 VLM 的說明
        std::string error;       // 失敗原因
        std::string model;
        int64_t latencyMs = 0;
        std::string rawReply;    // VLM 回覆的原始內容 (有收到回覆時)
    };
    using Callback = std::function<void(const Result&)>;

    struct Job {
        std::string situationId;
        std::string prompt;  // 狀況的 prompt 檔內容
        SignalFrame signals;
        std::vector<RingBuffer::FramePtr> frames;  // 事件片段 (依時間排序)
        int64_t preUs = 0;   // 片段範圍 (說明用)
        int64_t postUs = 0;
        // 巡檢：frames 是最近的影格 (相隔 1 秒)；排在一般判定之前，不占 queueLimit
        // (由 PatrolScheduler 保證同一巡檢最多一筆在佇列中)
        bool patrol = false;
        std::chrono::milliseconds timeout{20'000};  // 從 submit 起算
        Callback onDone;
    };

    explicit VlmJudge(Options options);
    ~VlmJudge();

    void start();
    void stop();

    // 送出判定；停用、佇列已滿時直接以對應的 status 呼叫 onDone (在呼叫端執行緒)。
    // 巡檢工作排在所有一般判定之前 (巡檢之間依送出順序)
    void submit(Job job);

    bool enabled() const { return !opts_.url.empty(); }
    bool available() const { return available_.load(); }
    std::string lastHealthError() const;
    const std::string& model() const { return opts_.model; }

    static const char* toString(Result::Status s);

    // 以下為內部步驟，公開以便測試
    static std::string chatUrl(const std::string& base);
    static std::string modelsUrl(const std::string& base);
    static std::vector<RingBuffer::FramePtr> sample(const std::vector<RingBuffer::FramePtr>& frames, int maxImages);
    // 從 VLM 回覆的文字中取出第一個 JSON 物件並解析 alert / cause；失敗時回傳 false 並寫入 error
    static bool parseVerdict(const std::string& content, bool& alert, std::string& cause, std::string& error);

private:
    struct Queued {
        Job job;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point submitted;
    };

    void run();
    void healthLoop();
    Result judge(const Queued& q);
    std::string buildRequest(const Job& job) const;

    Options opts_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Queued> queue_;
    bool stopping_ = false;
    std::thread worker_;
    std::thread health_;

    std::atomic<bool> abort_{false};  // 關閉時中斷進行中的 HTTP 請求
    std::atomic<bool> available_{false};
    mutable std::mutex healthMu_;
    std::string healthError_;
};
