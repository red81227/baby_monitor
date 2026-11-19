#include "events/EventRecorder.h"

#include "common/TimeUtil.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

EventRecorder::EventRecorder(RingBuffer& ring, std::string dir, int64_t maxBytes)
    : ring_(ring), dir_(std::move(dir)), maxBytes_(maxBytes) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) std::cerr << "[WARN] 無法建立事件片段目錄 " << dir_ << ": " << ec.message() << "\n";
    // 上次中斷留下的暫存目錄
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (e.path().filename().string().rfind(".tmp-", 0) == 0) fs::remove_all(e.path(), ec);
    }
}

EventRecorder::~EventRecorder() {
    stop();
}

void EventRecorder::start() {
    if (worker_.joinable()) return;
    worker_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mu_);
        while (!stopping_) {
            cv_.wait_for(lock, std::chrono::milliseconds(200));
            lock.unlock();
            processDue(nowMonoUs());
            lock.lock();
        }
    });
}

void EventRecorder::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    processDue(nowMonoUs(), true);
}

bool EventRecorder::isValidId(const std::string& id) {
    static const std::regex re(R"(^\d{8}-\d{6}-[a-z0-9_]+-\d+$)");
    return std::regex_match(id, re);
}

std::string EventRecorder::request(const Request& req, ClipCallback onDone) {
    const std::string stamp = formatLocalTime(monoUsToEpochMs(req.triggerUs), "%Y%m%d-%H%M%S");
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = stamp + "-" + req.situationId;
    char n[8];
    std::snprintf(n, sizeof(n), "%03d", ++counters_[key] % 1000);
    const std::string id = key + "-" + n;  // 補零：字串順序與時間順序一致
    if (counters_.size() > 1000) counters_.clear();  // 只需要同一秒內不重複

    const auto remainingUs = std::max<int64_t>(0, req.triggerUs + req.postUs - nowMonoUs());
    Pending p{id, req, std::move(onDone),
              std::chrono::steady_clock::now() + std::chrono::microseconds(remainingUs) + kStallGrace};
    pending_.push_back(std::move(p));
    cv_.notify_all();
    return id;
}

void EventRecorder::processDue(int64_t /*nowUs*/, bool force) {
    std::vector<Pending> due;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto newest = ring_.newestUs();
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_.begin(); it != pending_.end();) {
            const int64_t end = it->request.triggerUs + it->request.postUs;
            if (force || (newest && *newest >= end) || now >= it->deadline) {
                due.push_back(std::move(*it));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& p : due) finish(p);
}

void EventRecorder::finish(Pending& p) {
    Clip clip;
    clip.id = p.id;
    clip.request = p.request;
    clip.frames = ring_.range(p.request.triggerUs - p.request.preUs, p.request.triggerUs + p.request.postUs);
    clip.written = writeClip(clip);
    if (clip.written) enforceLimit(clip.id);
    if (p.onDone) p.onDone(clip);
}

bool EventRecorder::writeClip(const Clip& clip) {
    const fs::path tmp = fs::path(dir_) / (".tmp-" + clip.id);
    const fs::path final = fs::path(dir_) / clip.id;
    std::error_code ec;
    fs::create_directories(tmp, ec);
    if (ec) {
        std::cerr << "[WARN] 無法建立事件片段 " << tmp.string() << ": " << ec.message() << "\n";
        return false;
    }

    std::ostringstream meta;
    meta << "{\"id\":\"" << clip.id << "\",\"situation\":\"" << clip.request.situationId
         << "\",\"triggerUs\":" << clip.request.triggerUs
         << ",\"triggerEpochMs\":" << monoUsToEpochMs(clip.request.triggerUs)
         << ",\"rulesVersion\":" << clip.request.rulesVersion << ",\"rulesHash\":\"" << clip.request.rulesHash
         << "\",\"frames\":[";
    char name[32];
    for (size_t i = 0; i < clip.frames.size(); ++i) {
        const auto& f = clip.frames[i];
        std::snprintf(name, sizeof(name), "%06zu.jpg", i + 1);
        std::ofstream out(tmp / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(f->jpeg.data()), static_cast<std::streamsize>(f->jpeg.size()));
        if (!out) {
            std::cerr << "[WARN] 寫入事件片段失敗: " << (tmp / name).string() << "\n";
            fs::remove_all(tmp, ec);
            return false;
        }
        meta << (i ? "," : "") << "{\"file\":\"" << name << "\",\"seq\":" << f->seq
             << ",\"tCaptureUs\":" << toMicros(f->tCapture) << "}";
    }
    meta << "]}";
    std::ofstream(tmp / "meta.json") << meta.str() << "\n";

    fs::rename(tmp, final, ec);
    if (ec) {
        std::cerr << "[WARN] 無法完成事件片段 " << final.string() << ": " << ec.message() << "\n";
        fs::remove_all(tmp, ec);
        return false;
    }
    return true;
}

void EventRecorder::enforceLimit(const std::string& keep) {
    struct Entry {
        std::string name;
        fs::path path;
        int64_t bytes = 0;
    };
    std::vector<Entry> clips;
    int64_t total = 0;
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(dir_, ec)) {
        const std::string name = d.path().filename().string();
        if (!d.is_directory() || !isValidId(name)) continue;
        Entry e{name, d.path(), 0};
        for (const auto& f : fs::directory_iterator(d.path(), ec)) {
            if (f.is_regular_file()) e.bytes += static_cast<int64_t>(f.file_size(ec));
        }
        total += e.bytes;
        clips.push_back(std::move(e));
    }
    // 名稱以觸發時間開頭，字串順序即時間順序
    std::sort(clips.begin(), clips.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    for (const auto& c : clips) {
        if (total <= maxBytes_) break;
        if (c.name == keep) continue;
        fs::remove_all(c.path, ec);
        if (!ec) total -= c.bytes;
    }
}

std::optional<std::string> EventRecorder::readMeta(const std::string& id) const {
    if (!isValidId(id)) return std::nullopt;
    std::ifstream in(fs::path(dir_) / id / "meta.json");
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<std::string> EventRecorder::readFrame(const std::string& id, const std::string& file) const {
    static const std::regex fileRe(R"(^\d{6}\.jpg$)");
    if (!isValidId(id) || !std::regex_match(file, fileRe)) return std::nullopt;
    std::ifstream in(fs::path(dir_) / id / file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
