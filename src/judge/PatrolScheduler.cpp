#include "judge/PatrolScheduler.h"

#include "common/TimeUtil.h"

#include <crow/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// PatrolLog

namespace {
constexpr const char* kPrefix = "patrols-";
constexpr const char* kSuffix = ".jsonl";
} // namespace

PatrolLog::PatrolLog(std::string dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) std::cerr << "[WARN] 無法建立紀錄目錄 " << dir_ << ": " << ec.message() << "\n";
}

void PatrolLog::removeOld(const std::string& today) {
    // 檔名中的日期早於「今天 - kRetentionDays」者刪除 (以字串比較 YYYYMMDD)
    std::tm tm{};
    tm.tm_year = std::atoi(today.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(today.substr(4, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(today.substr(6, 2).c_str()) - kRetentionDays;
    tm.tm_hour = 12;
    const std::string cutoff = formatLocalTime(int64_t(std::mktime(&tm)) * 1000, "%Y%m%d");

    const std::string prefix(kPrefix), suffix(kSuffix);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || name.size() != prefix.size() + 8 + suffix.size()) continue;
        if (name.substr(prefix.size(), 8) < cutoff) fs::remove(entry.path(), ec);
    }
}

void PatrolLog::append(const std::string& json) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string date = formatLocalTime(nowEpochMs(), "%Y%m%d");
    if (date != lastDate_) {
        lastDate_ = date;
        removeOld(date);
    }
    const fs::path p = fs::path(dir_) / (kPrefix + date + kSuffix);
    std::ofstream out(p, std::ios::app);
    if (!out) {
        std::cerr << "[WARN] 無法寫入巡檢紀錄 " << p.string() << "\n";
        return;
    }
    out << json << '\n';
}

// ---------------------------------------------------------------------------
// PatrolScheduler

PatrolScheduler::PatrolScheduler(Deps deps) : deps_(std::move(deps)) {
    if (!deps_.nowMonoUs) deps_.nowMonoUs = [] { return ::nowMonoUs(); };
    if (!deps_.nowEpochMs) deps_.nowEpochMs = [] { return ::nowEpochMs(); };
}

PatrolScheduler::~PatrolScheduler() {
    stop();
}

void PatrolScheduler::start() {
    if (worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(threadMu_);
        stopping_ = false;
    }
    worker_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(threadMu_);
        while (!stopping_) {
            lock.unlock();
            tick();
            lock.lock();
            cv_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stopping_; });
        }
    });
}

void PatrolScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(threadMu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::vector<RingBuffer::FramePtr> PatrolScheduler::pickFrames(const std::vector<RingBuffer::FramePtr>& frames, int n,
                                                              int64_t spacingUs) {
    std::vector<RingBuffer::FramePtr> out;
    if (frames.empty() || n <= 0) return out;
    const int64_t newest = toMicros(frames.back()->tCapture);
    size_t idx = frames.size();  // 下一張要找的影格不晚於 frames[idx - 1]
    for (int k = 0; k < n && idx > 0; ++k) {
        const int64_t target = newest - k * spacingUs;
        // 取擷取時間不晚於 target 的最新一張
        while (idx > 0 && toMicros(frames[idx - 1]->tCapture) > target) --idx;
        if (idx == 0) break;
        out.push_back(frames[idx - 1]);
        --idx;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void PatrolScheduler::syncRules(const std::shared_ptr<const Rules>& rules, int64_t now) {
    std::map<std::string, State> next;
    auto whileRules = std::make_shared<Rules>();
    if (rules) {
        whileRules->version = rules->version;
        for (const auto& p : rules->patrols) {
            auto it = states_.find(p.id);
            State st = it != states_.end() ? std::move(it->second) : State{};
            if (it == states_.end()) {
                st.nextDueUs = now;
                st.status.id = p.id;
                st.generation = ++generationCounter_;
            }
            st.status.severity = p.severity;
            st.status.everyUs = p.everyUs;
            st.status.confirm = p.confirm;
            st.status.shadow = p.shadow;
            st.status.enabled = p.enabled;
            if (!p.enabled) st.status.paused = false;
            next.emplace(p.id, std::move(st));

            if (!p.whileConds.empty()) {
                Situation s;
                s.id = p.id;
                s.when = p.whileConds;
                whileRules->situations.push_back(std::move(s));
            }
        }
    }
    states_ = std::move(next);
    rules_ = rules;
    whileRules_ = whileRules;
    whileEngine_.setRules(whileRules_);  // 同 id 的前提條件保留計時
}

bool PatrolScheduler::whileHolds(const Patrol& p, int64_t) {
    if (p.whileConds.empty()) return true;
    const auto* st = whileEngine_.state(p.id);
    return st && st->active;
}

PatrolScheduler::Event PatrolScheduler::makeEvent(Event::Type type, const Patrol& p, int64_t tUs) const {
    Event e;
    e.type = type;
    e.patrolId = p.id;
    e.severity = p.severity;
    e.shadow = p.shadow;
    e.tUs = tUs;
    e.clipPreUs = p.clipPreUs;
    e.clipPostUs = p.clipPostUs;
    if (rules_) {
        e.rulesVersion = rules_->version;
        e.rulesHash = rules_->hash;
    }
    return e;
}

std::optional<PatrolScheduler::Event> PatrolScheduler::failLocked(State& st, const Patrol& p, const std::string& error,
                                                                  int64_t now) {
    ++st.failures;
    st.status.lastError = error;
    if (st.status.stalled || st.failures < kStallFailures) return std::nullopt;
    st.status.stalled = true;
    st.status.stallReason = "連續 " + std::to_string(st.failures) + " 次巡檢失敗: " + error;
    std::cout << "[WARN] 巡檢 " << p.id << " 停止: " << st.status.stallReason << std::endl;
    if (p.shadow) return std::nullopt;
    Event e = makeEvent(Event::Type::Stalled, p, now);
    e.reason = st.status.stallReason;
    return e;
}

void PatrolScheduler::logLocked(const Patrol& p, const VlmJudge::Result& res, int positives) {
    if (!deps_.log) return;
    crow::json::wvalue j;
    j["epochMs"] = deps_.nowEpochMs();
    j["patrol"] = p.id;
    j["status"] = VlmJudge::toString(res.status);
    if (res.status == VlmJudge::Result::Status::Ok) j["alert"] = res.alert;
    else j["alert"] = nullptr;
    j["cause"] = res.cause;
    j["error"] = res.error;
    j["latencyMs"] = res.latencyMs;
    j["model"] = res.model;
    j["raw"] = res.rawReply;
    j["positives"] = positives;
    j["confirm"] = p.confirm;
    j["shadow"] = p.shadow;
    j["rulesVersion"] = rules_ ? rules_->version : 0;
    j["rulesHash"] = rules_ ? rules_->hash : "";
    deps_.log->append(j.dump());
}

void PatrolScheduler::tick() {
    const int64_t now = deps_.nowMonoUs();
    const auto rules = deps_.rules ? deps_.rules() : nullptr;
    const auto signals = deps_.signals ? deps_.signals() : std::nullopt;

    std::vector<VlmJudge::Job> jobs;
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (rules != rules_) syncRules(rules, now);
        if (!rules_) return;
        if (signals && signals->seq != lastSignalSeq_) {
            lastSignalSeq_ = signals->seq;
            whileEngine_.onSignals(*signals);
        }

        for (const auto& p : rules_->patrols) {
            State& st = states_[p.id];
            const int64_t dt = st.lastTickUs ? now - *st.lastTickUs : 0;
            st.lastTickUs = now;
            if (!p.enabled) continue;

            st.status.paused = !whileHolds(p, now);
            if (st.status.paused) continue;  // 暫停期間不計入停止判斷

            if (!deps_.submit) {
                // VLM 未設定：巡檢無法進行，直接視為停止
                st.status.lastError = "VLM 未設定 (BM_VLM_URL)";
                if (!st.status.stalled) {
                    st.status.stalled = true;
                    st.status.stallReason = st.status.lastError;
                    if (!warnedNoVlm_) {
                        warnedNoVlm_ = true;
                        std::cout << "[WARN] 規則檔有巡檢，但 VLM 未設定 (BM_VLM_URL)，巡檢無法進行" << std::endl;
                    }
                    if (!p.shadow) {
                        Event e = makeEvent(Event::Type::Stalled, p, now);
                        e.reason = st.status.stallReason;
                        events.push_back(std::move(e));
                    }
                }
                continue;
            }

            st.noSuccessUs += dt;
            if (!st.status.stalled && st.noSuccessUs >= kStallEveryMultiple * p.everyUs) {
                st.status.stalled = true;
                st.status.stallReason = "超過 " + std::to_string(st.noSuccessUs / 1'000'000) + " 秒沒有成功的巡檢結果" +
                                        (st.status.lastError.empty() ? "" : " (" + st.status.lastError + ")");
                std::cout << "[WARN] 巡檢 " << p.id << " 停止: " << st.status.stallReason << std::endl;
                if (!p.shadow) {
                    Event e = makeEvent(Event::Type::Stalled, p, now);
                    e.reason = st.status.stallReason;
                    events.push_back(std::move(e));
                }
            }

            if (st.status.inFlight || now < st.nextDueUs) continue;
            st.nextDueUs = now + p.everyUs;

            std::vector<RingBuffer::FramePtr> frames;
            if (deps_.ring) {
                if (const auto newest = deps_.ring->newestUs()) {
                    const int64_t from = *newest - (p.frames - 1) * kFrameSpacingUs - kFrameSpacingUs / 2;
                    frames = pickFrames(deps_.ring->range(from, *newest), p.frames, kFrameSpacingUs);
                }
            }
            if (frames.empty()) {
                VlmJudge::Result res;
                res.status = VlmJudge::Result::Status::Unavailable;
                res.error = "環形緩衝沒有影格 (攝影機未運作)";
                // 啟動時攝影機可能還沒送出第一張影格：較快重試 (攝影機真的停住時同樣在連續失敗後停止)
                st.nextDueUs = now + std::min<int64_t>(p.everyUs, kNoFrameRetryUs);
                logLocked(p, res, st.status.positives);
                if (auto e = failLocked(st, p, res.error, now)) events.push_back(std::move(*e));
                continue;
            }

            VlmJudge::Job job;
            job.situationId = p.id;
            job.prompt = p.promptText;
            if (signals) job.signals = *signals;
            job.frames = std::move(frames);
            job.patrol = true;
            job.timeout = std::chrono::milliseconds(p.timeoutUs / 1000);
            const int64_t frameUs = toMicros(job.frames.back()->tCapture);
            const std::string id = p.id;
            const uint64_t gen = st.generation;
            const SignalFrame sig = job.signals;
            job.onDone = [this, id, gen, frameUs, sig](const VlmJudge::Result& r) { onResult(id, gen, frameUs, sig, r); };
            st.status.inFlight = true;
            jobs.push_back(std::move(job));
        }
    }
    for (const auto& e : events) {
        if (deps_.onEvent) deps_.onEvent(e);
    }
    for (auto& j : jobs) deps_.submit(std::move(j));
}

void PatrolScheduler::onResult(const std::string& id, uint64_t generation, int64_t frameUs, const SignalFrame& signals,
                               const VlmJudge::Result& res) {
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = states_.find(id);
        const Patrol* p = rules_ ? rules_->findPatrol(id) : nullptr;
        if (it == states_.end() || !p || it->second.generation != generation) return;
        State& st = it->second;
        st.status.inFlight = false;
        const int64_t now = deps_.nowMonoUs();

        if (res.status != VlmJudge::Result::Status::Ok) {
            logLocked(*p, res, st.status.positives);
            if (auto e = failLocked(st, *p, std::string(VlmJudge::toString(res.status)) +
                                                (res.error.empty() ? "" : ": " + res.error),
                                    now)) {
                events.push_back(std::move(*e));
            }
        } else {
            st.failures = 0;
            st.noSuccessUs = 0;
            st.status.lastOkEpochMs = deps_.nowEpochMs();
            st.status.lastAlert = res.alert;
            st.status.lastCause = res.cause;
            st.status.lastError.clear();
            if (st.status.stalled) {
                st.status.stalled = false;
                st.status.stallReason.clear();
                std::cout << "[INFO] 巡檢 " << id << " 已恢復" << std::endl;
                if (!p->shadow) {
                    Event e = makeEvent(Event::Type::Recovered, *p, now);
                    e.result = res;
                    events.push_back(std::move(e));
                }
            }
            if (res.alert) {
                ++st.status.positives;
                if (st.status.positives >= p->confirm) {
                    const bool fire = !st.active ||
                                      (p->cooldownUs > 0 && st.lastTriggerUs && now - *st.lastTriggerUs >= p->cooldownUs);
                    st.active = true;
                    if (fire) {
                        st.lastTriggerUs = now;
                        Event e = makeEvent(Event::Type::Trigger, *p, frameUs);
                        e.signals = signals;
                        e.result = res;
                        events.push_back(std::move(e));
                    }
                } else {
                    st.nextDueUs = now;  // 尚未達到確認次數：立即再巡檢一次
                }
            } else {
                st.status.positives = 0;
                st.active = false;
            }
            logLocked(*p, res, st.status.positives);
        }
    }
    for (const auto& e : events) {
        if (deps_.onEvent) deps_.onEvent(e);
    }
}

std::vector<PatrolScheduler::Status> PatrolScheduler::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Status> out;
    if (!rules_) return out;
    for (const auto& p : rules_->patrols) {
        const auto it = states_.find(p.id);
        if (it != states_.end()) out.push_back(it->second.status);
    }
    return out;
}
