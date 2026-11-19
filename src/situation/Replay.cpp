#include "situation/Replay.h"

#include "situation/RuleEngine.h"

#include <cstdio>
#include <ctime>
#include <map>
#include <memory>

std::vector<ReplayHit> replayRules(const Rules& rules, const std::vector<ObservationLog::Session>& sessions) {
    auto shared = std::make_shared<const Rules>(rules);
    std::vector<ReplayHit> hits;
    for (const auto& session : sessions) {
        if (session.frames.empty()) continue;
        RuleEngine engine(shared);
        const int64_t start = session.frames.front().tCaptureUs;
        for (const auto& frame : session.frames) {
            for (const auto& t : engine.onSignals(frame)) {
                hits.push_back(ReplayHit{t.situationId, t.shadow, t.seq, t.tUs,
                                         session.epochMs + (t.tUs - session.monoUs) / 1000, t.tUs - start});
            }
        }
    }
    return hits;
}

std::string formatReplay(const Rules& rules, const std::vector<ReplayHit>& hits, size_t frameCount) {
    std::string out;
    char buf[160];
    for (const auto& h : hits) {
        const std::time_t sec = static_cast<std::time_t>(h.epochMs / 1000);
        std::tm tm{};
        localtime_r(&sec, &tm);
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        std::snprintf(buf, sizeof(buf), "%s.%03d  +%9.1fs  seq=%-8llu %s%s\n", when,
                      static_cast<int>(((h.epochMs % 1000) + 1000) % 1000), h.sinceStartUs / 1e6,
                      static_cast<unsigned long long>(h.seq), h.situationId.c_str(), h.shadow ? " (shadow)" : "");
        out += buf;
    }

    std::map<std::string, int> counts;
    for (const auto& h : hits) ++counts[h.situationId];
    std::snprintf(buf, sizeof(buf), "\n規則 version %d (%s)，%zu 張影格，共 %zu 次觸發\n", rules.version,
                  rules.hash.c_str(), frameCount, hits.size());
    out += buf;
    for (const auto& s : rules.situations) {
        std::snprintf(buf, sizeof(buf), "  %-20s %5d 次%s%s\n", s.id.c_str(), counts[s.id], s.shadow ? " (shadow)" : "",
                      s.enabled ? "" : " (停用)");
        out += buf;
    }
    for (const auto& p : rules.patrols) {
        std::snprintf(buf, sizeof(buf), "  %-20s   略過 (巡檢需要 VLM，無法重播)\n", p.id.c_str());
        out += buf;
    }
    return out;
}
