#include "alerts/AlertManager.h"
#include "check.h"

#include <crow/json.h>

#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSec = 1'000'000;

Situation sit(const std::string& id, Severity sev, Route route) {
    Situation s;
    s.id = id;
    s.severity = sev;
    s.route = route;
    s.when = {Condition{Signal::Activity, Condition::Op::Above, 0.0, 0}};
    s.cooldownUs = 60 * kSec;
    if (route == Route::Llm) s.promptText = "prompt for " + id;
    return s;
}

// 測試環境：可控制的規則、成立狀態、時間、片段與判定
struct Env {
    std::shared_ptr<Rules> rules = std::make_shared<Rules>();
    std::map<std::string, SituationMonitor::ActiveState> active;
    int64_t mono = 1000 * kSec;
    int64_t epoch = 1'795'000'000'000;
    std::vector<std::pair<EventRecorder::Request, EventRecorder::ClipCallback>> clips;
    std::vector<VlmJudge::Job> jobs;
    bool withJudge = true;

    Env() {
        rules->version = 4;
        rules->hash = "hash12345678";
        rules->situations = {sit("face_covered", Severity::Critical, Route::Direct),
                             sit("awake_active", Severity::Warning, Route::Llm),
                             sit("adult_present", Severity::Info, Route::Suppress)};
        Situation& adult = rules->situations[2];
        adult.suppressBelow = Severity::Critical;
        adult.lingerUs = 120 * kSec;
    }

    AlertManager::Deps deps(AlertLog* log = nullptr) {
        AlertManager::Deps d;
        d.rules = [this] { return std::shared_ptr<const Rules>(rules); };
        d.activeStates = [this] { return active; };
        d.requestClip = [this](const EventRecorder::Request& r, EventRecorder::ClipCallback cb) {
            clips.emplace_back(r, std::move(cb));
            return "clip-" + std::to_string(clips.size());
        };
        if (withJudge) d.submitJudge = [this](VlmJudge::Job j) { jobs.push_back(std::move(j)); };
        d.nowMonoUs = [this] { return mono; };
        d.nowEpochMs = [this] { return epoch; };
        d.log = log;
        return d;
    }

    Trigger trigger(const std::string& id) {
        const Situation* s = rules->find(id);
        Trigger t{id, s->severity, s->route, s->shadow, 42, mono, SignalFrame{}, rules->version, rules->hash};
        t.signals[Signal::Activity] = 0.2;
        return t;
    }

    // 完成最後一個片段 (10 張影格)
    void finishClip() {
        EventRecorder::Clip c;
        c.id = "clip-" + std::to_string(clips.size());
        c.request = clips.back().first;
        for (int i = 0; i < 10; ++i) {
            auto f = std::make_shared<RawFrame>();
            f->seq = i + 1;
            c.frames.push_back(f);
        }
        c.written = true;
        clips.back().second(c);
    }

    void verdict(VlmJudge::Result::Status st, bool alert = false, const std::string& cause = "") {
        VlmJudge::Result r;
        r.status = st;
        r.alert = alert;
        r.cause = cause;
        r.model = "fake";
        if (st != VlmJudge::Result::Status::Ok) r.error = "boom";
        auto job = std::move(jobs.back());
        jobs.pop_back();
        job.onDone(r);
    }
};

using D = AlertManager::Delivery;

AlertManager::Record only(const AlertManager& m) {
    const auto list = m.recentJson();
    CHECK(!list.empty());
    const auto j = crow::json::load(list.front());
    CHECK(j);
    const auto r = m.find(j["id"].s());
    CHECK(r.has_value());
    return *r;
}

} // namespace

