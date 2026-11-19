#include "situation/SituationMonitor.h"

SituationMonitor::SituationMonitor(RulesStore& rules, ObservationLog* log, TriggerCallback onTrigger)
    : store_(rules), log_(log), onTrigger_(std::move(onTrigger)), engine_(rules.current()) {}

std::optional<cv::Rect2f> SituationMonitor::bedRoi() const {
    const auto rules = store_.current();
    return rules ? rules->params.bedRoi : std::nullopt;
}

void SituationMonitor::onAnalysis(uint64_t seq, int64_t tCaptureUs, double activity,
                                  const DetectionResult* detection) {
    auto rules = store_.current();
    if (rules != engine_.rules()) engine_.setRules(rules);
    const SignalParams params = rules ? rules->params : SignalParams{};

    const SignalFrame frame = extractor_.update(seq, tCaptureUs, activity, detection, params);
    if (log_) log_->append(frame);
    const auto triggers = engine_.onSignals(frame);

    {
        std::lock_guard<std::mutex> lock(mu_);
        last_ = frame;
        active_.clear();
        if (rules) {
            for (const auto& s : rules->situations) {
                if (const auto* st = engine_.state(s.id)) active_[s.id] = ActiveState{st->active, st->inactiveSinceUs};
            }
        }
    }
    if (onTrigger_) {
        for (const auto& t : triggers) onTrigger_(t);
    }
}

std::map<std::string, SituationMonitor::ActiveState> SituationMonitor::activeStates() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
}

std::optional<SignalFrame> SituationMonitor::lastSignals() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_;
}
