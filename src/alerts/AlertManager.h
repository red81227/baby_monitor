#pragma once

#include "events/EventRecorder.h"
#include "judge/PatrolScheduler.h"
#include "judge/VlmJudge.h"
#include "situation/RuleEngine.h"
#include "situation/SituationMonitor.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// 警報紀錄：每筆觸發一行 JSON (每次狀態變化寫入完整內容)，依當地日期分檔 alerts-YYYYMMDD.jsonl
class AlertLog {
public:
    explicit AlertLog(std::string dir);
    void append(const std::string& json);
    // 讀回最近的紀錄：同一 id 以最後一行為準，回傳最新的 limit 筆 (新的在前)
    std::vector<std::string> loadRecent(size_t limit) const;

private:
    std::string dir_;
    std::mutex mu_;
};

// 把觸發轉成警報：依路由立即提醒、交給 VLM 判定或只用來壓制；處理壓制、影子模式與判定失敗，
// 並保存所有觸發與警報。所有方法皆執行緒安全 (觸發來自分析執行緒，片段與判定結果來自各自的執行緒)。
class AlertManager {
public:
    // 送出狀態
    enum class Delivery {
        Pending,     // 等待片段或 VLM 判定
        Sent,        // 已送出 (監視頁面會顯示)
        Suppressed,  // 被 suppress 狀況壓制
        Shadow,      // 影子模式，只記錄
        NoAlert,     // VLM 判定不需要提醒
        Dropped,     // VLM 判定失敗且 on_timeout 為 drop
        Suppressor,  // 本身是 suppress 狀況，不產生警報
        Logged,      // 系統紀錄 (例如 ROI 由網頁修改)：只記錄，不是警報 (不跳橫幅、不更新 lastAlertEpochMs)
    };
    static const char* toString(Delivery d);

    struct Record {
        std::string id;
        std::string situationId;
        Severity severity = Severity::Info;
        Route route = Route::Direct;
        bool shadow = false;
        uint64_t seq = 0;
        int64_t tUs = 0;
        int64_t epochMs = 0;
        SignalFrame signals;
        int rulesVersion = 0;
        std::string rulesHash;
        std::string clipId;
        Delivery delivery = Delivery::Pending;
        int64_t deliveredEpochMs = 0;
        std::string suppressedBy;
        bool viaLlm = false;  // 是否經 LLM 判定 (direct 路由為 false)
        std::string cause;    // 原因說明 (LLM 或「未經 LLM 判定」的說明)
        std::optional<VlmJudge::Result> llm;
    };

    struct Deps {
        std::function<std::shared_ptr<const Rules>()> rules;
        std::function<std::map<std::string, SituationMonitor::ActiveState>()> activeStates;
        std::function<std::string(const EventRecorder::Request&, EventRecorder::ClipCallback)> requestClip;
        std::function<void(VlmJudge::Job)> submitJudge;
        std::function<int64_t()> nowMonoUs;   // 預設為 steady_clock
        std::function<int64_t()> nowEpochMs;  // 預設為 system_clock
        AlertLog* log = nullptr;
    };

    static constexpr size_t kRecentLimit = 100;

    explicit AlertManager(Deps deps);

    void onTrigger(const Trigger& trigger);
    // 巡檢觸發 (立即送出，依嚴重程度適用壓制)、巡檢停止 / 恢復 (系統警報，不可壓制)
    void onPatrolEvent(const PatrolScheduler::Event& event);

    // 系統紀錄 (route = system、送出狀態 logged)：kind 為紀錄的 situation (例如 "roi_changed")，
    // cause 為說明。規則版本與雜湊值記錄當下 (修改後) 的規則。回傳紀錄 id。
    std::string recordSystem(const std::string& kind, Severity severity, const std::string& cause);

    // 最近的觸發與警報 (JSON 物件字串，新的在前)
    std::vector<std::string> recentJson() const;
    std::optional<Record> find(const std::string& id) const;
    int64_t lastAlertEpochMs() const;

    static std::string toJson(const Record& r);

private:
    // 觸發當下的狀況設定 (規則之後可能被修改，判定時仍依觸發當下的設定)
    struct Settings {
        std::string prompt;
        int64_t preUs = 0;
        int64_t postUs = 0;
        int64_t timeoutUs = 0;
        bool onTimeoutAlert = true;
    };

    void onClip(const std::string& id, const EventRecorder::Clip& clip);
    void onVerdict(const std::string& id, const VlmJudge::Result& result);
    // 依 suppress 狀況與影子模式決定送出狀態 (呼叫時持有 mu_)
    void deliverLocked(Record& r);
    void commitLocked(const Record& r);

    Deps deps_;
    mutable std::mutex mu_;
    std::map<std::string, Record> live_;              // 尚在處理或最近的紀錄
    std::map<std::string, Settings> settings_;        // 尚未完成判定的紀錄的設定
    std::deque<std::pair<std::string, std::string>> recent_;  // (id, json)，新的在前
    int64_t lastAlertEpochMs_ = 0;
    uint64_t counter_ = 0;
};
