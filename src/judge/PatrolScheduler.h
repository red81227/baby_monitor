#pragma once

#include "events/RingBuffer.h"
#include "judge/VlmJudge.h"
#include "situation/RuleEngine.h"
#include "situation/Rules.h"
#include "situation/Signals.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// 巡檢紀錄：每次巡檢結果一行 JSON，依當地日期分檔 patrols-YYYYMMDD.jsonl，保留 14 天
class PatrolLog {
public:
    static constexpr int kRetentionDays = 14;
    explicit PatrolLog(std::string dir);
    void append(const std::string& json);

private:
    void removeOld(const std::string& today);

    std::string dir_;
    std::mutex mu_;
    std::string lastDate_;
};

// VLM 定期巡檢 (design D11)：每個巡檢依 every 從環形緩衝取最近的影格交給 VlmJudge，
// 連續 confirm 次回覆需要提醒時觸發；連續失敗或太久沒有成功結果時視為停止。
// 只會讓提醒發生，沒有撤回提醒的路徑。所有方法皆執行緒安全。
class PatrolScheduler {
public:
    struct Event {
        enum class Type { Trigger, Stalled, Recovered };
        Type type = Type::Trigger;
        std::string patrolId;
        Severity severity = Severity::Warning;
        bool shadow = false;
        int64_t tUs = 0;               // 觸發：最後一次判定所用最新影格的擷取時間；停止 / 恢復：當下時間
        SignalFrame signals;
        int64_t clipPreUs = 0;
        int64_t clipPostUs = 0;
        int rulesVersion = 0;
        std::string rulesHash;
        std::optional<VlmJudge::Result> result;  // 觸發：最後一次判定；恢復：成功的那一次
        std::string reason;                      // 停止的原因
    };

    struct Status {
        std::string id;
        Severity severity = Severity::Warning;
        int64_t everyUs = 0;
        int confirm = 1;
        bool shadow = false;
        bool enabled = true;
        bool paused = false;              // 前提條件不成立
        bool stalled = false;
        bool inFlight = false;
        int64_t lastOkEpochMs = 0;        // 最後一次成功的時間 (0 = 尚未成功)
        std::optional<bool> lastAlert;    // 最近一次成功結果是否需要提醒
        std::string lastCause;
        std::string lastError;            // 最近一次失敗的原因 (成功後清除)
        std::string stallReason;
        int positives = 0;                // 目前連續回覆需要提醒的次數
    };

    struct Deps {
        std::function<std::shared_ptr<const Rules>()> rules;
        std::function<std::optional<SignalFrame>()> signals;  // 最新的訊號 (前提條件與送給 VLM 的說明)
        RingBuffer* ring = nullptr;
        std::function<void(VlmJudge::Job)> submit;            // 空 = VLM 未設定
        std::function<void(const Event&)> onEvent;            // 觸發、停止、恢復 (交給 AlertManager)
        std::function<int64_t()> nowMonoUs;                   // 預設 steady_clock
        std::function<int64_t()> nowEpochMs;                  // 預設 system_clock
        PatrolLog* log = nullptr;
    };

    static constexpr int kStallFailures = 3;      // 連續失敗幾次視為停止
    static constexpr int kStallEveryMultiple = 3; // 前提條件成立期間超過幾倍 every 沒有成功視為停止
    static constexpr int64_t kFrameSpacingUs = 1'000'000;
    static constexpr int64_t kNoFrameRetryUs = 5'000'000;  // 沒有影格時多久後重試

    explicit PatrolScheduler(Deps deps);
    ~PatrolScheduler();

    void start();  // 背景執行緒每 200ms 呼叫一次 tick()
    void stop();

    // 依目前時間處理到期的巡檢 (測試可直接呼叫)
    void tick();

    std::vector<Status> status() const;

    // 從依時間排序的影格中，以最新一張為準往前每隔 spacingUs 取一張，共 n 張 (依時間排序)
    static std::vector<RingBuffer::FramePtr> pickFrames(const std::vector<RingBuffer::FramePtr>& frames, int n,
                                                        int64_t spacingUs);

private:
    struct State {
        Status status;
        int64_t nextDueUs = 0;
        int failures = 0;
        int64_t noSuccessUs = 0;        // 前提條件成立期間累計多久沒有成功結果
        std::optional<int64_t> lastTickUs;
        bool active = false;            // 已確認並觸發 (持續回覆需要提醒時依冷卻重複觸發)
        std::optional<int64_t> lastTriggerUs;
        uint64_t generation = 0;        // 規則換過後，舊的判定結果不再套用
    };

    void syncRules(const std::shared_ptr<const Rules>& rules, int64_t now);
    bool whileHolds(const Patrol& p, int64_t now);
    void onResult(const std::string& id, uint64_t generation, int64_t frameUs, const SignalFrame& signals,
                  const VlmJudge::Result& res);
    // 失敗 / 成功時更新停止狀態 (持有 mu_)；回傳要送出的事件
    std::optional<Event> failLocked(State& st, const Patrol& p, const std::string& error, int64_t now);
    void logLocked(const Patrol& p, const VlmJudge::Result& res, int positives);
    Event makeEvent(Event::Type type, const Patrol& p, int64_t tUs) const;

    Deps deps_;
    mutable std::mutex mu_;
    std::shared_ptr<const Rules> rules_;
    std::map<std::string, State> states_;
    RuleEngine whileEngine_;  // 前提條件：每個巡檢的 while 視為一個狀況
    std::shared_ptr<const Rules> whileRules_;
    uint64_t lastSignalSeq_ = 0;
    uint64_t generationCounter_ = 0;
    bool warnedNoVlm_ = false;

    std::mutex threadMu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread worker_;
};
