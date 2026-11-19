#include "check.h"
#include "common/Sha256.h"
#include "situation/Rules.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

fs::path g_dir;

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

// 一份合法的規則檔：每個測試在其中替換一段文字來製造錯誤
const std::string kValid = R"(version: 3
keypoint_visibility: 0.5
bed_roi: [0.2, 0.15, 0.6, 0.75]
bed_edge_margin: 0.1
situations:
  - id: face_covered
    severity: critical
    route: direct
    when:
      - { signal: body_visible, is: true }
      - { signal: face_visible, is: false, for: 10s }
    cooldown: 60s
    clip: [-20s, 5s]
    prompt: prompts/face.md
  - id: awake_active
    severity: warning
    route: llm
    when:
      - { signal: activity, above: 0.03, for: 30s }
    cooldown: 5m
    prompt: prompts/awake.md
    timeout: 20s
    on_timeout: drop
  - id: adult_present
    severity: info
    route: suppress
    when:
      - { signal: person_count, above: 1 }
    suppress: { below: critical, linger: 2m }
    cooldown: 0s
patrols:
  - id: airway_covered
    severity: critical
    every: 30s
    frames: 2
    confirm: 2
    cooldown: 60s
    prompt: prompts/airway.md
)";

std::optional<Rules> load(const std::string& text, std::string& error, int ring = 60) {
    const fs::path p = g_dir / "situations.yaml";
    writeFile(p, text);
    return loadRules(p.string(), ring, error);
}

std::string replaced(const std::string& from, const std::string& to) {
    std::string s = kValid;
    const auto pos = s.find(from);
    CHECK(pos != std::string::npos);
    s.replace(pos, from.size(), to);
    return s;
}

// 預期驗證失敗，且錯誤訊息包含規則檔路徑與所有 needles
void expectError(const std::string& text, std::initializer_list<const char*> needles) {
    std::string error;
    if (load(text, error).has_value()) {
        std::cerr << "預期失敗但載入成功; 期望訊息含: " << *needles.begin() << "\n";
        std::exit(1);
    }
    std::cout << "  " << error << "\n";
    CHECK(error.find((g_dir / "situations.yaml").string()) != std::string::npos);
    for (const char* n : needles) {
        if (error.find(n) == std::string::npos) {
            std::cerr << "錯誤訊息缺少 \"" << n << "\": " << error << "\n";
            std::exit(1);
        }
    }
}

} // namespace

