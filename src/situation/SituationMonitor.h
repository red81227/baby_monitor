#pragma once

#include "situation/ObservationLog.h"
#include "situation/RuleEngine.h"
#include "situation/RulesStore.h"
#include "situation/SignalExtractor.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// 第一段：每張分析影格 → SignalExtractor → 觀測紀錄 → RuleEngine → 觸發。
// 由分析執行緒呼叫 onAnalysis()；規則換過 (RulesStore::current() 指標改變) 時自動套用。
// 狀況的成立狀態另存一份快照，供 HTTP 執行緒與警報管理查詢。
class SituationMonitor {
public:
    using TriggerCallback = std::function<void(const Trigger&)>;

    struct ActiveState {
        bool active = false;
        std::optional<int64_t> inactiveSinceUs;
    };

    SituationMonitor(RulesStore& rules, ObservationLog* log, TriggerCallback onTrigger);

    // detection 為空表示這張影格沒有執行人物偵測
    void onAnalysis(uint64_t seq, int64_t tCaptureUs, double activity, const DetectionResult* detection);

    // 目前規則的嬰兒床區域 (移動分數只計算區域內)
    std::optional<cv::Rect2f> bedRoi() const;

    std::map<std::string, ActiveState> activeStates() const;
    std::optional<SignalFrame> lastSignals() const;

private:
    RulesStore& store_;
    ObservationLog* log_;
    TriggerCallback onTrigger_;

    SignalExtractor extractor_;
    RuleEngine engine_;

    mutable std::mutex mu_;
    std::map<std::string, ActiveState> active_;
    std::optional<SignalFrame> last_;
};
