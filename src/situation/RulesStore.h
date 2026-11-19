#pragma once

#include "situation/Rules.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// 持有目前使用的規則，並在規則檔、ROI 檔或其引用的 prompt 檔變更時重新載入。
// 新規則驗證失敗時繼續使用原本的規則並記錄錯誤，直到下一次成功載入。
// 使用端以 current() 取得不可變的快照，比對指標即可知道規則是否換過。執行緒安全。
class RulesStore {
public:
    struct Status {
        std::shared_ptr<const Rules> rules;
        std::optional<std::string> lastError;  // 最近一次載入失敗的原因 (成功載入後清除)
        int64_t loadedAtEpochMs = 0;           // 目前規則的載入時間
    };

    // roiPath：網頁設定的 ROI 檔 (見 loadRules)；空字串 = 不使用
    RulesStore(std::string path, int ringSeconds,
               std::chrono::milliseconds pollInterval = std::chrono::milliseconds(2000), std::string roiPath = "");
    ~RulesStore();

    // 啟動時載入；失敗時回傳 false 並寫入 error (呼叫端應以非 0 結束)
    bool loadInitial(std::string& error);

    std::shared_ptr<const Rules> current() const;
    Status status() const;

    // 檢查檔案是否變更，有變更就重新載入。回傳 true 表示換成了新規則。
    bool checkNow();

    // 不比對檔案狀態，立即重新載入 (例如剛寫入 ROI 檔，要在回應前生效)。
    // 成功回傳 true；失敗時沿用目前的規則並回傳 false，error 為失敗原因。
    bool reloadNow(std::string& error);

    const std::string& roiPath() const { return roiPath_; }

    void start();  // 背景執行緒每 pollInterval 呼叫一次 checkNow()
    void stop();

private:
    struct Stamp {
        std::string path;
        int64_t mtimeNs = -1;  // -1 = 不存在
        int64_t size = -1;
        bool operator==(const Stamp& o) const { return path == o.path && mtimeNs == o.mtimeNs && size == o.size; }
    };
    static std::vector<Stamp> stampsOf(const std::vector<std::string>& files);
    static void printWarnings(const Rules& rules);
    std::vector<std::string> primaryFiles() const;  // 規則檔 (與 ROI 檔)，固定在 watchedFiles 的最前面
    bool reloadLocked(std::string* errorOut);       // 呼叫時持有 reloadMu_

    std::string path_;
    std::string roiPath_;
    int ringSeconds_;
    std::chrono::milliseconds pollInterval_;

    std::mutex reloadMu_;  // 序列化 checkNow() 與 reloadNow()
    mutable std::mutex mu_;
    std::shared_ptr<const Rules> rules_;
    std::optional<std::string> lastError_;
    int64_t loadedAtEpochMs_ = 0;
    std::vector<std::string> watchedFiles_;  // 最近一次嘗試載入時涉及的檔案
    std::vector<Stamp> stamps_;              // 最近一次嘗試載入時的檔案狀態

    std::mutex threadMu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread worker_;
};
