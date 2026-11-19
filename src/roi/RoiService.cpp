#include "roi/RoiService.h"

#include <crow/json.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {

int64_t systemEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

std::string fmtRoi(const cv::Rect2f& r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[%.3f, %.3f, %.3f, %.3f]", r.x, r.y, r.width, r.height);
    return buf;
}

const char* sourceLabel(RoiSource s) {
    switch (s) {
        case RoiSource::Override: return "網頁設定";
        case RoiSource::Rules: return "規則檔";
        case RoiSource::None: return "未設定";
    }
    return "?";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// "host[:port]" 或 "[v6][:port]" -> (host, port)；沒有 port 時回傳空字串
std::pair<std::string, std::string> splitHostPort(const std::string& authority) {
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) return {authority, ""};
        const std::string host = authority.substr(0, close + 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') return {host, authority.substr(close + 2)};
        return {host, ""};
    }
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) return {authority, ""};
    return {authority.substr(0, colon), authority.substr(colon + 1)};
}

// fsync 一個路徑 (檔案或目錄)；失敗回傳 false
bool fsyncPath(const std::string& path, int flags) {
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

} // namespace

RoiService::RoiService(RulesStore& store, std::function<void(const std::string&)> onChanged,
                       std::function<int64_t()> nowEpochMs)
    : store_(store), onChanged_(std::move(onChanged)), nowEpochMs_(std::move(nowEpochMs)) {
    if (!nowEpochMs_) nowEpochMs_ = systemEpochMs;
}

std::string RoiService::describe(const Rules& rules) {
    if (!rules.params.bedRoi) return "未設定";
    return fmtRoi(*rules.params.bedRoi) + "（" + sourceLabel(rules.bedRoiSource) + "）";
}

RoiService::Result RoiService::current(int status, std::string error) const {
    return Result{status, std::move(error), store_.current()};
}

bool RoiService::parseBody(const std::string& body, std::optional<std::vector<double>>& out, std::string& error) {
    out.reset();
    const auto j = crow::json::load(body);
    if (!j || j.t() != crow::json::type::Object) {
        error = "請求內容必須是 JSON 物件 {\"roi\": [x, y, w, h]}";
        return false;
    }
    if (!j.has("roi")) {
        error = "缺少 roi";
        return false;
    }
    const auto& roi = j["roi"];
    if (roi.t() == crow::json::type::Null) return true;
    if (roi.t() != crow::json::type::List) {
        error = "roi 必須是 [x, y, w, h] 四個數字";
        return false;
    }
    if (roi.size() == 0) return true;
    std::vector<double> v;
    for (size_t i = 0; i < roi.size(); ++i) {
        if (roi[i].t() != crow::json::type::Number) {
            error = "roi 必須是 [x, y, w, h] 四個數字";
            return false;
        }
        v.push_back(roi[i].d());
    }
    out = std::move(v);
    return true;
}

bool RoiService::sameOrigin(const std::string& origin, const std::string& host) {
    if (origin.empty()) return true;
    std::string scheme, rest;
    if (lower(origin).rfind("http://", 0) == 0) {
        scheme = "http";
        rest = origin.substr(7);
    } else if (lower(origin).rfind("https://", 0) == 0) {
        scheme = "https";
        rest = origin.substr(8);
    } else {
        return false;  // 包含 "null" (sandbox、file://)
    }
    if (rest.find_first_of("/?#") != std::string::npos || host.empty()) return false;
    const std::string defaultPort = scheme == "https" ? "443" : "80";
    auto [oHost, oPort] = splitHostPort(lower(rest));
    auto [hHost, hPort] = splitHostPort(lower(host));
    if (oPort.empty()) oPort = defaultPort;
    if (hPort.empty()) hPort = defaultPort;
    return !oHost.empty() && oHost == hHost && oPort == hPort;
}

