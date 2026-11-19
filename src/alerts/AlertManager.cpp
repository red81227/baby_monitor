#include "alerts/AlertManager.h"

#include "common/TimeUtil.h"

#include <crow/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// AlertLog

AlertLog::AlertLog(std::string dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) std::cerr << "[WARN] 無法建立紀錄目錄 " << dir_ << ": " << ec.message() << "\n";
}

void AlertLog::append(const std::string& json) {
    std::lock_guard<std::mutex> lock(mu_);
    const fs::path p = fs::path(dir_) / ("alerts-" + formatLocalTime(nowEpochMs(), "%Y%m%d") + ".jsonl");
    std::ofstream out(p, std::ios::app);
    if (!out) {
        std::cerr << "[WARN] 無法寫入警報紀錄 " << p.string() << "\n";
        return;
    }
    out << json << '\n';
}

std::vector<std::string> AlertLog::loadRecent(size_t limit) const {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("alerts-", 0) == 0 && e.path().extension() == ".jsonl") files.push_back(e.path());
    }
    std::sort(files.rbegin(), files.rend());  // 新的日期在前

    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& f : files) {
        std::ifstream in(f);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) lines.push_back(line);
        }
        // 同一檔案中由後往前：同一 id 以最後一行為準
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            const auto j = crow::json::load(*it);
            if (!j || !j.has("id")) continue;
            const std::string id = j["id"].s();
            if (!seen.insert(id).second) continue;
            out.push_back(*it);
            if (out.size() >= limit) return out;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// AlertManager

const char* AlertManager::toString(Delivery d) {
    switch (d) {
        case Delivery::Pending: return "pending";
        case Delivery::Sent: return "sent";
        case Delivery::Suppressed: return "suppressed";
        case Delivery::Shadow: return "shadow";
        case Delivery::NoAlert: return "no_alert";
        case Delivery::Dropped: return "dropped";
        case Delivery::Suppressor: return "suppressor";
        case Delivery::Logged: return "logged";
    }
    return "?";
}


AlertManager::AlertManager(Deps deps) : deps_(std::move(deps)) {
    if (!deps_.nowMonoUs) deps_.nowMonoUs = [] { return ::nowMonoUs(); };
    if (!deps_.nowEpochMs) deps_.nowEpochMs = [] { return ::nowEpochMs(); };
    if (deps_.log) {
        for (const auto& line : deps_.log->loadRecent(kRecentLimit)) {
            const auto j = crow::json::load(line);
            recent_.emplace_back(std::string(j["id"].s()), line);
            if (j.has("delivery") && std::string(j["delivery"].s()) == "sent" && j.has("deliveredEpochMs")) {
                lastAlertEpochMs_ = std::max<int64_t>(lastAlertEpochMs_, j["deliveredEpochMs"].i());
            }
        }
    }
}

std::string AlertManager::toJson(const Record& r) {
    crow::json::wvalue j;
    j["id"] = r.id;
    j["situation"] = r.situationId;
    j["severity"] = ::toString(r.severity);
    j["route"] = ::toString(r.route);
    j["shadow"] = r.shadow;
    j["seq"] = static_cast<uint64_t>(r.seq);
    j["epochMs"] = r.epochMs;
    for (const auto& info : kSignals) {
        const auto& v = r.signals[info.id];
        const std::string name(info.name);
        if (!v) j["signals"][name] = nullptr;
        else if (info.isBool) j["signals"][name] = *v != 0.0;
        else j["signals"][name] = *v;
    }
    j["rulesVersion"] = r.rulesVersion;
    j["rulesHash"] = r.rulesHash;
    if (r.clipId.empty()) j["clipId"] = nullptr;
    else j["clipId"] = r.clipId;
    j["delivery"] = toString(r.delivery);
    j["deliveredEpochMs"] = r.deliveredEpochMs;
    if (r.suppressedBy.empty()) j["suppressedBy"] = nullptr;
    else j["suppressedBy"] = r.suppressedBy;
    j["viaLlm"] = r.viaLlm;
    j["cause"] = r.cause;
    if (!r.llm) {
        j["llm"] = nullptr;
    } else {
        j["llm"]["status"] = VlmJudge::toString(r.llm->status);
        j["llm"]["alert"] = r.llm->alert;
        j["llm"]["cause"] = r.llm->cause;
        j["llm"]["model"] = r.llm->model;
        j["llm"]["latencyMs"] = r.llm->latencyMs;
        j["llm"]["error"] = r.llm->error;
        j["llm"]["raw"] = r.llm->rawReply;
    }
    return j.dump();
}

void AlertManager::commitLocked(const Record& r) {
    const std::string json = toJson(r);
    if (deps_.log) deps_.log->append(json);
    auto it = std::find_if(recent_.begin(), recent_.end(), [&](const auto& e) { return e.first == r.id; });
    if (it != recent_.end()) {
        it->second = json;
    } else {
        recent_.emplace_front(r.id, json);
        while (recent_.size() > kRecentLimit) {
            const std::string old = recent_.back().first;
            recent_.pop_back();
            auto lit = live_.find(old);
            if (lit != live_.end() && lit->second.delivery != Delivery::Pending) live_.erase(lit);
        }
    }
}

