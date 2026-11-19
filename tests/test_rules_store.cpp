#include "check.h"
#include "situation/RuleEngine.h"
#include "situation/RulesStore.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path g_dir;

std::string rulesText(int version, const std::string& forValue = "10s", const std::string& extra = "") {
    return "version: " + std::to_string(version) + R"(
situations:
  - id: face_covered
    severity: critical
    route: direct
    when:
      - { signal: face_visible, is: false, for: )" + forValue + R"( }
    cooldown: 60s
  - id: awake_active
    severity: warning
    route: llm
    when:
      - { signal: activity, above: 0.03 }
    cooldown: 5m
    prompt: prompts/awake.md
)" + extra;
}

// 寫入檔案並把 mtime 往後推，避免同一個時間刻度內的兩次寫入無法分辨
void writeFile(const fs::path& p, const std::string& text) {
    static int bump = 0;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
    fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(++bump));
}

} // namespace

int main() {
    char tmpl[] = "/tmp/bm_test_rules_store_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    g_dir = tmpl;
    const fs::path rulesPath = g_dir / "situations.yaml";
    writeFile(g_dir / "prompts/awake.md", "v1");

    // 啟動時規則檔不存在：失敗
    {
        RulesStore store(rulesPath.string(), 60);
        std::string error;
        CHECK(!store.loadInitial(error));
        CHECK(error.find(rulesPath.string()) != std::string::npos);
    }

    writeFile(rulesPath, rulesText(1));
    RulesStore store(rulesPath.string(), 60);
    std::string error;
    CHECK(store.loadInitial(error));
    auto first = store.current();
    CHECK(first->version == 1);
    CHECK(!store.status().lastError);
    CHECK(store.status().loadedAtEpochMs > 0);

    // 沒有變更：不重新載入
    CHECK(!store.checkNow());
    CHECK(store.current() == first);

    // 修改門檻：換成新規則
    writeFile(rulesPath, rulesText(2, "15s"));
    CHECK(store.checkNow());
    CHECK(store.current()->version == 2);
    CHECK(store.current()->find("face_covered")->when[0].forUs == 15'000'000);

    // 改壞規則檔：繼續使用原本的規則，回報錯誤
    auto good = store.current();
    writeFile(rulesPath, "version: 3\nsituations: [\n");
    CHECK(!store.checkNow());
    CHECK(store.current() == good);
    CHECK(store.status().lastError.has_value());
    CHECK(store.status().lastError->find("YAML") != std::string::npos);
    // 檔案沒再變更前持續重試，仍失敗、仍是原本的規則
    CHECK(!store.checkNow());
    CHECK(store.current() == good);

    // 修正後恢復：錯誤清除、套用新規則
    writeFile(rulesPath, rulesText(3));
    CHECK(store.checkNow());
    CHECK(store.current()->version == 3);
    CHECK(!store.status().lastError);

    // 只改 prompt 檔也會重新載入 (雜湊改變)
    const std::string hashBefore = store.current()->hash;
    writeFile(g_dir / "prompts/awake.md", "v2");
    CHECK(store.checkNow());
    CHECK(store.current()->hash != hashBefore);
    CHECK(store.current()->find("awake_active")->promptText == "v2");

    // 引用不存在的 prompt：失敗；補上檔案後 (規則檔沒有再變更) 自動恢復
    writeFile(rulesPath, rulesText(4) + R"(  - id: prone
    severity: warning
    route: llm
    when:
      - { signal: prone_likely, is: true }
    cooldown: 5m
    prompt: prompts/prone.md
)");
    CHECK(!store.checkNow());
    CHECK(store.current()->version == 3);
    CHECK(store.status().lastError->find("prone.md") != std::string::npos);
    writeFile(g_dir / "prompts/prone.md", "prone");
    CHECK(store.checkNow());
    CHECK(store.current()->version == 4);

    // critical 狀況設為影子模式：合法，有警告
    {
        std::string text = rulesText(5);
        text.replace(text.find("    cooldown: 60s\n"), 18, "    cooldown: 60s\n    shadow: true\n");
        writeFile(rulesPath, text);
        CHECK(store.checkNow());
        CHECK(store.current()->warnings.size() == 1);
    }

    // RuleEngine 換規則時保留同 id 狀況的連續計時 (由 RulesStore 換規則的典型用法)
    {
        writeFile(rulesPath, rulesText(6, "10s"));
        CHECK(store.checkNow());
        RuleEngine engine(store.current());
        auto feed = [&](int64_t t) {
            SignalFrame f;
            f.tCaptureUs = t;
            f[Signal::FaceVisible] = 0.0;
            return engine.onSignals(f);
        };
        for (int64_t t = 0; t < 6'000'000; t += 500'000) CHECK(feed(t).empty());
        writeFile(rulesPath, rulesText(7, "8s"));
        CHECK(store.checkNow());
        engine.setRules(store.current());
        CHECK(feed(7'500'000).empty());
        CHECK(feed(8'000'000).size() == 1);  // 從第 0 秒起算 8 秒
    }

    // 背景執行緒：變更後在輪詢週期內套用
    {
        RulesStore bg(rulesPath.string(), 60, 50ms);
        CHECK(bg.loadInitial(error));
        bg.start();
        writeFile(rulesPath, rulesText(8));
        bool updated = false;
        for (int i = 0; i < 100 && !updated; ++i) {
            std::this_thread::sleep_for(20ms);
            updated = bg.current()->version == 8;
        }
        CHECK(updated);
        bg.stop();
    }

    // ROI 檔：建立、修改、刪除都會重新載入；損壞時保留舊規則；reloadNow() 不等 stamp
    {
        const fs::path roiPath = g_dir / "overrides" / "bed_roi.json";
        writeFile(rulesPath, rulesText(9));
        RulesStore rs(rulesPath.string(), 60, 2000ms, roiPath.string());
        CHECK(rs.roiPath() == roiPath.string());
        CHECK(rs.loadInitial(error));
        CHECK(rs.current()->bedRoiSource == RoiSource::None);
        CHECK(!rs.checkNow());

        writeFile(roiPath, R"({"bed_roi": [0.1, 0.1, 0.5, 0.5]})");  // 建立
        CHECK(rs.checkNow());
        CHECK(rs.current()->bedRoiSource == RoiSource::Override);
        CHECK(rs.current()->version == 9);
        const std::string h1 = rs.current()->hash;

        writeFile(roiPath, R"({"bed_roi": [0.2, 0.1, 0.5, 0.5]})");  // 修改
        CHECK(rs.checkNow());
        CHECK(std::abs(rs.current()->params.bedRoi->x - 0.2f) < 1e-6);
        CHECK(rs.current()->hash != h1);

        auto good = rs.current();
        writeFile(roiPath, "{broken");  // 損壞
        CHECK(!rs.checkNow());
        CHECK(rs.current() == good);
        CHECK(rs.status().lastError && rs.status().lastError->find(roiPath.string()) != std::string::npos);

        fs::remove(roiPath);  // 刪除：回到規則檔 (未設定)
        CHECK(rs.checkNow());
        CHECK(rs.current()->bedRoiSource == RoiSource::None);
        CHECK(!rs.status().lastError);

        // reloadNow()：不比對 stamp 直接載入；之後 checkNow() 不會再載入一次
        writeFile(roiPath, R"({"bed_roi": [0.3, 0.3, 0.4, 0.4]})");
        std::string reloadError;
        CHECK(rs.reloadNow(reloadError));
        CHECK(rs.current()->bedRoiSource == RoiSource::Override);
        auto afterReload = rs.current();
        CHECK(!rs.checkNow());
        CHECK(rs.current() == afterReload);
        // 沒有變更也會重新載入 (換成新的快照)
        CHECK(rs.reloadNow(reloadError));
        CHECK(rs.current() != afterReload);
        // 失敗時回傳原因，保留目前規則
        writeFile(roiPath, R"({"bed_roi": [0.9, 0.9, 0.4, 0.4]})");
        CHECK(!rs.reloadNow(reloadError));
        CHECK(reloadError.find(roiPath.string()) != std::string::npos);
        CHECK(rs.current()->params.bedRoi->x < 0.31f);

        // 背景輪詢與 reloadNow() 同時執行不會出錯
        writeFile(roiPath, R"({"bed_roi": [0.1, 0.1, 0.5, 0.5]})");
        RulesStore bg2(rulesPath.string(), 60, 1ms, roiPath.string());
        CHECK(bg2.loadInitial(error));
        bg2.start();
        for (int i = 0; i < 50; ++i) CHECK(bg2.reloadNow(reloadError));
        bg2.stop();
        fs::remove_all(g_dir / "overrides");
    }

    fs::remove_all(g_dir);
    return 0;
}