bool RoiService::writeRoiFile(const std::vector<double>& roi, const std::string& remoteIp, std::string& error) {
    const std::string path = store_.roiPath();
    const fs::path dir = fs::path(path).parent_path();
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(dir, ec);
    if (ec) {
        error = "無法建立網頁設定目錄 " + dir.string() + ": " + ec.message();
        return false;
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf), "[%.3f, %.3f, %.3f, %.3f]", roi[0], roi[1], roi[2], roi[3]);
    const std::string text = std::string("{\"bed_roi\": ") + buf + ", \"updatedAtEpochMs\": " +
                             std::to_string(nowEpochMs_()) + ", \"updatedFrom\": " + crow::json::wvalue(remoteIp).dump() +
                             "}\n";

    // 原子寫入：暫存檔 -> fsync -> rename -> fsync 目錄
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "無法寫入 ROI 檔 " + tmp + ": " + std::error_code(errno, std::generic_category()).message();
        return false;
    }
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ok = ok && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) {
        error = "無法寫入 ROI 檔 " + path + ": " + std::error_code(errno, std::generic_category()).message();
        ::unlink(tmp.c_str());
        return false;
    }
    fsyncPath(dir.empty() ? "." : dir.string(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return true;
}

RoiService::Result RoiService::set(const std::optional<std::vector<double>>& roiIn, const std::string& ifMatch,
                                   const std::string& remoteIp) {
    std::lock_guard<std::mutex> lock(writeMu_);
    const auto st = store_.status();
    if (st.lastError) return current(409, "規則目前載入失敗，請先修正規則檔: " + *st.lastError);
    if (ifMatch.empty()) return current(400, "缺少 If-Match (送出前讀到的規則雜湊值)");
    if (!st.rules || ifMatch != st.rules->hash) return current(409, "規則或 ROI 已被其他人修改，請重新載入");
    if (store_.roiPath().empty()) return current(500, "沒有設定 ROI 檔路徑");

    if (!roiIn || roiIn->empty()) return current(400, "不能從網頁清除 ROI；要取消 ROI 請修改規則檔並還原網頁設定");
    if (roiIn->size() != 4) return current(400, "roi 必須是 [x, y, w, h] 四個數字");
    const auto& in = *roiIn;
    if (std::string reason; !validateBedRoi(in[0], in[1], in[2], in[3], reason)) return current(400, reason);
    if (in[2] < kMinSize - 1e-9 || in[3] < kMinSize - 1e-9) return current(400, "ROI 的寬與高都不可小於 0.05 (畫面的 5%)");
    // 四捨五入到小數 3 位 (雜湊值不受浮點雜訊影響)；進位可能讓右下角超出 0.001，縮回畫面內
    std::vector<double> roi(4);
    for (size_t i = 0; i < 4; ++i) roi[i] = round3(in[i]);
    if (roi[0] + roi[2] > 1.0) roi[2] = round3(1.0 - roi[0]);
    if (roi[1] + roi[3] > 1.0) roi[3] = round3(1.0 - roi[1]);

    const std::string before = describe(*st.rules);
    std::string error;
    if (!writeRoiFile(roi, remoteIp, error)) return current(500, error);
    if (!store_.reloadNow(error)) return current(409, "ROI 已存檔，但規則載入失敗 (規則修正後生效): " + error);

    auto now = store_.current();
    if (onChanged_) onChanged_("ROI 由網頁設定：" + before + " → " + describe(*now) + "，來源 " + remoteIp);
    return Result{200, "", now};
}

RoiService::Result RoiService::reset(const std::string& ifMatch, const std::string& remoteIp) {
    std::lock_guard<std::mutex> lock(writeMu_);
    const auto st = store_.status();
    if (st.lastError) return current(409, "規則目前載入失敗，請先修正規則檔: " + *st.lastError);
    if (ifMatch.empty()) return current(400, "缺少 If-Match (送出前讀到的規則雜湊值)");
    if (!st.rules || ifMatch != st.rules->hash) return current(409, "規則或 ROI 已被其他人修改，請重新載入");

    const std::string path = store_.roiPath();
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return current(200, "");  // 沒有可還原的設定

    const std::string before = describe(*st.rules);
    if (!fs::remove(path, ec) || ec) return current(500, "無法刪除 ROI 檔 " + path + ": " + ec.message());
    const fs::path dir = fs::path(path).parent_path();
    fsyncPath(dir.empty() ? "." : dir.string(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    std::string error;
    if (!store_.reloadNow(error)) return current(409, "ROI 設定已刪除，但規則載入失敗 (規則修正後生效): " + error);
    auto now = store_.current();
    if (onChanged_) onChanged_("ROI 還原為規則檔：" + before + " → " + describe(*now) + "，來源 " + remoteIp);
    return Result{200, "", now};
}
