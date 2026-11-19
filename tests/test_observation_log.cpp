#include "check.h"
#include "situation/ObservationLog.h"
#include "situation/RulesStore.h"
#include "situation/SituationMonitor.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

SignalFrame sample(uint64_t seq, int64_t t) {
    SignalFrame f;
    f.seq = seq;
    f.tCaptureUs = t;
    f[Signal::PersonPresent] = 1.0;
    f[Signal::PersonCount] = 2.0;
    f[Signal::PersonConf] = 0.734512;
    f[Signal::FaceVisible] = 0.0;
    f[Signal::KeypointMotion] = 0.0123456;
    f[Signal::Activity] = 0.25;
    // BodyVisible、ProneLikely、NearBedEdge 未知
    return f;
}

bool sameFrame(const SignalFrame& a, const SignalFrame& b) {
    if (a.seq != b.seq || a.tCaptureUs != b.tCaptureUs) return false;
    for (const auto& info : kSignals) {
        const auto& x = a[info.id];
        const auto& y = b[info.id];
        if (x.has_value() != y.has_value()) return false;
        if (x && std::abs(*x - *y) > 1e-6 * std::max(1.0, std::abs(*x))) return false;
    }
    return true;
}

} // namespace

int main() {
    char tmpl[] = "/tmp/bm_test_obs_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    const fs::path dir = fs::path(tmpl) / "logs";  // 不存在的目錄會被建立

    // 寫入後讀回相同的 SignalFrame
    std::string date = "20261118";
    {
        ObservationLog log(dir.string());
        log.setDateProvider([&] { return date; });
        log.append(sample(1, 1'000'000));
        log.append(sample(2, 1'100'000));
        SignalFrame empty;  // 全部未知
        empty.seq = 3;
        empty.tCaptureUs = 1'200'000;
        log.append(empty);
        log.flush();

        int skipped = -1;
        const auto sessions = ObservationLog::read((dir / "observations-20261118.jsonl").string(), &skipped);
        CHECK(skipped == 0);
        CHECK(sessions.size() == 1);
        CHECK(sessions[0].epochMs > 0);
        CHECK(sessions[0].frames.size() == 3);
        CHECK(sameFrame(sessions[0].frames[0], sample(1, 1'000'000)));
        CHECK(sameFrame(sessions[0].frames[1], sample(2, 1'100'000)));
        CHECK(sameFrame(sessions[0].frames[2], empty));

        // 換日：寫入新檔
        date = "20261119";
        log.append(sample(4, 2'000'000));
    }
    CHECK(fs::exists(dir / "observations-20261119.jsonl"));

    // 重新開啟 (服務重啟)：同一檔案新增一個 session
    {
        ObservationLog log(dir.string());
        log.setDateProvider([&] { return date; });
        log.append(sample(1, 5'000'000));
    }
    {
        const auto sessions = ObservationLog::read((dir / "observations-20261119.jsonl").string());
        CHECK(sessions.size() == 2);
        CHECK(sessions[0].frames.size() == 1 && sessions[1].frames.size() == 1);
        CHECK(sessions[1].frames[0].tCaptureUs == 5'000'000);
    }

    // 保留 14 天：開新檔時刪除更舊的紀錄，其他檔案不受影響
    {
        std::ofstream(dir / "observations-20261101.jsonl") << "";  // 18 天前
        std::ofstream(dir / "observations-20261105.jsonl") << "";  // 14 天前：保留
        std::ofstream(dir / "alerts-20261101.jsonl") << "";
        ObservationLog log(dir.string());
        date = "20261119";
        log.setDateProvider([&] { return date; });
        log.append(sample(1, 1));
        CHECK(!fs::exists(dir / "observations-20261101.jsonl"));
        CHECK(fs::exists(dir / "observations-20261105.jsonl"));
        CHECK(fs::exists(dir / "alerts-20261101.jsonl"));
    }

    // 格式錯誤的行被略過；欄位依 session 的訊號名稱對應 (順序不同、多出不認得的訊號)
    {
        const fs::path p = dir / "handmade.jsonl";
        std::ofstream(p) << "[1,2,3]\n"  // session 之前
                         << R"({"type":"session","signals":["activity","new_signal","face_visible"],"monoUs":1,"epochMs":2})"
                         << "\n[7,100,0.5,9,1]\n"
                         << "[8,200,oops,9,1]\n"
                         << "[9,300,0.5]\n"
                         << "garbage\n"
                         << "[10,400,null,null,0]\n";
        int skipped = 0;
        const auto sessions = ObservationLog::read(p.string(), &skipped);
        CHECK(skipped == 4);
        CHECK(sessions.size() == 1 && sessions[0].frames.size() == 2);
        const auto& f = sessions[0].frames[0];
        CHECK(f.seq == 7 && f.tCaptureUs == 100);
        CHECK(f[Signal::Activity] == 0.5);
        CHECK(f[Signal::FaceVisible] == 1.0);
        CHECK(!f[Signal::PersonPresent]);
        CHECK(!sessions[0].frames[1][Signal::Activity]);
        CHECK(sessions[0].frames[1][Signal::FaceVisible] == 0.0);
    }

    // SituationMonitor：每張分析影格都寫入紀錄並送進規則引擎
    {
        const fs::path rulesPath = fs::path(tmpl) / "rules.yaml";
        std::ofstream(rulesPath) << R"(version: 1
bed_roi: [0.1, 0.1, 0.8, 0.8]
situations:
  - id: busy
    severity: warning
    route: direct
    when:
      - { signal: activity, above: 0.1, for: 1s }
    cooldown: 0s
)";
        RulesStore store(rulesPath.string(), 60);
        std::string error;
        CHECK(store.loadInitial(error));
        const fs::path monDir = fs::path(tmpl) / "monlogs";
        ObservationLog log(monDir.string());
        log.setDateProvider([] { return std::string("20261120"); });
        std::vector<Trigger> triggers;
        SituationMonitor mon(store, &log, [&](const Trigger& t) { triggers.push_back(t); });
        CHECK(mon.bedRoi().has_value());

        DetectionResult det;
        det.width = 1280;
        det.height = 720;
        for (int i = 0; i <= 20; ++i) {
            mon.onAnalysis(i, i * 100'000, 0.5, i % 5 == 0 ? &det : nullptr);
        }
        CHECK(triggers.size() == 1);
        CHECK(triggers[0].situationId == "busy" && triggers[0].tUs == 1'000'000);
        CHECK(mon.activeStates().at("busy").active);
        CHECK(mon.lastSignals()->seq == 20);
        CHECK(std::isfinite(*mon.lastSignals()->values[static_cast<int>(Signal::Activity)]));
        log.flush();
        const auto sessions = ObservationLog::read((monDir / "observations-20261120.jsonl").string());
        CHECK(sessions.size() == 1 && sessions[0].frames.size() == 21);
        CHECK(sessions[0].frames[0][Signal::PersonPresent] == 0.0);
    }

    fs::remove_all(tmpl);
    return 0;
}
