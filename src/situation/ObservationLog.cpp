#include "situation/ObservationLog.h"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>

namespace fs = std::filesystem;

namespace {

std::string localDate(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return buf;
}

std::string todayLocal() {
    return localDate(std::time(nullptr));
}

constexpr const char* kPrefix = "observations-";
constexpr const char* kSuffix = ".jsonl";

} // namespace

std::string encodeObservation(const SignalFrame& frame) {
    std::string line = "[" + std::to_string(frame.seq) + "," + std::to_string(frame.tCaptureUs);
    char buf[32];
    for (const auto& info : kSignals) {
        const auto& v = frame[info.id];
        if (!v) {
            line += ",null";
        } else if (info.isBool) {
            line += *v != 0.0 ? ",1" : ",0";
        } else {
            std::snprintf(buf, sizeof(buf), ",%.6g", *v);
            line += buf;
        }
    }
    line += "]";
    return line;
}

ObservationLog::ObservationLog(std::string dir) : dir_(std::move(dir)), dateProvider_(todayLocal) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) std::cerr << "[WARN] 無法建立紀錄目錄 " << dir_ << ": " << ec.message() << "\n";
}

ObservationLog::~ObservationLog() {
    flush();
}

void ObservationLog::removeOld(const std::string& today) {
    // 檔名中的日期早於「今天 - kRetentionDays」者刪除 (以字串比較 YYYYMMDD)
    std::tm tm{};
    tm.tm_year = std::atoi(today.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(today.substr(4, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(today.substr(6, 2).c_str()) - kRetentionDays;
    tm.tm_hour = 12;
    const std::string cutoff = localDate(std::mktime(&tm));

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(kPrefix, 0) != 0 || name.size() != std::string(kPrefix).size() + 8 + std::string(kSuffix).size()) {
            continue;
        }
        const std::string date = name.substr(std::string(kPrefix).size(), 8);
        if (date < cutoff) fs::remove(entry.path(), ec);
    }
}

void ObservationLog::openForDate(const std::string& date) {
    if (out_.is_open()) out_.close();
    currentDate_ = date;
    const fs::path p = fs::path(dir_) / (kPrefix + date + kSuffix);
    out_.open(p, std::ios::app);
    if (!out_) {
        std::cerr << "[WARN] 無法寫入觀測紀錄 " << p.string() << "\n";
        return;
    }
    std::string names;
    for (const auto& info : kSignals) names += (names.empty() ? "\"" : ",\"") + std::string(info.name) + "\"";
    const auto mono = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    const auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    out_ << "{\"type\":\"session\",\"signals\":[" << names << "],\"monoUs\":" << mono << ",\"epochMs\":" << epoch
         << "}\n";
    out_.flush();
    removeOld(date);
}

void ObservationLog::append(const SignalFrame& frame) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string date = dateProvider_();
    if (date != currentDate_) openForDate(date);
    if (!out_) return;
    out_ << encodeObservation(frame) << '\n';
    if (++unflushed_ >= 30) {
        out_.flush();
        unflushed_ = 0;
    }
}

void ObservationLog::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.flush();
    unflushed_ = 0;
}

std::vector<ObservationLog::Session> ObservationLog::read(const std::string& path, int* skipped) {
    std::vector<Session> sessions;
    std::vector<std::optional<Signal>> columns;  // 每個值欄位對應的訊號 (不認得的訊號為 nullopt)
    int bad = 0;

    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '{') {
            try {
                const YAML::Node n = YAML::Load(line);
                if (n["type"].as<std::string>() != "session") throw YAML::Exception({}, "");
                columns.clear();
                for (const auto& name : n["signals"]) columns.push_back(signalByName(name.as<std::string>()));
                Session s;
                s.monoUs = n["monoUs"].as<int64_t>();
                s.epochMs = n["epochMs"].as<int64_t>();
                sessions.push_back(std::move(s));
            } catch (const YAML::Exception&) {
                ++bad;
            }
            continue;
        }
        if (sessions.empty() || line.front() != '[' || line.back() != ']') {
            ++bad;
            continue;
        }

        // [seq,t,v...]
        std::vector<std::string> fields;
        size_t start = 1;
        while (start < line.size()) {
            size_t comma = line.find(',', start);
            if (comma == std::string::npos) comma = line.size() - 1;
            fields.push_back(line.substr(start, comma - start));
            start = comma + 1;
        }
        if (fields.size() != columns.size() + 2) {
            ++bad;
            continue;
        }
        SignalFrame f;
        char* end = nullptr;
        f.seq = std::strtoull(fields[0].c_str(), &end, 10);
        bool ok = *end == '\0';
        f.tCaptureUs = std::strtoll(fields[1].c_str(), &end, 10);
        ok = ok && *end == '\0';
        for (size_t i = 0; ok && i < columns.size(); ++i) {
            const std::string& v = fields[i + 2];
            if (v == "null") continue;
            const double d = std::strtod(v.c_str(), &end);
            if (*end != '\0') {
                ok = false;
                break;
            }
            if (columns[i]) f[*columns[i]] = d;
        }
        if (!ok) {
            ++bad;
            continue;
        }
        sessions.back().frames.push_back(f);
    }
    if (skipped) *skipped = bad;
    return sessions;
}
