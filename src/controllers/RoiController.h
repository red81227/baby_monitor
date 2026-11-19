#pragma once

#include "roi/RoiService.h"

#include <crow.h>

#include <cmath>
#include <optional>
#include <string>

// ROI 寫入端點：PUT /api/roi (設定)、DELETE /api/roi (還原為規則檔)。
// 這是監視頁面唯一會修改設定的地方；之後加上認證時只需要保護這裡。
//
// 沒有認證期間的防護 (只擋跨站請求，不擋同網段的人)：
// - 只接受 Content-Type: application/json (跨站表單與 no-cors fetch 只能送簡單請求)
// - 有 Origin 時必須與 Host 相同
// - 請求內容上限 4 KiB
class RoiController {
public:
    static constexpr size_t kMaxBodyBytes = 4096;

    static crow::json::wvalue roiValue(const std::optional<cv::Rect2f>& r) {
        if (!r) return crow::json::wvalue(nullptr);
        std::vector<crow::json::wvalue> v;
        // float 輸出較短 (0.1 而不是 0.100000000000000005551)；數值已四捨五入到小數 3 位
        for (float x : {r->x, r->y, r->width, r->height}) v.emplace_back(std::round(x * 1000.0f) / 1000.0f);
        return crow::json::wvalue(std::move(v));
    }

    // /api/situations 與寫入端點共用的 ROI 欄位
    static void fillRoi(crow::json::wvalue& j, const Rules* rules) {
        j["bedRoi"] = rules ? roiValue(rules->params.bedRoi) : crow::json::wvalue(nullptr);
        j["bedRoiSource"] = rules ? toString(rules->bedRoiSource) : "none";
        j["fileBedRoi"] = rules ? roiValue(rules->fileBedRoi) : crow::json::wvalue(nullptr);
        j["bedEdgeMargin"] = rules ? rules->params.bedEdgeMargin : 0.1f;
    }

    static void registerRoutes(crow::SimpleApp& app, RoiService& service) {
        CROW_ROUTE(app, "/api/roi").methods(crow::HTTPMethod::Put, crow::HTTPMethod::Delete)(
            [&service](const crow::request& req) {
                if (auto rejected = guard(req)) return std::move(*rejected);
                const std::string ifMatch = unquote(req.get_header_value("If-Match"));
                const std::string ip = req.remote_ip_address;
                if (req.method == crow::HTTPMethod::Delete) return respond(service.reset(ifMatch, ip));

                std::optional<std::vector<double>> roi;
                std::string error;
                if (!RoiService::parseBody(req.body, roi, error)) return errorResponse(400, error);
                return respond(service.set(roi, ifMatch, ip));
            });
    }

private:
    static crow::response errorResponse(int code, const std::string& message) {
        crow::json::wvalue j;
        j["error"] = message;
        crow::response res(code, j);
        res.set_header("Cache-Control", "no-store");
        return res;
    }

    // 跨站與格式檢查；通過時回傳 nullopt
    static std::optional<crow::response> guard(const crow::request& req) {
        if (!RoiService::sameOrigin(req.get_header_value("Origin"), req.get_header_value("Host"))) {
            return errorResponse(403, "拒絕跨站請求");
        }
        if (req.body.size() > kMaxBodyBytes) return errorResponse(413, "請求內容太大");
        if (req.method == crow::HTTPMethod::Put) {
            std::string type = req.get_header_value("Content-Type");
            type = type.substr(0, type.find(';'));
            while (!type.empty() && type.back() == ' ') type.pop_back();
            for (auto& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (type != "application/json") return errorResponse(415, "Content-Type 必須是 application/json");
        }
        return std::nullopt;
    }

    // If-Match 可以帶引號 ("abc123")
    static std::string unquote(std::string s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
        return s;
    }

    static crow::response respond(const RoiService::Result& r) {
        crow::json::wvalue j;
        fillRoi(j, r.rules.get());
        j["hash"] = r.rules ? r.rules->hash : "";
        j["version"] = r.rules ? r.rules->version : 0;
        if (!r.error.empty()) j["error"] = r.error;
        crow::response res(r.status, j);
        res.set_header("Cache-Control", "no-store");
        if (r.rules) res.set_header("ETag", "\"" + r.rules->hash + "\"");
        return res;
    }
};
