#include "check.h"
#include "situation/Replay.h"

#include <fstream>
#include <iostream>
#include <sstream>

int main() {
    const std::string dir = std::string(BM_SOURCE_DIR) + "/tests/data/replay/";
    std::string error;
    const auto rules = loadRules(dir + "rules.yaml", 3600, error);
    if (!rules) std::cerr << error << "\n";
    CHECK(rules.has_value());

    int skipped = -1;
    const auto sessions = ObservationLog::read(dir + "observations.jsonl", &skipped);
    CHECK(skipped == 0);
    CHECK(sessions.size() == 2);

    const auto hits = replayRules(*rules, sessions);
    // session 1：臉部第 2 秒起看不到 → 第 12 秒；活動第 20 秒起 → 第 25 秒；第 30 秒兩人 (影子模式)
    // session 2 (重啟)：從頭看不到臉 → 第 10 秒
    CHECK(hits.size() == 4);
    CHECK(hits[0].situationId == "face_covered" && hits[0].sinceStartUs == 12'000'000 && !hits[0].shadow);
    CHECK(hits[1].situationId == "awake_active" && hits[1].sinceStartUs == 25'000'000);
    CHECK(hits[2].situationId == "adult_present" && hits[2].sinceStartUs == 30'000'000 && hits[2].shadow);
    CHECK(hits[3].situationId == "face_covered" && hits[3].sinceStartUs == 10'000'000);
    CHECK(hits[0].epochMs == 1'795'000'000'000 + 12'000);

    // 相同輸入產生相同輸出，且與預期的輸出檔相同
    const std::string out1 = formatReplay(*rules, hits, 550);
    const std::string out2 = formatReplay(*rules, replayRules(*rules, ObservationLog::read(dir + "observations.jsonl")), 550);
    CHECK(out1 == out2);
    std::ifstream in(dir + "expected.txt");
    std::stringstream expected;
    expected << in.rdbuf();
    if (out1 != expected.str()) {
        std::cerr << "輸出與 expected.txt 不同:\n" << out1;
        return 1;
    }
    return 0;
}
