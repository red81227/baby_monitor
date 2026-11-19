#pragma once

#include "situation/Rules.h"
#include "situation/Signals.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 一次狀況觸發
struct Trigger {
    std::string situationId;
    Severity severity;
    Route route;
    bool shadow;
    uint64_t seq;        // 觸發當下的影格
    int64_t tUs;         // 觸發時間 (擷取時間，monotonic 微秒)
    SignalFrame signals; // 觸發當下的訊號值
    int rulesVersion;
    std::string rulesHash;
};

// 依規則判斷狀況是否成立與何時觸發。純邏輯：所有時間都取自輸入 SignalFrame 的擷取時間，
// 不讀系統時鐘，因此即時服務與離線重播 (bm_replay_rules) 的結果相同。非執行緒安全。
class RuleEngine {
public:
    struct SituationState {
        bool active = false;                    // 目前是否成立
        std::optional<int64_t> lastTriggerUs;   // 最近一次觸發
        std::optional<int64_t> inactiveSinceUs; // 最近一次由成立變成不成立的時間
        std::vector<std::optional<int64_t>> holdingSinceUs;  // 每個條件開始連續成立的時間
        std::vector<std::optional<int64_t>> gapSinceUs;      // 每個條件目前這段中斷 (grace 內) 的開始時間
    };

    explicit RuleEngine(std::shared_ptr<const Rules> rules = nullptr);

    // 換成新規則：id 相同的狀況保留觸發與成立狀態；內容相同的條件保留連續計時，其餘重新開始
    void setRules(std::shared_ptr<const Rules> rules);
    const std::shared_ptr<const Rules>& rules() const { return rules_; }

    std::vector<Trigger> onSignals(const SignalFrame& frame);

    const SituationState* state(const std::string& id) const;

private:
    std::shared_ptr<const Rules> rules_;
    std::map<std::string, SituationState> states_;
};
