// 離線重播：以規則檔重新判斷觀測紀錄，輸出每次觸發與各狀況的觸發次數。
//
//   bm_replay_rules [--ring-seconds N] <規則檔> <觀測紀錄.jsonl>...
//
// 時間取自紀錄中的擷取時間，相同輸入一定產生相同輸出。

#include "situation/ObservationLog.h"
#include "situation/Replay.h"
#include "situation/Rules.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    int ringSeconds = 3600;  // 重播時不受環形緩衝限制，除非指定
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ring-seconds") == 0 && i + 1 < argc) {
            ringSeconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            args.clear();
            break;
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() < 2 || ringSeconds < 1) {
        std::cerr << "用法: bm_replay_rules [--ring-seconds N] <規則檔> <觀測紀錄.jsonl>...\n";
        return 2;
    }

    std::string error;
    const auto rules = loadRules(args[0], ringSeconds, error);
    if (!rules) {
        std::cerr << error << "\n";
        return 1;
    }

    std::vector<ObservationLog::Session> sessions;
    size_t frames = 0;
    for (size_t i = 1; i < args.size(); ++i) {
        int skipped = 0;
        auto s = ObservationLog::read(args[i], &skipped);
        if (s.empty()) {
            std::cerr << "錯誤: 無法讀取觀測紀錄 " << args[i] << " (檔案不存在或沒有 session)\n";
            return 1;
        }
        if (skipped > 0) std::cerr << "[WARN] " << args[i] << ": 略過 " << skipped << " 行格式錯誤的紀錄\n";
        for (auto& session : s) {
            frames += session.frames.size();
            sessions.push_back(std::move(session));
        }
    }

    std::cout << formatReplay(*rules, replayRules(*rules, sessions), frames);
    return 0;
}
