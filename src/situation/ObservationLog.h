#pragma once

#include "situation/Signals.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// 觀測紀錄：每個 SignalFrame 寫一行，依當地日期分檔 (observations-YYYYMMDD.jsonl)，保留 kRetentionDays 天。
//
// 檔案格式 (JSON lines)：
//   {"type":"session","signals":["person_present",...],"monoUs":M,"epochMs":E}   每次開始寫入時一行
//   [seq,tCaptureUs,v0,v1,...]                                                 每個 SignalFrame 一行，未知為 null
// session 行列出之後各欄的訊號名稱，並記下一組 (monotonic, 牆鐘) 對照，供換算時間。
// 每個 session 的 tCaptureUs 是同一個 monotonic 時鐘；不同 session (例如重新開機) 之間不可比較。
class ObservationLog {
public:
    static constexpr int kRetentionDays = 14;

    // dir 不存在時會建立
    explicit ObservationLog(std::string dir);
    ~ObservationLog();

    void append(const SignalFrame& frame);
    void flush();

    // 讀取一個紀錄檔，每個 session 一組 SignalFrame。格式錯誤的行會被略過並計入 skipped。
    struct Session {
        int64_t monoUs = 0;
        int64_t epochMs = 0;
        std::vector<SignalFrame> frames;
    };
    static std::vector<Session> read(const std::string& path, int* skipped = nullptr);

    // 測試用：以固定的日期取代當地日期 (YYYYMMDD)
    void setDateProvider(std::function<std::string()> provider) { dateProvider_ = std::move(provider); }

private:
    void openForDate(const std::string& date);
    void removeOld(const std::string& today);

    std::string dir_;
    std::function<std::string()> dateProvider_;
    std::mutex mu_;
    std::string currentDate_;
    std::ofstream out_;
    int unflushed_ = 0;
};

// 把 SignalFrame 編成紀錄檔的一行 (不含換行)，以及反向解析
std::string encodeObservation(const SignalFrame& frame);
