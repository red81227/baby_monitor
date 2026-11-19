#include "check.h"
#include "judge/PatrolScheduler.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using Type = PatrolScheduler::Event::Type;

namespace {

constexpr int64_t kSec = 1'000'000;

RingBuffer::FramePtr frameAt(int64_t tUs, uint64_t seq) {
    auto f = std::make_shared<RawFrame>();
    f->seq = seq;
    f->tCapture = std::chrono::steady_clock::time_point(std::chrono::microseconds(tUs));
    f->jpeg = {0xff, 0xd8, 0xff, 0xd9};
    f->width = 1280;
    f->height = 720;
    return f;
}

Patrol airway(int confirm = 2, int64_t cooldownUs = 60 * kSec) {
    Patrol p;
    p.id = "airway_covered";
    p.severity = Severity::Critical;
    p.everyUs = 30 * kSec;
    p.frames = 2;
    p.confirm = confirm;
    p.cooldownUs = cooldownUs;
    p.promptText = "口鼻是否被蓋住";
    p.timeoutUs = 20 * kSec;
    return p;
}

// 測試環境：可控制的時間、環形緩衝 (10 fps)、VLM 工作與事件
struct Env {
    std::shared_ptr<Rules> rules = std::make_shared<Rules>();
    int64_t mono = 1000 * kSec;
    int64_t epoch = 1'795'000'000'000;
    RingBuffer ring{60 * kSec};
    uint64_t seq = 0;
    std::vector<VlmJudge::Job> jobs;
    std::vector<PatrolScheduler::Event> events;
    std::optional<SignalFrame> signals;
    bool withVlm = true;
    std::unique_ptr<PatrolLog> log;
    std::unique_ptr<PatrolScheduler> sched;

    explicit Env(std::vector<Patrol> patrols, const fs::path& logDir = {}) {
        rules->version = 7;
        rules->hash = "hashpatrol12";
        rules->patrols = std::move(patrols);
        if (!logDir.empty()) log = std::make_unique<PatrolLog>(logDir.string());
    }

    void build() {
        PatrolScheduler::Deps d;
        d.rules = [this] { return std::shared_ptr<const Rules>(rules); };
        d.signals = [this] { return signals; };
        d.ring = &ring;
        if (withVlm) d.submit = [this](VlmJudge::Job j) { jobs.push_back(std::move(j)); };
        d.onEvent = [this](const PatrolScheduler::Event& e) { events.push_back(e); };
        d.nowMonoUs = [this] { return mono; };
        d.nowEpochMs = [this] { return epoch + (mono - 1000 * kSec) / 1000; };
        d.log = log.get();
        sched = std::make_unique<PatrolScheduler>(std::move(d));
    }

    // 時間前進 us：每 100ms 推入一張影格並 tick 一次
    void advance(int64_t us) {
        for (int64_t end = mono + us; mono < end;) {
            mono += 100'000;
            ring.push(frameAt(mono, ++seq));
            if (sched) sched->tick();
        }
    }

    // 回覆最早一筆尚未回覆的工作
    void reply(bool alert, const std::string& cause = "原因") {
        CHECK(!jobs.empty());
        VlmJudge::Job j = std::move(jobs.front());
        jobs.erase(jobs.begin());
        VlmJudge::Result r;
        r.status = VlmJudge::Result::Status::Ok;
        r.alert = alert;
        r.cause = cause;
        r.model = "fake";
        r.latencyMs = 600;
        j.onDone(r);
    }

    void fail(VlmJudge::Result::Status status = VlmJudge::Result::Status::Timeout) {
        CHECK(!jobs.empty());
        VlmJudge::Job j = std::move(jobs.front());
        jobs.erase(jobs.begin());
        VlmJudge::Result r;
        r.status = status;
        r.error = "fake failure";
        j.onDone(r);
    }

    int count(Type t) const {
        int n = 0;
        for (const auto& e : events) n += e.type == t;
        return n;
    }

    const PatrolScheduler::Status& status() {
        static PatrolScheduler::Status st;
        st = sched->status().at(0);
        return st;
    }
};

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("bm_patrol_test_" + std::to_string(getpid()));
    fs::remove_all(dir);

