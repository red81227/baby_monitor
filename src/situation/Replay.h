#pragma once

#include "situation/ObservationLog.h"
#include "situation/Rules.h"

#include <string>
#include <vector>

// 離線重播：以觀測紀錄中的擷取時間 (而不是實際時間) 執行與服務相同的 RuleEngine。
// 每個 session (服務的一次執行) 使用一個新的 RuleEngine，與服務重啟時相同。
struct ReplayHit {
    std::string situationId;
    bool shadow;
    uint64_t seq;
    int64_t tUs;          // 擷取時間 (monotonic)
    int64_t epochMs;      // 由 session 的 (monotonic, 牆鐘) 對照換算
    int64_t sinceStartUs; // 距該 session 第一張影格
};

std::vector<ReplayHit> replayRules(const Rules& rules, const std::vector<ObservationLog::Session>& sessions);

// 以當地時間輸出每次觸發與各狀況的觸發次數；相同輸入產生相同輸出
std::string formatReplay(const Rules& rules, const std::vector<ReplayHit>& hits, size_t frameCount);