void AlertManager::deliverLocked(Record& r) {
    if (r.shadow) {
        r.delivery = Delivery::Shadow;
        return;
    }
    // critical 與系統警報不可壓制
    if (r.severity != Severity::Critical && r.route != Route::System) {
        const auto rules = deps_.rules ? deps_.rules() : nullptr;
        const auto active = deps_.activeStates ? deps_.activeStates() : std::map<std::string, SituationMonitor::ActiveState>{};
        const int64_t now = deps_.nowMonoUs();
        if (rules) {
            for (const auto& s : rules->situations) {
                if (s.route != Route::Suppress || !s.enabled || s.shadow) continue;
                if (static_cast<int>(r.severity) >= static_cast<int>(s.suppressBelow)) continue;
                const auto it = active.find(s.id);
                if (it == active.end()) continue;
                const auto& st = it->second;
                const bool on = st.active || (st.inactiveSinceUs && now - *st.inactiveSinceUs <= s.lingerUs);
                if (on) {
                    r.delivery = Delivery::Suppressed;
                    r.suppressedBy = s.id;
                    return;
                }
            }
        }
    }
    r.delivery = Delivery::Sent;
    r.deliveredEpochMs = deps_.nowEpochMs();
    lastAlertEpochMs_ = std::max(lastAlertEpochMs_, r.deliveredEpochMs);
    std::cout << "[ALERT] " << ::toString(r.severity) << " " << r.situationId
              << (r.cause.empty() ? "" : ": " + r.cause) << std::endl;
}