    // pickFrames：以最新一張為準往前每隔 1 秒取一張，依時間排序
    {
        std::vector<RingBuffer::FramePtr> frames;
        for (int i = 0; i <= 30; ++i) frames.push_back(frameAt(i * 100'000, i));  // 0.0s .. 3.0s
        const auto two = PatrolScheduler::pickFrames(frames, 2, kSec);
        CHECK(two.size() == 2 && two[0]->seq == 20 && two[1]->seq == 30);
        const auto five = PatrolScheduler::pickFrames(frames, 5, kSec);
        CHECK(five.size() == 4 && five[0]->seq == 0 && five[3]->seq == 30);  // 影格不夠時取到最早為止
        CHECK(PatrolScheduler::pickFrames({}, 2, kSec).empty());
    }

    // 口鼻被蓋住：每 30 秒巡檢，第一次回覆需要提醒後立即再巡檢，連續兩次才觸發
    {
        Env env({airway()});
        env.advance(3 * kSec);  // 攝影機先運作 3 秒
        env.build();
        env.advance(200'000);
        CHECK(env.jobs.size() == 1);  // 啟動後第一次巡檢立即進行
        CHECK(env.jobs[0].patrol && env.jobs[0].frames.size() == 2);
        CHECK(env.jobs[0].prompt == "口鼻是否被蓋住");
        env.advance(5 * kSec);
        CHECK(env.jobs.size() == 1);  // 進行中不重複送出
        env.reply(true, "被子蓋住口鼻");
        CHECK(env.events.empty());
        CHECK(env.status().positives == 1);
        env.advance(200'000);
        CHECK(env.jobs.size() == 1);  // 未達確認次數：立即再巡檢，不等 30 秒
        const int64_t lastFrame = toMicros(env.jobs[0].frames.back()->tCapture);
        env.reply(true, "被子蓋住口鼻與下巴");
        CHECK(env.events.size() == 1 && env.events[0].type == Type::Trigger);
        const auto& e = env.events[0];
        CHECK(e.patrolId == "airway_covered" && e.severity == Severity::Critical);
        CHECK(e.tUs == lastFrame);
        CHECK(e.result && e.result->cause == "被子蓋住口鼻與下巴");
        CHECK(e.rulesVersion == 7 && e.rulesHash == "hashpatrol12");
        CHECK(e.clipPreUs == 20 * kSec && e.clipPostUs == 5 * kSec);
    }

    // 單次誤判：立即再巡檢的結果不需要提醒 → 不觸發
    {
        Env env({airway()});
        env.build();
        env.advance(200'000);
        env.reply(true);
        env.advance(200'000);
        env.reply(false, "臉部清楚可見");
        CHECK(env.events.empty());
        CHECK(env.status().positives == 0);
        CHECK(env.status().lastAlert == false && env.status().lastCause == "臉部清楚可見");
        env.advance(10 * kSec);
        CHECK(env.jobs.empty());  // 下一次照 every 進行
    }

    // 冷卻：持續回覆需要提醒時，每隔 cooldown 再觸發一次；cooldown 為 0 時不重複
    for (const int64_t cooldown : {60 * kSec, int64_t(0)}) {
        Env env({airway(1, cooldown)});
        env.build();
        for (int i = 0; i < 6; ++i) {  // 6 次巡檢，約 150 秒
            env.advance(200'000);
            env.reply(true);
            env.advance(30 * kSec - 200'000);
        }
        CHECK(env.count(Type::Trigger) == (cooldown > 0 ? 3 : 1));
        // 回覆不需要提醒後再次需要提醒：視為新的一次
        env.advance(200'000);
        env.reply(false);
        env.advance(30 * kSec);
        env.reply(true);
        CHECK(env.count(Type::Trigger) == (cooldown > 0 ? 4 : 2));
    }

    // 巡檢紀錄：10 分鐘、每次都不需要提醒 → 約 20 筆紀錄，沒有事件
    {
        Env env({airway()}, dir / "log");
        env.build();
        for (int i = 0; i < 600; ++i) {
            env.advance(kSec);
            while (!env.jobs.empty()) env.reply(false, "臉部可見");
        }
        CHECK(env.events.empty());
        int lines = 0;
        for (const auto& f : fs::directory_iterator(dir / "log")) {
            CHECK(f.path().filename().string().rfind("patrols-", 0) == 0);
            std::ifstream in(f.path());
            for (std::string line; std::getline(in, line);) {
                ++lines;
                CHECK(line.find("\"patrol\":\"airway_covered\"") != std::string::npos);
                CHECK(line.find("\"status\":\"ok\"") != std::string::npos);
            }
        }
        CHECK(lines >= 19 && lines <= 21);
    }

    // 巡檢停止：連續 3 次失敗 → 停止事件一次；成功後恢復
    {
        Env env({airway()});
        env.build();
        for (int i = 0; i < 2; ++i) {
            env.advance(200'000);
            env.fail();
            env.advance(30 * kSec - 200'000);
        }
        CHECK(env.events.empty());
        env.advance(200'000);
        env.fail(VlmJudge::Result::Status::Unavailable);
        CHECK(env.count(Type::Stalled) == 1);
        CHECK(env.status().stalled);
        CHECK(env.events.back().reason.find("3") != std::string::npos);
        env.advance(30 * kSec);
        env.fail();
        CHECK(env.count(Type::Stalled) == 1);  // 停止期間不重複送出
        env.advance(30 * kSec);
        env.reply(false);
        CHECK(env.count(Type::Recovered) == 1);
        CHECK(!env.status().stalled && env.status().lastOkEpochMs > 0);
    }

    // 巡檢停止：判定一直沒有回來 (VLM 卡住)，超過 3 倍 every 沒有成功結果
    {
        Env env({airway()});
        env.build();
        env.advance(89 * kSec);
        CHECK(env.events.empty());
        env.advance(2 * kSec);
        CHECK(env.count(Type::Stalled) == 1);
        CHECK(env.jobs.size() == 1);  // 卡住的那一筆仍在進行，不重複送出
    }

    // 未設定 VLM：一開始就視為停止
    {
        Env env({airway()});
        env.withVlm = false;
        env.build();
        env.advance(200'000);
        CHECK(env.count(Type::Stalled) == 1);
        CHECK(env.status().stalled);
        CHECK(env.events[0].reason.find("BM_VLM_URL") != std::string::npos);
        env.advance(120 * kSec);
        CHECK(env.count(Type::Stalled) == 1);
    }

    // 沒有影格 (攝影機未運作) 算失敗
    {
        Env env({airway()});
        env.build();
        for (int i = 0; i < 2; ++i) {
            env.mono += 5 * kSec;
            env.sched->tick();
        }
        CHECK(env.count(Type::Stalled) == 0);
        env.mono += 4 * kSec;
        env.sched->tick();
        CHECK(env.count(Type::Stalled) == 0);  // 5 秒後才重試
        env.mono += 1 * kSec;
        env.sched->tick();
        CHECK(env.jobs.empty());
        CHECK(env.count(Type::Stalled) == 1);
        CHECK(env.status().lastError.find("影格") != std::string::npos);
    }
    // 啟動時還沒有影格：5 秒後重試，攝影機開始送影格後照常巡檢
    {
        Env env({airway()});
        env.build();
        env.sched->tick();
        CHECK(env.jobs.empty() && env.status().lastError.find("影格") != std::string::npos);
        env.advance(5 * kSec);
        CHECK(env.jobs.size() == 1 && env.events.empty());
        env.reply(false);
        CHECK(env.status().lastError.empty());
    }

    // 前提條件不成立：暫停，不呼叫 VLM，也不算停止
    {
        Patrol p = airway();
        p.whileConds = {Condition{Signal::PersonPresent, Condition::Op::Is, 1.0, 0}};
        Env env({p});
        SignalFrame f;
        f.seq = 1;
        f.tCaptureUs = env.mono;
        f[Signal::PersonPresent] = 0.0;
        env.signals = f;
        env.build();
        env.advance(200 * kSec);
        CHECK(env.jobs.empty() && env.events.empty());
        CHECK(env.status().paused && !env.status().stalled);
        // 有人之後開始巡檢
        f.seq = 2;
        f.tCaptureUs = env.mono;
        f[Signal::PersonPresent] = 1.0;
        env.signals = f;
        env.advance(200'000);
        CHECK(env.jobs.size() == 1 && !env.status().paused);
        CHECK(env.jobs[0].signals[Signal::PersonPresent] == 1.0);
    }

    // 停用與影子模式：停用不巡檢；影子模式照常巡檢與觸發 (標示影子)，停止時不送事件
    {
        Patrol off = airway();
        off.enabled = false;
        Env env({off});
        env.build();
        env.advance(100 * kSec);
        CHECK(env.jobs.empty() && env.events.empty());

        Patrol sh = airway(1);
        sh.shadow = true;
        Env env2({sh});
        env2.build();
        env2.advance(200'000);
        env2.reply(true);
        CHECK(env2.count(Type::Trigger) == 1 && env2.events[0].shadow);
        for (int i = 0; i < 3; ++i) {
            env2.advance(30 * kSec);
            env2.fail();
        }
        CHECK(env2.status().stalled && env2.count(Type::Stalled) == 0);
    }

    // 熱重載：同 id 巡檢保留計時與確認進度；刪除後舊的判定結果不再套用
    {
        Env env({airway()});
        env.build();
        env.advance(200'000);
        env.reply(true);
        auto r2 = std::make_shared<Rules>(*env.rules);
        r2->version = 8;
        env.rules = r2;
        env.advance(200'000);
        CHECK(env.status().positives == 1);
        CHECK(env.jobs.size() == 1);
        env.reply(true);
        CHECK(env.count(Type::Trigger) == 1 && env.events[0].rulesVersion == 8);

        env.advance(30 * kSec);
        CHECK(env.jobs.size() == 1);
        auto r3 = std::make_shared<Rules>(*env.rules);
        r3->patrols.clear();
        env.rules = r3;
        env.advance(200'000);
        env.reply(true);  // 巡檢已刪除：結果被忽略
        CHECK(env.count(Type::Trigger) == 1);
        CHECK(env.sched->status().empty());
    }

    fs::remove_all(dir);
    return 0;
}
