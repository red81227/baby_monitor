#pragma once

#include "situation/RulesStore.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// 從網頁設定 / 還原 ROI (bed_roi)。不依賴 HTTP 伺服器，方便測試；HTTP 介面見 RoiController。
//
// 設定時把 ROI 以原子方式寫入 RulesStore 的 ROI 檔，並在回傳前立即重新載入規則，
// 因此回傳成功時新的 ROI 已經生效。寫入前比對呼叫端讀到的規則雜湊值 (If-Match)，
// 避免覆蓋別人剛做的修改。執行緒安全 (同時間只處理一個寫入)。
class RoiService {
public:
    static constexpr double kMinSize = 0.05;  // 網頁設定的 ROI 寬、高下限 (畫面比例)

    struct Result {
        int status = 200;     // 200 / 400 / 409 / 500
        std::string error;    // 空字串 = 成功
        std::shared_ptr<const Rules> rules;  // 處理後 (或拒絕時) 目前使用的規則
    };

    // onChanged：成功修改後呼叫，參數為修改紀錄的說明 (舊值、新值、來源 IP)
    RoiService(RulesStore& store, std::function<void(const std::string&)> onChanged,
               std::function<int64_t()> nowEpochMs = nullptr);

    // roi：[x, y, w, h]，以畫面比例表示；nullopt 代表請求中沒有有效的 ROI (例如 null)
    Result set(const std::optional<std::vector<double>>& roi, const std::string& ifMatch, const std::string& remoteIp);
    // 刪除網頁設定，回到規則檔的 bed_roi (或未設定)
    Result reset(const std::string& ifMatch, const std::string& remoteIp);

    // 解析請求內容 {"roi": [x, y, w, h]}。格式錯誤時回傳 false 並寫入原因；
    // roi 為 null 或空陣列時回傳 true 且 out 為 nullopt (由 set() 拒絕，訊息較明確)。
    static bool parseBody(const std::string& body, std::optional<std::vector<double>>& out, std::string& error);

    // 跨站檢查：Origin (scheme://host[:port]) 的 host:port 必須與 Host 標頭相同。
    // origin 為空字串 (沒有 Origin 標頭，例如 curl) 時視為同源。
    static bool sameOrigin(const std::string& origin, const std::string& host);

    // "[0.200, 0.150, 0.600, 0.750]（規則檔）" / "未設定"
    static std::string describe(const Rules& rules);

private:
    Result current(int status, std::string error) const;
    bool writeRoiFile(const std::vector<double>& roi, const std::string& remoteIp, std::string& error);

    RulesStore& store_;
    std::function<void(const std::string&)> onChanged_;
    std::function<int64_t()> nowEpochMs_;
    std::mutex writeMu_;
};