void AlertManager::onTrigger(const Trigger& t) {
    const auto rules = deps_.rules ? deps_.rules() : nullptr;
    const Situation* sit = rules ? rules->find(t.situationId) : nullptr;

    Record r;
    r.situationId = t.situationId;
    r.severity = t.severity;
    r.route = t.route;
    r.shadow = t.shadow;
    r.seq = t.seq;
    r.tUs = t.tUs;
    r.epochMs = deps_.nowEpochMs() - (deps_.nowMonoUs() - t.tUs) / 1000;
    r.signals = t.signals;
    r.rulesVersion = t.rulesVersion;
    r.rulesHash = t.rulesHash;

    Settings settings;
    if (sit) {
        settings = Settings{sit->promptText, sit->clipPreUs, sit->clipPostUs, sit->timeoutUs, sit->onTimeoutAlert};
    } else {
        settings = Settings{"", 20'000'000, 5'000'000, 20'000'000, true};
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        r.id = formatLocalTime(r.epochMs, "%Y%m%d-%H%M%S") + "-" + t.situationId + "-" + std::to_string(++counter_);
        if (t.route == Route::Suppress) {
            r.delivery = Delivery::Suppressor;
            live_[r.id] = r;
            commitLocked(r);
            return;
        }
        if (t.route == Route::Direct) deliverLocked(r);  // 立即提醒，不等片段或 LLM
        live_[r.id] = r;
        settings_[r.id] = settings;
    }

    // 片段 (llm 路由的判定輸入；direct 路由用於補充說明與事後查看)
    std::string clipId;
    if (deps_.requestClip) {
        const std::string id = r.id;
        clipId = deps_.requestClip({t.situationId, t.tUs, settings.preUs, settings.postUs, t.rulesVersion, t.rulesHash},
                                   [this, id](const EventRecorder::Clip& c) { onClip(id, c); });
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto it = live_.find(r.id);
    if (it == live_.end()) return;
    if (!clipId.empty()) it->second.clipId = clipId;
    if (!deps_.requestClip && it->second.route == Route::Llm) {
        // 沒有片段機制 (不應發生)：直接視為判定失敗
        it->second.cause = "未經 LLM 判定 (沒有事件片段)";
        if (settings.onTimeoutAlert) deliverLocked(it->second);
        else it->second.delivery = Delivery::Dropped;
    }
    commitLocked(it->second);
}

void AlertManager::onPatrolEvent(const PatrolScheduler::Event& e) {
    using Type = PatrolScheduler::Event::Type;
    Record r;
    r.seq = e.signals.seq;
    r.tUs = e.tUs;
    r.epochMs = deps_.nowEpochMs() - (deps_.nowMonoUs() - e.tUs) / 1000;
    r.signals = e.signals;
    r.rulesVersion = e.rulesVersion;
    r.rulesHash = e.rulesHash;
    if (e.type == Type::Trigger) {
        r.situationId = e.patrolId;
        r.severity = e.severity;
        r.route = Route::Patrol;
        r.shadow = e.shadow;
        r.viaLlm = true;
        if (e.result) {
            r.cause = e.result->cause;
            r.llm = e.result;
        }
    } else {
        const bool stalled = e.type == Type::Stalled;
        r.situationId = (stalled ? "patrol_stalled:" : "patrol_recovered:") + e.patrolId;
        r.severity = stalled ? Severity::Warning : Severity::Info;
        r.route = Route::System;
        r.cause = stalled ? e.patrolId + " 巡檢停止: " + e.reason : e.patrolId + " 巡檢已恢復";
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        r.id = formatLocalTime(r.epochMs, "%Y%m%d-%H%M%S") + "-" +
               (r.route == Route::System ? std::string(e.type == Type::Stalled ? "patrol_stalled" : "patrol_recovered") +
                                               "-" + e.patrolId
                                         : r.situationId) +
               "-" + std::to_string(++counter_);
        deliverLocked(r);  // 立即送出 (巡檢已經過 VLM 判定)
        live_[r.id] = r;
        if (e.type == Type::Trigger) settings_[r.id] = Settings{"", e.clipPreUs, e.clipPostUs, 0, true};
        commitLocked(r);
    }
    if (e.type != Type::Trigger || !deps_.requestClip) return;

    const std::string id = r.id;
    const std::string clipId =
        deps_.requestClip({e.patrolId, e.tUs, e.clipPreUs, e.clipPostUs, e.rulesVersion, e.rulesHash},
                          [this, id](const EventRecorder::Clip& c) { onClip(id, c); });
    std::lock_guard<std::mutex> lock(mu_);
    auto it = live_.find(id);
    if (it == live_.end() || clipId.empty()) return;
    it->second.clipId = clipId;
    commitLocked(it->second);
}

std::string AlertManager::recordSystem(const std::string& kind, Severity severity, const std::string& cause) {
    const auto rules = deps_.rules ? deps_.rules() : nullptr;
    Record r;
    r.situationId = kind;
    r.severity = severity;
    r.route = Route::System;
    r.tUs = deps_.nowMonoUs();
    r.epochMs = deps_.nowEpochMs();
    r.rulesVersion = rules ? rules->version : 0;
    r.rulesHash = rules ? rules->hash : "";
    r.cause = cause;
    r.delivery = Delivery::Logged;
    std::cout << "[INFO] " << kind << ": " << cause << std::endl;
    std::lock_guard<std::mutex> lock(mu_);
    r.id = formatLocalTime(r.epochMs, "%Y%m%d-%H%M%S") + "-" + kind + "-" + std::to_string(++counter_);
    live_[r.id] = r;
    commitLocked(r);
    return r.id;
}

void AlertManager::onClip(const std::string& id, const EventRecorder::Clip& clip) {
    VlmJudge::Job job;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = live_.find(id);
        if (it == live_.end()) return;
        const Settings s = settings_[id];
        const Record& r = it->second;
        // direct 路由沒有 prompt、巡檢 (已經過 VLM 判定)：不需要 LLM
        if ((r.route == Route::Direct && s.prompt.empty()) || r.route == Route::Patrol) {
            settings_.erase(id);
            return;
        }
        job.situationId = r.situationId;
        job.prompt = s.prompt;
        job.signals = r.signals;
        job.frames = clip.frames;
        job.preUs = s.preUs;
        job.postUs = s.postUs;
        job.timeout = std::chrono::milliseconds(s.timeoutUs / 1000);
        job.onDone = [this, id](const VlmJudge::Result& res) { onVerdict(id, res); };
    }
    if (deps_.submitJudge) {
        deps_.submitJudge(std::move(job));
    } else {
        VlmJudge::Result res;
        res.status = VlmJudge::Result::Status::Unavailable;
        res.error = "VLM 未設定";
        onVerdict(id, res);
    }
}

void AlertManager::onVerdict(const std::string& id, const VlmJudge::Result& res) {
    std::lock_guard<std::mutex> lock(mu_);
    const Settings s = settings_[id];
    settings_.erase(id);
    auto it = live_.find(id);
    if (it == live_.end()) return;
    Record& r = it->second;
    r.llm = res;
    const bool ok = res.status == VlmJudge::Result::Status::Ok;

    if (r.route == Route::Direct) {
        // 補充說明：不撤回、不延後已送出的警報
        r.cause = ok ? res.cause : "LLM 補充說明失敗 (" + std::string(VlmJudge::toString(res.status)) + ")";
        commitLocked(r);
        return;
    }

    if (ok) {
        r.viaLlm = true;
        r.cause = res.cause;
        if (res.alert) deliverLocked(r);
        else r.delivery = Delivery::NoAlert;
    } else {
        r.viaLlm = false;
        r.cause = "未經 LLM 判定 (" + std::string(VlmJudge::toString(res.status)) +
                  (res.error.empty() ? "" : ": " + res.error) + ")";
        if (s.onTimeoutAlert) deliverLocked(r);
        else r.delivery = Delivery::Dropped;
    }
    commitLocked(r);
}

std::vector<std::string> AlertManager::recentJson() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (const auto& e : recent_) out.push_back(e.second);
    return out;
}

std::optional<AlertManager::Record> AlertManager::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = live_.find(id);
    if (it == live_.end()) return std::nullopt;
    return it->second;
}

int64_t AlertManager::lastAlertEpochMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return lastAlertEpochMs_;
}
