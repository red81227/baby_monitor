#include "check.h"
#include "situation/RuleEngine.h"

#include <memory>

namespace {

constexpr int64_t kSec = 1'000'000;

Condition cond(Signal s, Condition::Op op, double v, int64_t forUs = 0) { return Condition{s, op, v, forUs}; }

Situation situation(const std::string& id, std::vector<Condition> when, int64_t cooldownUs = 60 * kSec) {
    Situation s;
    s.id = id;
    s.severity = Severity::Warning;
    s.route = Route::Direct;
    s.when = std::move(when);
    s.cooldownUs = cooldownUs;
    return s;
}

std::shared_ptr<const Rules> rules(std::vector<Situation> list, int version = 1) {
    auto r = std::make_shared<Rules>();
    r->version = version;
    r->hash = "abc123def456";
    r->situations = std::move(list);
    return r;
}

// face_covered：身體可見且臉持續 10 秒看不到
std::shared_ptr<const Rules> faceRules() {
    return rules({situation("face_covered", {cond(Signal::BodyVisible, Condition::Op::Is, 1),
                                             cond(Signal::FaceVisible, Condition::Op::Is, 0, 10 * kSec)})});
}

SignalFrame frame(int64_t tUs, std::optional<bool> face, std::optional<bool> body = true) {
    SignalFrame f;
    f.seq = static_cast<uint64_t>(tUs / 100'000);
    f.tCaptureUs = tUs;
    if (face) f[Signal::FaceVisible] = *face ? 1.0 : 0.0;
    if (body) f[Signal::BodyVisible] = *body ? 1.0 : 0.0;
    return f;
}

// 以 10 Hz 送入 [from, to) 的影格，回傳觸發時間
std::vector<int64_t> run(RuleEngine& e, int64_t from, int64_t to, std::optional<bool> face,
                         std::optional<bool> body = true) {
    std::vector<int64_t> times;
    for (int64_t t = from; t < to; t += 100'000) {
        for (const auto& tr : e.onSignals(frame(t, face, body))) times.push_back(tr.tUs);
    }
    return times;
}

} // namespace

int main() {
    // 持續時間未達：臉部連續 6 秒看不到後又看到 → 不成立
    {
        RuleEngine e(faceRules());
        CHECK(run(e, 0, 6 * kSec, false).empty());
        CHECK(run(e, 6 * kSec, 8 * kSec, true).empty());
        CHECK(!e.state("face_covered")->active);
    }

    // 持續時間達到：在連續第 10 秒時成立並觸發
    {
        RuleEngine e(faceRules());
        const auto t = run(e, 0, 15 * kSec, false);
        CHECK(t.size() == 1);
        CHECK(t[0] == 10 * kSec);
        CHECK(e.state("face_covered")->active);
    }

    // 所有條件都要成立 (AND)：身體看不到時不成立
    {
        RuleEngine e(faceRules());
        CHECK(run(e, 0, 15 * kSec, false, false).empty());
    }

    // 中途訊號未知：第 5 秒起有一次偵測不到人物 (未知)，計時從再次偵測到時重新開始
    {
        RuleEngine e(faceRules());
        CHECK(run(e, 0, 5 * kSec, false).empty());
        CHECK(run(e, 5 * kSec, 5 * kSec + 500'000, std::nullopt, std::nullopt).empty());
        const auto t = run(e, 5 * kSec + 500'000, 20 * kSec, false);
        CHECK(t.size() == 1);
        CHECK(t[0] == 15 * kSec + 500'000);
    }

    // 觸發與冷卻：冷卻 60 秒的狀況持續成立 150 秒 → 第 0、60、120 秒共 3 次
    {
        RuleEngine e(rules({situation("awake", {cond(Signal::Activity, Condition::Op::Above, 0.03)}, 60 * kSec)}));
        std::vector<int64_t> times;
        for (int64_t t = 0; t < 150 * kSec; t += 500'000) {
            SignalFrame f;
            f.tCaptureUs = t;
            f[Signal::Activity] = 0.1;
            for (const auto& tr : e.onSignals(f)) times.push_back(tr.tUs);
        }
        CHECK(times.size() == 3);
        CHECK(times[0] == 0 && times[1] == 60 * kSec && times[2] == 120 * kSec);

        // 變成不成立後再成立：立即觸發
        SignalFrame quiet;
        quiet.tCaptureUs = 150 * kSec;
        quiet[Signal::Activity] = 0.0;
        CHECK(e.onSignals(quiet).empty());
        CHECK(e.state("awake")->inactiveSinceUs == 150 * kSec);
        SignalFrame busy;
        busy.tCaptureUs = 151 * kSec;
        busy[Signal::Activity] = 0.5;
        CHECK(e.onSignals(busy).size() == 1);
    }

    // 冷卻時間為 0：持續成立 10 分鐘只觸發 1 次，不成立後再成立再觸發 1 次
    {
        RuleEngine e(rules({situation("adult", {cond(Signal::PersonCount, Condition::Op::Above, 1)}, 0)}));
        int n = 0;
        auto feed = [&](int64_t from, int64_t to, double count) {
            for (int64_t t = from; t < to; t += kSec) {
                SignalFrame f;
                f.tCaptureUs = t;
                f[Signal::PersonCount] = count;
                n += static_cast<int>(e.onSignals(f).size());
            }
        };
        feed(0, 600 * kSec, 2);
        CHECK(n == 1);
        feed(600 * kSec, 610 * kSec, 1);
        feed(610 * kSec, 620 * kSec, 2);
        CHECK(n == 2);
    }

    // 觸發紀錄：狀況 id、時間、訊號值、規則版本與雜湊
    {
        auto r = rules({situation("face_covered", {cond(Signal::FaceVisible, Condition::Op::Is, 0)})}, 7);
        RuleEngine e(r);
        const auto trig = e.onSignals(frame(42 * kSec, false));
        CHECK(trig.size() == 1);
        CHECK(trig[0].situationId == "face_covered");
        CHECK(trig[0].tUs == 42 * kSec);
        CHECK(trig[0].seq == 420);
        CHECK(trig[0].signals[Signal::FaceVisible] == 0.0);
        CHECK(trig[0].rulesVersion == 7);
        CHECK(trig[0].rulesHash == "abc123def456");
        CHECK(trig[0].severity == Severity::Warning && trig[0].route == Route::Direct);
        CHECK(!trig[0].shadow);
    }

    // 影子模式：照常觸發並標示；停用：不成立
    {
        auto shadow = situation("s", {cond(Signal::FaceVisible, Condition::Op::Is, 0)});
        shadow.shadow = true;
        auto off = situation("off", {cond(Signal::FaceVisible, Condition::Op::Is, 0)});
        off.enabled = false;
        RuleEngine e(rules({shadow, off}));
        const auto trig = e.onSignals(frame(0, false));
        CHECK(trig.size() == 1 && trig[0].situationId == "s" && trig[0].shadow);
        CHECK(!e.state("off")->active);
    }

    // 換規則：同 id、同條件保留連續計時；改了門檻的條件重新計時；新增的從零開始；刪除的移除
    {
        RuleEngine e(faceRules());
        CHECK(run(e, 0, 6 * kSec, false).empty());

        // 只改 for (10s -> 15s)：條件內容 (訊號、比較、值) 相同，保留計時 → 第 15 秒觸發
        auto changedFor = faceRules();
        auto r2 = std::make_shared<Rules>(*changedFor);
        r2->situations[0].when[1].forUs = 15 * kSec;
        r2->situations.push_back(situation("awake", {cond(Signal::Activity, Condition::Op::Above, 0.03)}));
        e.setRules(r2);
        CHECK(e.state("awake") != nullptr && !e.state("awake")->active);
        auto t = run(e, 6 * kSec, 20 * kSec, false);
        CHECK(t.size() == 1 && t[0] == 15 * kSec);

        // 已成立的狀況換規則後仍視為成立，不會重新觸發 (冷卻 60s 內)
        e.setRules(r2);
        CHECK(run(e, 20 * kSec, 30 * kSec, false).empty());

        // 刪除 face_covered
        e.setRules(rules({situation("awake", {cond(Signal::Activity, Condition::Op::Above, 0.03)})}));
        CHECK(e.state("face_covered") == nullptr);

        // 條件改變 (比較的值不同)：重新計時
        RuleEngine e2(faceRules());
        CHECK(run(e2, 0, 6 * kSec, false).empty());
        auto r3 = std::make_shared<Rules>(*faceRules());
        r3->situations[0].when[0] = cond(Signal::BodyVisible, Condition::Op::Is, 1);  // 相同
        r3->situations[0].when[1] = cond(Signal::FaceVisible, Condition::Op::Is, 0, 10 * kSec);
        r3->situations[0].when.insert(r3->situations[0].when.begin(), cond(Signal::PersonConf, Condition::Op::Above, 0.1));
        e2.setRules(r3);
        // 新增的第一個條件讓順序改變 → 各條件重新計時；person_conf 未知 → 不成立
        CHECK(run(e2, 6 * kSec, 20 * kSec, false).empty());
    }

    // 容許中斷 grace：face_visible is false for 10s grace 1s
    auto graceRules = [](int64_t cooldownUs) {
        Condition face = cond(Signal::FaceVisible, Condition::Op::Is, 0, 10 * kSec);
        face.graceUs = 1 * kSec;
        return rules({situation("face_covered", {cond(Signal::BodyVisible, Condition::Op::Is, 1), face}, cooldownUs)});
    };
    // 中斷短於容許時間：第 4 秒起 0.3 秒看得到，計時不歸零，第 10 秒成立
    {
        RuleEngine e(graceRules(60 * kSec));
        auto t = run(e, 0, 4 * kSec, false);
        auto t2 = run(e, 4 * kSec, 4300'000, true);
        auto t3 = run(e, 4300'000, 15 * kSec, false);
        CHECK(t.empty() && t2.empty());
        CHECK(t3.size() == 1 && t3[0] == 10 * kSec);
    }
    // 訊號未知也算中斷：第 4 秒起 0.5 秒偵測不到人物，計時不歸零
    {
        RuleEngine e(graceRules(60 * kSec));
        CHECK(run(e, 0, 4 * kSec, false).empty());
        CHECK(run(e, 4 * kSec, 4500'000, std::nullopt, std::nullopt).empty());
        const auto t = run(e, 4500'000, 15 * kSec, false);
        CHECK(t.size() == 1 && t[0] == 10 * kSec);
    }
    // 中斷超過容許時間：第 4 秒起 1.5 秒看得到，從再次看不到 (5.5s) 重新計時，第 15.5 秒成立
    {
        RuleEngine e(graceRules(60 * kSec));
        CHECK(run(e, 0, 4 * kSec, false).empty());
        CHECK(run(e, 4 * kSec, 5500'000, true).empty());
        const auto t = run(e, 5500'000, 20 * kSec, false);
        CHECK(t.size() == 1 && t[0] == 15500'000);
    }
    // 成立中的短暫閃爍：冷卻 30 秒，觸發後短暫看得到 0.3 秒，維持成立且不再觸發
    {
        RuleEngine e(graceRules(30 * kSec));
        auto t = run(e, 0, 12 * kSec, false);
        CHECK(t.size() == 1 && t[0] == 10 * kSec);
        CHECK(run(e, 12 * kSec, 12300'000, true).empty());
        CHECK(e.state("face_covered")->active);
        CHECK(run(e, 12300'000, 25 * kSec, false).empty());
        CHECK(e.state("face_covered")->active);
        // 冷卻到了照常重複觸發
        t = run(e, 25 * kSec, 41 * kSec, false);
        CHECK(t.size() == 1 && t[0] == 40 * kSec);
    }
    // 對照：沒有 grace 時，同樣的閃爍讓狀況變成不成立，計時歸零
    {
        RuleEngine e(faceRules());
        CHECK(run(e, 0, 12 * kSec, false).size() == 1);
        CHECK(run(e, 12 * kSec, 12300'000, true).empty());
        CHECK(!e.state("face_covered")->active);
        const auto t = run(e, 12300'000, 25 * kSec, false);
        CHECK(t.size() == 1 && t[0] == 22300'000);
    }

    // 沒有規則
    {
        RuleEngine e;
        CHECK(e.onSignals(frame(0, false)).empty());
    }

    return 0;
}
