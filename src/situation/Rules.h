#pragma once

#include "situation/SignalExtractor.h"
#include "situation/Signals.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Patrol / System 只用於警報紀錄 (巡檢觸發、巡檢停止等系統警報)，規則檔的狀況不能使用
enum class Route { Direct, Llm, Suppress, Patrol, System };
enum class Severity { Info = 0, Warning = 1, Critical = 2 };

const char* toString(Route r);
const char* toString(Severity s);

// 生效的 bed_roi 來自哪裡：網頁設定的 ROI 檔優先於規則檔
enum class RoiSource { None, Rules, Override };
const char* toString(RoiSource s);  // "none" / "rules" / "override"

struct Condition {
    enum class Op { Is, Above, Below };
    Signal signal;
    Op op;
    double value;      // Is 時為 0 / 1
    int64_t forUs = 0;  // 需連續成立的時間
    int64_t graceUs = 0;  // 容許中斷：不成立 / 未知連續短於此時間時不重新計時

    bool holds(double v) const;
};

struct Situation {
    std::string id;
    Severity severity = Severity::Info;
    Route route = Route::Direct;
    std::vector<Condition> when;  // 全部成立 (AND)
    int64_t cooldownUs = 0;
    int64_t clipPreUs = 20'000'000;  // 片段：觸發前
    int64_t clipPostUs = 5'000'000;  // 片段：觸發後
    std::string promptPath;          // 解析後的路徑；空字串 = 沒有 prompt
    std::string promptText;
    int64_t timeoutUs = 20'000'000;  // VLM 判定逾時
    bool onTimeoutAlert = true;      // on_timeout: alert (true) / drop (false)
    bool shadow = false;
    bool enabled = true;
    // route == Suppress 時：壓制嚴重程度低於 suppressBelow 的警報，條件不再成立後持續 lingerUs
    Severity suppressBelow = Severity::Warning;
    int64_t lingerUs = 0;
};

// 巡檢：每隔 every 把最近的影格交給 VLM 判斷 (第一段訊號偵測不到的狀況，例如口鼻被蓋住)
struct Patrol {
    std::string id;
    Severity severity = Severity::Warning;
    int64_t everyUs = 30'000'000;
    int frames = 2;   // 送出最近幾張影格 (相隔 1 秒)
    int confirm = 1;  // 連續幾次回覆需要提醒才觸發
    int64_t cooldownUs = 0;
    int64_t clipPreUs = 20'000'000;
    int64_t clipPostUs = 5'000'000;
    std::string promptPath;
    std::string promptText;
    std::vector<Condition> whileConds;  // 前提條件 (AND)；空 = 一律巡檢
    int64_t timeoutUs = 20'000'000;
    bool shadow = false;
    bool enabled = true;
};

struct Rules {
    std::string path;
    int version = 0;
    std::string hash;  // 規則檔、所有 prompt 與 ROI 檔 (存在時) 內容的 SHA-256 前 12 碼
    SignalParams params;  // params.bedRoi 是生效的 ROI
    std::optional<cv::Rect2f> fileBedRoi;  // 規則檔中的 bed_roi (被 ROI 檔覆蓋時仍保留)
    RoiSource bedRoiSource = RoiSource::None;
    std::string roiPath;                   // ROI 檔路徑 (空 = 不使用 ROI 檔)
    std::vector<Situation> situations;
    std::vector<Patrol> patrols;
    std::vector<std::string> warnings;  // 合法但需要注意的設定 (例如 critical 狀況為影子模式)
    std::vector<std::string> watchedFiles;  // 規則檔、ROI 檔 (不論是否存在) 與引用的 prompt 檔 (熱重載時檢查)

    const Situation* find(const std::string& id) const;
    const Patrol* findPatrol(const std::string& id) const;
};

// 讀取並驗證規則檔。失敗時回傳 nullopt，error 含檔案路徑與錯誤位置 (行號或狀況 id)。
// ringSeconds：環形緩衝長度，片段前段不可超過此值。
// roiPath：網頁設定的 ROI 檔 (JSON，{"bed_roi": [x, y, w, h], ...})；檔案存在時覆蓋規則檔的 bed_roi，
// 內容無效時載入失敗。空字串 = 不使用 ROI 檔 (例如離線重播)。
std::optional<Rules> loadRules(const std::string& path, int ringSeconds, std::string& error,
                               const std::string& roiPath = "");

// ROI 檔在 overridesDir 中的路徑
std::string bedRoiFilePath(const std::string& overridesDir);

// bed_roi 的共用驗證：以畫面比例表示 (0–1)、寬高大於 0、不超出畫面。不符合時回傳 false 並寫入原因。
bool validateBedRoi(double x, double y, double w, double h, std::string& reason);

// 解析 "500ms" / "10s" / "5m" / "1h" (可帶負號)；格式錯誤回傳 nullopt
std::optional<int64_t> parseDurationUs(const std::string& text);