int main() {
    char tmpl[] = "/tmp/bm_test_rules_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    g_dir = tmpl;
    writeFile(g_dir / "prompts/face.md", "face prompt");
    writeFile(g_dir / "prompts/awake.md", "awake prompt");
    writeFile(g_dir / "prompts/airway.md", "airway prompt");

    // SHA-256 已知值
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");

    // 時間字串
    CHECK(parseDurationUs("500ms") == 500'000);
    CHECK(parseDurationUs("10s") == 10'000'000);
    CHECK(parseDurationUs("1.5s") == 1'500'000);
    CHECK(parseDurationUs("5m") == 300'000'000);
    CHECK(parseDurationUs("1h") == 3'600'000'000LL);
    CHECK(parseDurationUs("-20s") == -20'000'000);
    CHECK(!parseDurationUs("10"));
    CHECK(!parseDurationUs("10 sec"));
    CHECK(!parseDurationUs(""));

    // 合法的規則檔與預設值
    {
        std::string error;
        auto r = load(kValid, error);
        if (!r) std::cerr << error << "\n";
        CHECK(r.has_value());
        CHECK(r->version == 3);
        CHECK(r->hash.size() == 12);
        CHECK(r->situations.size() == 3);
        CHECK(r->params.bedRoi.has_value());
        CHECK_NEAR(r->params.bedRoi->width, 0.6f, 1e-6f);
        CHECK(r->warnings.empty());
        CHECK(r->watchedFiles.size() == 4);  // 規則檔 + 兩個狀況 prompt + 一個巡檢 prompt

        const Situation* face = r->find("face_covered");
        CHECK(face && face->severity == Severity::Critical && face->route == Route::Direct);
        CHECK(face->when.size() == 2);
        CHECK(face->when[1].signal == Signal::FaceVisible && face->when[1].op == Condition::Op::Is);
        CHECK(face->when[1].value == 0.0 && face->when[1].forUs == 10'000'000);
        CHECK(face->cooldownUs == 60'000'000);
        CHECK(face->clipPreUs == 20'000'000 && face->clipPostUs == 5'000'000);
        CHECK(face->promptText == "face prompt");

        const Situation* awake = r->find("awake_active");
        CHECK(awake && awake->route == Route::Llm && !awake->onTimeoutAlert);
        CHECK(awake->when[0].op == Condition::Op::Above && awake->when[0].value == 0.03);
        CHECK(awake->clipPreUs == 20'000'000);  // 預設 clip

        const Situation* adult = r->find("adult_present");
        CHECK(adult && adult->route == Route::Suppress && adult->suppressBelow == Severity::Critical);
        CHECK(adult->lingerUs == 120'000'000);
        CHECK(adult->onTimeoutAlert);  // 預設 on_timeout: alert
        CHECK(!adult->shadow && adult->enabled);

        // 內容不同時雜湊不同；prompt 改變也會改變雜湊
        auto r2 = load(replaced("cooldown: 5m", "cooldown: 6m"), error);
        CHECK(r2 && r2->hash != r->hash);
        writeFile(g_dir / "prompts/awake.md", "awake prompt v2");
        auto r3 = load(kValid, error);
        CHECK(r3 && r3->hash != r->hash);
        writeFile(g_dir / "prompts/awake.md", "awake prompt");
    }

    // repo 隨附的預設規則檔：六個初始狀況都能載入
    {
        std::string error;
        auto r = loadRules(std::string(BM_SOURCE_DIR) + "/config/situations.yaml", 60, error);
        if (!r) std::cerr << error << "\n";
        CHECK(r.has_value());
        CHECK(r->warnings.empty());
        for (const char* id : {"face_covered", "prone", "awake_active", "not_in_bed", "near_bed_side", "adult_present"}) {
            CHECK(r->find(id) != nullptr);
        }
        CHECK(r->situations.size() == 6);
        // 口鼻被蓋住的巡檢：critical、沒有前提條件
        const Patrol* airway = r->findPatrol("airway_covered");
        CHECK(airway && airway->severity == Severity::Critical && airway->whileConds.empty());
        CHECK(airway->everyUs == 30'000'000 && airway->confirm == 2 && airway->frames == 2);
        CHECK(airway->promptText.find("\"alert\"") != std::string::npos);
        CHECK(r->find("face_covered")->when[1].graceUs == 1'000'000);
        CHECK(r->find("face_covered")->severity == Severity::Critical);
        CHECK(r->find("face_covered")->route == Route::Direct);
        CHECK(r->find("adult_present")->route == Route::Suppress);
        for (const auto& s : r->situations) {
            if (s.route == Route::Llm) CHECK(s.promptText.find("\"alert\"") != std::string::npos);
        }
    }

    // 巡檢：欄位與預設值
    {
        std::string error;
        auto r = load(kValid, error);
        if (!r) std::cerr << error << "\n";
        CHECK(r && r->patrols.size() == 1);
        const Patrol* p = r->findPatrol("airway_covered");
        CHECK(p && p->severity == Severity::Critical && p->everyUs == 30'000'000);
        CHECK(p->frames == 2 && p->confirm == 2 && p->cooldownUs == 60'000'000);
        CHECK(p->clipPreUs == 20'000'000 && p->clipPostUs == 5'000'000 && p->timeoutUs == 20'000'000);
        CHECK(p->whileConds.empty() && !p->shadow && p->enabled);
        CHECK(p->promptText == "airway prompt");
        CHECK(std::find(r->watchedFiles.begin(), r->watchedFiles.end(), p->promptPath) != r->watchedFiles.end());
        CHECK(r->find("airway_covered") == nullptr);  // 巡檢不是狀況

        auto w = load(replaced("    prompt: prompts/airway.md\n",
                               "    prompt: prompts/airway.md\n    while:\n      - { signal: person_present, is: true, for: 5s }\n"
                               "    clip: [-10s, 2s]\n    timeout: 15s\n    shadow: true\n"),
                      error);
        CHECK(w && w->patrols[0].whileConds.size() == 1 && w->patrols[0].whileConds[0].forUs == 5'000'000);
        CHECK(w->patrols[0].clipPreUs == 10'000'000 && w->patrols[0].timeoutUs == 15'000'000);
        CHECK(w->warnings.size() == 1 && w->warnings[0].find("airway_covered") != std::string::npos);

        // 巡檢 prompt 改變 → 雜湊改變
        auto h1 = load(kValid, error)->hash;
        writeFile(g_dir / "prompts/airway.md", "airway prompt v2");
        auto h2 = load(kValid, error)->hash;
        CHECK(h1 != h2);
        writeFile(g_dir / "prompts/airway.md", "airway prompt");
    }

    // grace：解析為微秒，省略時為 0
    {
        std::string error;
        auto r = load(replaced("for: 30s", "for: 30s, grace: 1500ms"), error);
        CHECK(r.has_value());
        CHECK(r && r->find("awake_active")->when[0].graceUs == 1'500'000);
        CHECK(r && r->find("face_covered")->when[1].graceUs == 0);
    }

    // 條件語意
    {
        Condition is{Signal::FaceVisible, Condition::Op::Is, 0.0, 0};
        CHECK(is.holds(0.0) && !is.holds(1.0));
        Condition above{Signal::Activity, Condition::Op::Above, 0.03, 0};
        CHECK(above.holds(0.04) && !above.holds(0.03));
        Condition below{Signal::PersonConf, Condition::Op::Below, 0.3, 0};
        CHECK(below.holds(0.2) && !below.holds(0.3));
    }

    // critical 為影子模式或停用：合法但有警告
    {
        std::string error;
        auto r = load(replaced("    clip: [-20s, 5s]\n", "    clip: [-20s, 5s]\n    shadow: true\n"), error);
        CHECK(r && r->warnings.size() == 1);
        CHECK(r->warnings[0].find("face_covered") != std::string::npos);
    }

    std::cout << "驗證失敗的案例:\n";
    // 檔案不存在
    {
        std::string error;
        CHECK(!loadRules((g_dir / "missing.yaml").string(), 60, error));
        CHECK(error.find("missing.yaml") != std::string::npos);
    }
    // YAML 語法錯誤
    expectError("version: 3\nsituations: [\n", {"YAML"});
    expectError("- just a list\n", {"對應表"});
    // version
    expectError(replaced("version: 3\n", ""), {"version"});
    expectError(replaced("version: 3", "version: three"), {"version", "第 1 行"});
    // 頂層參數
    expectError(replaced("keypoint_visibility: 0.5", "keypoint_visibility: 1.5"), {"keypoint_visibility"});
    expectError(replaced("bed_roi: [0.2, 0.15, 0.6, 0.75]", "bed_roi: [0.5, 0.15, 0.6, 0.75]"), {"bed_roi"});
    expectError(replaced("bed_roi: [0.2, 0.15, 0.6, 0.75]", "bed_roi: [0.2, 0.15]"), {"bed_roi"});
    expectError(replaced("bed_edge_margin: 0.1", "bed_edge_margin: 0.9"), {"bed_edge_margin"});
    expectError(replaced("bed_edge_margin: 0.1", "bed_edge_marign: 0.1"), {"bed_edge_marign"});
    expectError("version: 1\nsituations: []\n", {"situations"});
    // 狀況欄位
    expectError(replaced("  - id: face_covered\n", "  - severity_x: 1\n"), {"id"});
    expectError(replaced("id: awake_active", "id: Awake-Active"), {"Awake-Active", "id"});
    expectError(replaced("id: awake_active", "id: face_covered"), {"重複", "face_covered"});
    expectError(replaced("    cooldown: 5m\n", ""), {"cooldown", "awake_active"});
    expectError(replaced("    cooldown: 5m\n", "    cooldwn: 5m\n"), {"cooldwn"});
    expectError(replaced("severity: warning", "severity: high"), {"severity", "high", "awake_active"});
    expectError(replaced("route: llm", "route: email"), {"route", "email"});
    expectError(replaced("cooldown: 5m", "cooldown: 5"), {"cooldown", "awake_active"});
    expectError(replaced("cooldown: 5m", "cooldown: -5m"), {"cooldown", "負數"});
    // 條件
    expectError(replaced("signal: face_visible", "signal: face_visable"), {"face_visable", "face_covered", "第 11 行"});
    expectError(replaced("{ signal: activity, above: 0.03, for: 30s }", "{ signal: activity, is: true }"),
                {"activity", "above"});
    expectError(replaced("{ signal: body_visible, is: true }", "{ signal: body_visible, above: 0.5 }"),
                {"body_visible", "is"});
    expectError(replaced("{ signal: body_visible, is: true }", "{ signal: body_visible, is: yes }"),
                {"true 或 false"});
    expectError(replaced("{ signal: body_visible, is: true }", "{ signal: body_visible }"), {"is、above、below"});
    expectError(replaced("{ signal: activity, above: 0.03, for: 30s }",
                         "{ signal: activity, above: 0.03, below: 0.5 }"),
                {"is、above、below"});
    expectError(replaced("above: 0.03", "above: lots"), {"above", "數字"});
    expectError(replaced("for: 30s", "for: soon"), {"for"});
    expectError(replaced("for: 30s", "for: 30s, grace: -1s"), {"grace", "負數"});
    expectError(replaced("for: 30s", "for: 30s, grace: 1"), {"grace"});
    expectError(replaced("{ signal: activity, above: 0.03, for: 30s }", "{ signal: activity, abov: 0.03 }"),
                {"abov"});
    expectError(replaced("    when:\n      - { signal: activity, above: 0.03, for: 30s }\n", "    when: []\n"),
                {"when"});
    // clip、prompt、timeout
    expectError(replaced("clip: [-20s, 5s]", "clip: [20s, 5s]"), {"clip"});
    expectError(replaced("clip: [-20s, 5s]", "clip: [-20s]"), {"clip"});
    expectError(replaced("clip: [-20s, 5s]", "clip: [-20s, 2m]"), {"clip"});
    expectError(replaced("clip: [-20s, 5s]", "clip: [-90s, 5s]"), {"BM_RING_SECONDS", "face_covered"});
    {
        // 預設片段前段 20s 也受 BM_RING_SECONDS 限制
        std::string error;
        CHECK(!load(kValid, error, 10));
        CHECK(error.find("BM_RING_SECONDS") != std::string::npos);
    }
    expectError(replaced("prompt: prompts/awake.md", "prompt: prompts/nope.md"), {"nope.md", "awake_active"});
    expectError(replaced("    prompt: prompts/awake.md\n", ""), {"prompt", "awake_active"});
    expectError(replaced("timeout: 20s", "timeout: 10m"), {"timeout"});
    expectError(replaced("on_timeout: drop", "on_timeout: ignore"), {"on_timeout"});
    // suppress
    expectError(replaced("    suppress: { below: critical, linger: 2m }\n", ""), {"suppress", "adult_present"});
    expectError(replaced("below: critical", "below: info"), {"suppress.below"});
    expectError(replaced("below: critical", "below: everything"), {"suppress.below"});
    expectError(replaced("    timeout: 20s\n", "    timeout: 20s\n    suppress: { below: warning }\n"),
                {"suppress", "awake_active"});
    // 巡檢
    expectError(replaced("id: airway_covered", "id: face_covered"), {"重複", "face_covered"});
    expectError(replaced("    every: 30s\n", ""), {"every", "airway_covered"});
    expectError(replaced("every: 30s", "every: 2s"), {"every"});
    expectError(replaced("every: 30s", "every: 2h"), {"every"});
    expectError(replaced("    prompt: prompts/airway.md\n", ""), {"prompt", "airway_covered"});
    expectError(replaced("prompts/airway.md", "prompts/missing.md"), {"missing.md"});
    expectError(replaced("    cooldown: 60s\n    prompt: prompts/airway", "    prompt: prompts/airway"), {"cooldown"});
    expectError(replaced("frames: 2", "frames: 7"), {"frames"});
    expectError(replaced("confirm: 2", "confirm: 0"), {"confirm"});
    expectError(replaced("confirm: 2", "confirm: two"), {"confirm"});
    expectError(replaced("    frames: 2\n", "    frames: 2\n    route: llm\n"), {"route", "airway_covered"});
    expectError(replaced("    frames: 2\n", "    frames: 2\n    timeout: 30s\n"), {"timeout", "every"});
    expectError(replaced("    frames: 2\n", "    frames: 2\n    while: []\n"), {"while"});
    expectError(replaced("    frames: 2\n", "    frames: 2\n    while:\n      - { signal: nose_visible, is: true }\n"),
                {"nose_visible", "airway_covered"});
    {
        std::string text = kValid.substr(0, kValid.find("patrols:")) + "patrols: 5\n";
        expectError(text, {"patrols"});
    }

    // 底線保護：critical 不可交給 LLM 或用來壓制
    expectError(replaced("severity: critical\n    route: direct", "severity: critical\n    route: llm"),
                {"critical", "face_covered", "llm"});
    expectError(replaced("severity: info\n    route: suppress", "severity: critical\n    route: suppress"),
                {"critical", "adult_present"});

    // ROI 檔 (網頁設定)：優先於規則檔的 bed_roi，納入雜湊值與監看檔案
    {
        const fs::path rulesPath = g_dir / "situations.yaml";
        const fs::path roiPath = g_dir / "overrides" / "bed_roi.json";
        writeFile(rulesPath, kValid);
        fs::remove(roiPath);
        std::string error;
        auto approx = [](const std::optional<cv::Rect2f>& r, float x, float y, float w, float h) {
            return r && std::abs(r->x - x) < 1e-6 && std::abs(r->y - y) < 1e-6 && std::abs(r->width - w) < 1e-6 &&
                   std::abs(r->height - h) < 1e-6;
        };
        CHECK(bedRoiFilePath((g_dir / "overrides").string()) == roiPath.string());

        // 只有規則檔
        auto base = loadRules(rulesPath.string(), 60, error, roiPath.string());
        CHECK(base.has_value());
        CHECK(base->bedRoiSource == RoiSource::Rules);
        CHECK(approx(base->params.bedRoi, 0.2f, 0.15f, 0.6f, 0.75f));
        CHECK(approx(base->fileBedRoi, 0.2f, 0.15f, 0.6f, 0.75f));
        CHECK(base->roiPath == roiPath.string());
        // ROI 檔不存在也要監看 (被建立時才會重新載入)，且排在規則檔之後
        CHECK(base->watchedFiles.size() >= 2 && base->watchedFiles[0] == rulesPath.string() &&
              base->watchedFiles[1] == roiPath.string());
        // 不使用 ROI 檔時雜湊值與原本相同
        auto noRoi = loadRules(rulesPath.string(), 60, error);
        CHECK(noRoi && noRoi->hash == base->hash);
        CHECK(std::find(noRoi->watchedFiles.begin(), noRoi->watchedFiles.end(), roiPath.string()) ==
              noRoi->watchedFiles.end());

        // 兩者都有：ROI 檔優先；version 不變、hash 改變；未知欄位忽略
        writeFile(roiPath, R"({"bed_roi": [0.1, 0.1, 0.5, 0.5], "updatedAtEpochMs": 1, "updatedFrom": "x", "extra": [1]})");
        auto both = loadRules(rulesPath.string(), 60, error, roiPath.string());
        CHECK(both.has_value());
        CHECK(both->bedRoiSource == RoiSource::Override);
        CHECK(approx(both->params.bedRoi, 0.1f, 0.1f, 0.5f, 0.5f));
        CHECK(approx(both->fileBedRoi, 0.2f, 0.15f, 0.6f, 0.75f));
        CHECK(both->version == base->version);
        CHECK(both->hash != base->hash);
        writeFile(roiPath, R"({"bed_roi": [0.1, 0.1, 0.5, 0.6]})");
        auto changed = loadRules(rulesPath.string(), 60, error, roiPath.string());
        CHECK(changed && changed->hash != both->hash && changed->version == base->version);

        // 只有 ROI 檔
        writeFile(rulesPath, replaced("bed_roi: [0.2, 0.15, 0.6, 0.75]\n", ""));
        auto onlyRoi = loadRules(rulesPath.string(), 60, error, roiPath.string());
        CHECK(onlyRoi && onlyRoi->bedRoiSource == RoiSource::Override && !onlyRoi->fileBedRoi);
        CHECK(approx(onlyRoi->params.bedRoi, 0.1f, 0.1f, 0.5f, 0.6f));
        fs::remove(roiPath);
        auto none = loadRules(rulesPath.string(), 60, error, roiPath.string());
        CHECK(none && none->bedRoiSource == RoiSource::None && !none->params.bedRoi);
        writeFile(rulesPath, kValid);

        // ROI 檔無效：載入失敗，錯誤訊息含 ROI 檔路徑
        for (const char* bad : {"{not json", "[0.1, 0.1, 0.5, 0.5]", R"({"roi": [0.1, 0.1, 0.5, 0.5]})",
                                R"({"bed_roi": [0.1, 0.1, 0.5]})", R"({"bed_roi": [0.8, 0.1, 0.5, 0.5]})",
                                R"({"bed_roi": [0.1, 0.1, "a", 0.5]})", R"({"bed_roi": [0.1, 0.1, 0, 0.5]})"}) {
            writeFile(roiPath, bad);
            error.clear();
            CHECK(!loadRules(rulesPath.string(), 60, error, roiPath.string()));
            CHECK(error.find(roiPath.string()) != std::string::npos);
        }
        fs::remove_all(g_dir / "overrides");
    }

    // 共用驗證
    {
        std::string reason;
        CHECK(validateBedRoi(0, 0, 1, 1, reason));
        CHECK(!validateBedRoi(-0.1, 0, 0.5, 0.5, reason));
        CHECK(!validateBedRoi(0.6, 0, 0.5, 0.5, reason));
        CHECK(!validateBedRoi(0, 0, 0, 0.5, reason));
        CHECK(!validateBedRoi(0, 0, std::nan(""), 0.5, reason));
        CHECK(reason.find("bed_roi") != std::string::npos);
    }

    fs::remove_all(g_dir);
    return 0;
}