int main() {
    // direct：觸發當下立即送出，不等片段或 LLM；附上片段 id
    {
        Env env;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("face_covered"));
        const auto r = only(m);
        CHECK(r.delivery == D::Sent);
        CHECK(r.severity == Severity::Critical);
        CHECK(!r.viaLlm);
        CHECK(r.clipId == "clip-1");
        CHECK(r.rulesVersion == 4 && r.rulesHash == "hash12345678");
        CHECK(r.epochMs == env.epoch);
        CHECK(m.lastAlertEpochMs() == env.epoch);
        CHECK(env.clips.size() == 1 && env.clips[0].first.preUs == 20 * kSec);
        // 沒有 prompt：片段完成後不送 LLM
        env.finishClip();
        CHECK(env.jobs.empty());
    }

    // llm：等片段 → 送判定 (prompt、影格、逾時) → 判定需要提醒時送出
    {
        Env env;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("awake_active"));
        CHECK(only(m).delivery == D::Pending);
        CHECK(m.lastAlertEpochMs() == 0);
        env.finishClip();
        CHECK(env.jobs.size() == 1);
        CHECK(env.jobs[0].prompt == "prompt for awake_active");
        CHECK(env.jobs[0].frames.size() == 10);
        CHECK(env.jobs[0].timeout == std::chrono::milliseconds(20'000));
        CHECK(env.jobs[0].signals[Signal::Activity] == 0.2);
        env.verdict(VlmJudge::Result::Status::Ok, true, "睜眼並揮動手腳");
        const auto r = only(m);
        CHECK(r.delivery == D::Sent && r.viaLlm && r.cause == "睜眼並揮動手腳");
        CHECK(r.llm && r.llm->model == "fake");
    }

    // llm 判定不需要提醒：不送出，紀錄原因
    {
        Env env;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(VlmJudge::Result::Status::Ok, false, "翻身後繼續睡");
        const auto r = only(m);
        CHECK(r.delivery == D::NoAlert && r.cause == "翻身後繼續睡");
        CHECK(m.lastAlertEpochMs() == 0);
    }

    // 判定失敗 (VLM 停止、逾時、格式錯誤)：on_timeout 未指定 (alert) 時送出並註明未經 LLM 判定
    for (auto st : {VlmJudge::Result::Status::Unavailable, VlmJudge::Result::Status::Timeout,
                    VlmJudge::Result::Status::BadReply, VlmJudge::Result::Status::QueueFull}) {
        Env env;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(st);
        const auto r = only(m);
        CHECK(r.delivery == D::Sent && !r.viaLlm);
        CHECK(r.cause.find("未經 LLM 判定") != std::string::npos);
    }

    // on_timeout: drop → 只記錄不送出
    {
        Env env;
        env.rules->situations[1].onTimeoutAlert = false;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(VlmJudge::Result::Status::Timeout);
        CHECK(only(m).delivery == D::Dropped);
    }

    // 沒有 VLM (未設定)：依 on_timeout 處理
    {
        Env env;
        env.withJudge = false;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        const auto r = only(m);
        CHECK(r.delivery == D::Sent && r.cause.find("未經 LLM 判定") != std::string::npos);
    }

    // 判定時使用觸發當下的設定 (之後改了規則也一樣)
    {
        Env env;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("awake_active"));
        env.rules->situations[1].onTimeoutAlert = false;
        env.rules->situations[1].promptText = "changed";
        env.finishClip();
        CHECK(env.jobs[0].prompt == "prompt for awake_active");
        env.verdict(VlmJudge::Result::Status::Timeout);
        CHECK(only(m).delivery == D::Sent);
    }

    // 壓制：大人在旁邊時 warning 不送出 (記錄壓制來源)，critical 照常送出
    {
        Env env;
        AlertManager m(env.deps());
        env.active["adult_present"] = {true, std::nullopt};
        m.onTrigger(env.trigger("adult_present"));
        CHECK(only(m).delivery == D::Suppressor);
        CHECK(env.clips.empty());

        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(VlmJudge::Result::Status::Ok, true, "醒了");
        auto r = only(m);
        CHECK(r.delivery == D::Suppressed && r.suppressedBy == "adult_present");
        CHECK(m.lastAlertEpochMs() == 0);

        m.onTrigger(env.trigger("face_covered"));
        CHECK(only(m).delivery == D::Sent);

        // linger：大人離開後 2 分鐘內仍壓制，之後恢復
        env.active["adult_present"] = {false, env.mono};
        env.mono += 100 * kSec;
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(VlmJudge::Result::Status::Ok, true, "醒了");
        CHECK(only(m).delivery == D::Suppressed);
        env.mono += 30 * kSec;
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(VlmJudge::Result::Status::Ok, true, "醒了");
        CHECK(only(m).delivery == D::Sent);

        // 判定失敗而送出的警報同樣會被壓制
        env.active["adult_present"] = {true, std::nullopt};
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(VlmJudge::Result::Status::Timeout);
        CHECK(only(m).delivery == D::Suppressed);

        // 影子模式的 suppress 狀況不會真的壓制
        env.rules->situations[2].shadow = true;
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        env.verdict(VlmJudge::Result::Status::Ok, true, "醒了");
        CHECK(only(m).delivery == D::Sent);
    }

    // 巡檢觸發：立即送出 (經 LLM 判定、附原因)，要求片段但不再送判定；
    // critical 巡檢與巡檢停止系統警報不可壓制，warning 巡檢依嚴重程度可被壓制
    {
        Env env;
        AlertManager m(env.deps());
        auto patrolEvent = [&](PatrolScheduler::Event::Type type, Severity sev, bool shadow = false) {
            PatrolScheduler::Event e;
            e.type = type;
            e.patrolId = "airway_covered";
            e.severity = sev;
            e.shadow = shadow;
            e.tUs = env.mono;
            e.clipPreUs = 20 * kSec;
            e.clipPostUs = 5 * kSec;
            e.rulesVersion = 4;
            e.rulesHash = "hash12345678";
            VlmJudge::Result res;
            res.status = VlmJudge::Result::Status::Ok;
            res.alert = true;
            res.cause = "被子蓋住口鼻";
            res.model = "fake";
            if (type != PatrolScheduler::Event::Type::Stalled) e.result = res;
            e.reason = "連續 3 次巡檢失敗: timeout";
            return e;
        };
        using T = PatrolScheduler::Event::Type;

        m.onPatrolEvent(patrolEvent(T::Trigger, Severity::Critical));
        auto r = only(m);
        CHECK(r.delivery == D::Sent && r.route == Route::Patrol && r.viaLlm);
        CHECK(r.situationId == "airway_covered" && r.severity == Severity::Critical);
        CHECK(r.cause == "被子蓋住口鼻" && r.llm && r.llm->alert);
        CHECK(r.clipId == "clip-1" && env.clips.size() == 1);
        CHECK(env.clips[0].first.situationId == "airway_covered" && env.clips[0].first.preUs == 20 * kSec);
        CHECK(m.lastAlertEpochMs() == env.epoch);
        env.finishClip();
        CHECK(env.jobs.empty());  // 已經過 VLM 判定，不再送判定
        CHECK(m.recentJson().front().find("\"route\":\"patrol\"") != std::string::npos);

        env.active["adult_present"] = {true, std::nullopt};
        m.onPatrolEvent(patrolEvent(T::Trigger, Severity::Critical));
        CHECK(only(m).delivery == D::Sent);
        m.onPatrolEvent(patrolEvent(T::Trigger, Severity::Warning));
        CHECK(only(m).delivery == D::Suppressed && only(m).suppressedBy == "adult_present");

        // 巡檢停止：warning 系統警報，大人在旁邊也照常送出；恢復時記錄一筆
        m.onPatrolEvent(patrolEvent(T::Stalled, Severity::Critical));
        r = only(m);
        CHECK(r.delivery == D::Sent && r.route == Route::System && r.severity == Severity::Warning);
        CHECK(r.situationId == "patrol_stalled:airway_covered");
        CHECK(r.cause.find("巡檢停止") != std::string::npos && r.cause.find("timeout") != std::string::npos);
        CHECK(r.clipId.empty());
        m.onPatrolEvent(patrolEvent(T::Recovered, Severity::Critical));
        r = only(m);
        CHECK(r.situationId == "patrol_recovered:airway_covered" && r.severity == Severity::Info);
        CHECK(r.delivery == D::Sent && r.cause.find("恢復") != std::string::npos);

        // 影子模式的巡檢觸發只記錄
        m.onPatrolEvent(patrolEvent(T::Trigger, Severity::Critical, true));
        CHECK(only(m).delivery == D::Shadow);
    }

    // 影子模式：照常判定，只記錄、不送出
    {
        Env env;
        env.rules->situations[1].shadow = true;
        env.rules->situations[0].shadow = true;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("face_covered"));
        CHECK(only(m).delivery == D::Shadow);
        m.onTrigger(env.trigger("awake_active"));
        env.finishClip();
        CHECK(env.jobs.size() == 1);  // 影子模式也送 LLM 判定
        env.verdict(VlmJudge::Result::Status::Ok, true, "醒了");
        const auto r = only(m);
        CHECK(r.delivery == D::Shadow && r.shadow && r.viaLlm);
        CHECK(m.lastAlertEpochMs() == 0);
    }

    // direct 路由的補充說明：先送出，VLM 之後回覆「不需要提醒」也不撤回，只附加說明
    {
        Env env;
        env.rules->situations[0].promptText = "describe";
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("face_covered"));
        CHECK(only(m).delivery == D::Sent);
        env.finishClip();
        CHECK(env.jobs.size() == 1 && env.jobs[0].prompt == "describe");
        env.verdict(VlmJudge::Result::Status::Ok, false, "被子蓋住臉");
        const auto r = only(m);
        CHECK(r.delivery == D::Sent && r.cause == "被子蓋住臉" && !r.viaLlm);
        CHECK(r.llm && !r.llm->alert);
    }

    // JSON 內容
    {
        Env env;
        AlertManager m(env.deps());
        m.onTrigger(env.trigger("face_covered"));
        const auto j = crow::json::load(m.recentJson().front());
        CHECK(j);
        CHECK(std::string(j["situation"].s()) == "face_covered");
        CHECK(std::string(j["severity"].s()) == "critical");
        CHECK(std::string(j["delivery"].s()) == "sent");
        CHECK(std::string(j["clipId"].s()) == "clip-1");
        CHECK(j["rulesVersion"].i() == 4);
        CHECK(j["signals"]["activity"].d() == 0.2);
        CHECK(j["signals"]["face_visible"].t() == crow::json::type::Null);
        CHECK(j["llm"].t() == crow::json::type::Null);
        CHECK(j["viaLlm"].t() == crow::json::type::False);
    }

    // 持久保存：重啟 (建立新的 AlertManager) 後查詢結果相同
    {
        char tmpl[] = "/tmp/bm_test_alerts_XXXXXX";
        CHECK(mkdtemp(tmpl) != nullptr);
        AlertLog log(tmpl);
        std::vector<std::string> before;
        int64_t lastBefore = 0;
        {
            Env env;
            AlertManager m(env.deps(&log));
            m.onTrigger(env.trigger("face_covered"));
            m.onTrigger(env.trigger("awake_active"));
            env.finishClip();
            env.verdict(VlmJudge::Result::Status::Ok, false, "翻身");
            m.onTrigger(env.trigger("adult_present"));
            before = m.recentJson();
            lastBefore = m.lastAlertEpochMs();
        }
        CHECK(before.size() == 3);
        Env env;
        AlertManager m(env.deps(&log));
        CHECK(m.recentJson() == before);
        CHECK(m.lastAlertEpochMs() == lastBefore);
        fs::remove_all(tmpl);
    }

    // 最近清單最多 100 筆
    {
        Env env;
        AlertManager m(env.deps());
        for (int i = 0; i < 130; ++i) m.onTrigger(env.trigger("face_covered"));
        CHECK(m.recentJson().size() == AlertManager::kRecentLimit);
    }

    // 系統紀錄 (ROI 修改)：送出狀態 logged，不更新 lastAlertEpochMs，持久保存
    {
        char tmpl[] = "/tmp/bm_test_alerts_sys_XXXXXX";
        CHECK(mkdtemp(tmpl) != nullptr);
        AlertLog log(tmpl);
        std::string id;
        {
            Env env;
            AlertManager m(env.deps(&log));
            id = m.recordSystem("roi_changed", Severity::Info, "ROI 由網頁設定：[0.2, 0.15, 0.6, 0.75]（規則檔）→ [0.1, 0.1, 0.5, 0.5]（網頁設定），來源 192.168.1.23");
            CHECK(m.lastAlertEpochMs() == 0);
            const auto r = only(m);
            CHECK(r.id == id);
            CHECK(r.situationId == "roi_changed");
            CHECK(r.route == Route::System);
            CHECK(r.severity == Severity::Info);
            CHECK(r.delivery == D::Logged);
            CHECK(r.rulesVersion == 4 && r.rulesHash == "hash12345678");
            const auto j = crow::json::load(m.recentJson().front());
            CHECK(std::string(j["delivery"].s()) == "logged");
            CHECK(std::string(j["route"].s()) == "system");
            CHECK(std::string(j["cause"].s()).find("192.168.1.23") != std::string::npos);
            CHECK(j["clipId"].t() == crow::json::type::Null);
        }
        Env env;
        AlertManager m(env.deps(&log));
        CHECK(m.recentJson().size() == 1);
        CHECK(m.lastAlertEpochMs() == 0);
        fs::remove_all(tmpl);
    }

    return 0;
}
