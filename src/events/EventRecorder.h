#pragma once

#include "events/RingBuffer.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// 狀況觸發時的事件片段：等到片段結束時間 (觸發時間 + 後段) 後，從環形緩衝取出
// [觸發時間 - 前段, 觸發時間 + 後段] 的影格，寫成 BM_EVENT_DIR/<id>/ 目錄：
//   000001.jpg ...  原始 JPEG (不重新編碼)
//   meta.json       片段 id、狀況、觸發時間、規則版本，以及每張影格的檔名、seq、擷取時間
// 總大小超過上限時從最舊的片段開始刪除。
class EventRecorder {
public:
    struct Request {
        std::string situationId;
        int64_t triggerUs = 0;  // 觸發的擷取時間 (monotonic 微秒)
        int64_t preUs = 0;
        int64_t postUs = 0;
        int rulesVersion = 0;
        std::string rulesHash;
    };

    struct Clip {
        std::string id;
        Request request;
        std::vector<RingBuffer::FramePtr> frames;
        bool written = false;  // 是否成功寫入磁碟
    };
    using ClipCallback = std::function<void(const Clip&)>;

    // 攝影機停住時最多再等多久 (超過片段結束時間) 就用現有的影格產生片段
    static constexpr auto kStallGrace = std::chrono::seconds(5);

    EventRecorder(RingBuffer& ring, std::string dir, int64_t maxBytes);
    ~EventRecorder();

    void start();
    void stop();  // 尚未完成的片段會立即以現有影格產生

    // 排入一個片段，立即回傳片段 id；片段完成時呼叫 onDone (在 recorder 的執行緒)
    std::string request(const Request& req, ClipCallback onDone);

    // 依 id 讀取片段 (給 API)：id 格式不符或不存在時回傳 nullopt
    static bool isValidId(const std::string& id);
    std::optional<std::string> readMeta(const std::string& id) const;
    std::optional<std::string> readFrame(const std::string& id, const std::string& file) const;

    const std::string& dir() const { return dir_; }

    // 測試用：立即處理到期的片段 (now 為 monotonic 微秒)
    void processDue(int64_t nowUs, bool force = false);

private:
    struct Pending {
        std::string id;
        Request request;
        ClipCallback onDone;
        std::chrono::steady_clock::time_point deadline;  // 攝影機停住時的最後期限
    };

    void finish(Pending& p);
    bool writeClip(const Clip& clip);
    void enforceLimit(const std::string& keep);

    RingBuffer& ring_;
    std::string dir_;
    int64_t maxBytes_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Pending> pending_;
    bool stopping_ = false;
    std::map<std::string, int> counters_;  // 同一秒同一狀況的序號
    std::thread worker_;
};
