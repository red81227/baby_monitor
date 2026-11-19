#include "situation/RuleEngine.h"

namespace {

bool sameCondition(const Condition& a, const Condition& b) {
    return a.signal == b.signal && a.op == b.op && a.value == b.value;
}

} // namespace

RuleEngine::RuleEngine(std::shared_ptr<const Rules> rules) {
    setRules(std::move(rules));
}

void RuleEngine::setRules(std::shared_ptr<const Rules> rules) {
    std::map<std::string, SituationState> next;
    if (rules) {
        const Rules* old = rules_.get();
        for (const auto& s : rules->situations) {
            SituationState st;
            st.holdingSinceUs.resize(s.when.size());
            st.gapSinceUs.resize(s.when.size());
            const auto it = states_.find(s.id);
            const Situation* prev = old ? old->find(s.id) : nullptr;
            if (it != states_.end() && prev) {
                st.active = it->second.active;
                st.lastTriggerUs = it->second.lastTriggerUs;
                st.inactiveSinceUs = it->second.inactiveSinceUs;
                for (size_t i = 0; i < s.when.size() && i < prev->when.size(); ++i) {
                    if (sameCondition(s.when[i], prev->when[i]) && i < it->second.holdingSinceUs.size()) {
                        st.holdingSinceUs[i] = it->second.holdingSinceUs[i];
                        if (i < it->second.gapSinceUs.size()) st.gapSinceUs[i] = it->second.gapSinceUs[i];
                    }
                }
            }
            next.emplace(s.id, std::move(st));
        }
    }
    rules_ = std::move(rules);
    states_ = std::move(next);
}

std::vector<Trigger> RuleEngine::onSignals(const SignalFrame& frame) {
    std::vector<Trigger> out;
    if (!rules_) return out;
    const int64_t t = frame.tCaptureUs;

    for (const auto& s : rules_->situations) {
        SituationState& st = states_[s.id];
        st.holdingSinceUs.resize(s.when.size());
        st.gapSinceUs.resize(s.when.size());

        bool all = s.enabled;
        for (size_t i = 0; i < s.when.size(); ++i) {
            const Condition& c = s.when[i];
            const auto& v = frame[c.signal];
            auto& since = st.holdingSinceUs[i];
            auto& gap = st.gapSinceUs[i];
            if (s.enabled && v && c.holds(*v)) {
                gap.reset();
                if (!since) since = t;
            } else {
                // 不成立或未知：計時中且未超過 grace 時視為短暫中斷 (計時不歸零、已成立的條件仍成立)，
                // 否則連續計時歸零
                if (s.enabled && since && c.graceUs > 0) {
                    if (!gap) gap = t;
                    if (t - *gap >= c.graceUs) {
                        since.reset();
                        gap.reset();
                    }
                } else {
                    since.reset();
                    gap.reset();
                }
                if (!since) {
                    all = false;
                    continue;
                }
            }
            if (t - *since < c.forUs) all = false;
        }

        const bool wasActive = st.active;
        st.active = all;
        if (wasActive && !all) st.inactiveSinceUs = t;
        if (!all) continue;

        // 變成成立時觸發；持續成立時每隔 cooldown 再觸發 (cooldown 為 0 時不重複觸發)
        const bool rising = !wasActive;
        const bool repeat = wasActive && s.cooldownUs > 0 && st.lastTriggerUs && t - *st.lastTriggerUs >= s.cooldownUs;
        if (rising || repeat) {
            st.lastTriggerUs = t;
            out.push_back(Trigger{s.id, s.severity, s.route, s.shadow, frame.seq, t, frame, rules_->version,
                                  rules_->hash});
        }
    }
    return out;
}

const RuleEngine::SituationState* RuleEngine::state(const std::string& id) const {
    const auto it = states_.find(id);
    return it == states_.end() ? nullptr : &it->second;
}
