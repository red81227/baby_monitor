#include "situation/RulesStore.h"

#include <filesystem>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

RulesStore::RulesStore(std::string path, int ringSeconds, std::chrono::milliseconds pollInterval,
                       std::string roiPath)
    : path_(std::move(path)), roiPath_(std::move(roiPath)), ringSeconds_(ringSeconds), pollInterval_(pollInterval) {}

std::vector<std::string> RulesStore::primaryFiles() const {
    if (roiPath_.empty()) return {path_};
    return {path_, roiPath_};
}

RulesStore::~RulesStore() {
    stop();
}

std::vector<RulesStore::Stamp> RulesStore::stampsOf(const std::vector<std::string>& files) {
    std::vector<Stamp> out;
    for (const auto& f : files) {
        Stamp s{f};
        std::error_code ec;
        const auto t = fs::last_write_time(f, ec);
        if (!ec) {
            s.mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
            const auto size = fs::file_size(f, ec);
            s.size = ec ? -1 : static_cast<int64_t>(size);
        }
        out.push_back(s);
    }
    return out;
}

void RulesStore::printWarnings(const Rules& rules) {
    for (const auto& w : rules.warnings) std::cerr << "[WARN] 規則檔 " << rules.path << ": " << w << "\n";
}

bool RulesStore::loadInitial(std::string& error) {
    // 先記下檔案狀態再讀取：讀取期間被修改時，下一次檢查會再載入一次
    std::lock_guard<std::mutex> reloadLock(reloadMu_);
    const auto stamps = stampsOf(primaryFiles());
    auto loaded = loadRules(path_, ringSeconds_, error, roiPath_);
    if (!loaded) return false;

    auto rules = std::make_shared<const Rules>(std::move(*loaded));
    printWarnings(*rules);
    std::lock_guard<std::mutex> lock(mu_);
    watchedFiles_ = rules->watchedFiles;
    stamps_ = stampsOf(watchedFiles_);
    for (size_t i = 0; i < stamps.size() && i < stamps_.size(); ++i) stamps_[i] = stamps[i];
    rules_ = std::move(rules);
    lastError_.reset();
    loadedAtEpochMs_ = nowEpochMs();
    return true;
}

std::shared_ptr<const Rules> RulesStore::current() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rules_;
}

RulesStore::Status RulesStore::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return Status{rules_, lastError_, loadedAtEpochMs_};
}

bool RulesStore::checkNow() {
    std::lock_guard<std::mutex> reloadLock(reloadMu_);
    std::vector<std::string> files;
    std::vector<Stamp> previous;
    {
        std::lock_guard<std::mutex> lock(mu_);
        files = watchedFiles_;
        previous = stamps_;
    }
    if (files.empty()) files = primaryFiles();
    if (stampsOf(files) == previous) return false;
    return reloadLocked(nullptr);
}

bool RulesStore::reloadNow(std::string& error) {
    std::lock_guard<std::mutex> reloadLock(reloadMu_);
    return reloadLocked(&error);
}

bool RulesStore::reloadLocked(std::string* errorOut) {
    // 先記下檔案狀態再讀取：讀取期間被修改時，下一次檢查會再載入一次
    const auto before = stampsOf(primaryFiles());
    std::string error;
    auto loaded = loadRules(path_, ringSeconds_, error, roiPath_);

    std::lock_guard<std::mutex> lock(mu_);
    if (!loaded) {
        // 保留原本的規則。不更新檔案狀態，之後每次檢查都重試 (例如補上缺少的 prompt 檔後就會成功)；
        // 同樣的錯誤只印一次。
        if (lastError_ != error) {
            std::cerr << error << " (繼續使用目前的規則";
            if (rules_) std::cerr << " version " << rules_->version;
            std::cerr << ")\n";
        }
        lastError_ = error;
        if (errorOut) *errorOut = error;
        return false;
    }

    auto rules = std::make_shared<const Rules>(std::move(*loaded));
    std::cout << "[INFO] 已重新載入規則檔 " << path_ << " (version " << rules->version << ", " << rules->hash
              << ", ROI " << toString(rules->bedRoiSource) << ")\n";
    printWarnings(*rules);
    watchedFiles_ = rules->watchedFiles;
    stamps_ = stampsOf(watchedFiles_);
    for (size_t i = 0; i < before.size() && i < stamps_.size(); ++i) stamps_[i] = before[i];
    rules_ = std::move(rules);
    lastError_.reset();
    loadedAtEpochMs_ = nowEpochMs();
    return true;
}

void RulesStore::start() {
    if (worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(threadMu_);
        stopping_ = false;
    }
    worker_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(threadMu_);
        while (!cv_.wait_for(lock, pollInterval_, [this] { return stopping_; })) {
            lock.unlock();
            checkNow();
            lock.lock();
        }
    });
}

void RulesStore::stop() {
    {
        std::lock_guard<std::mutex> lock(threadMu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}
