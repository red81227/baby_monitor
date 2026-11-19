#pragma once

#include <crow.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

// 提供 frontend/ 靜態檔案；以執行檔所在目錄為根，與啟動時的工作目錄無關
class StaticController {
public:
    static void registerRoutes(crow::SimpleApp& app) {
        CROW_ROUTE(app, "/")([]() {
            return readAndRespond("index.html", "text/html; charset=utf-8");
        });

        // 前端模組與樣式：只接受 frontend/ 第一層的 <名稱>.js / <名稱>.css (不會讀到其他目錄)
        CROW_ROUTE(app, "/<string>")([](const std::string& name) {
            static const std::regex allowed(R"(^[a-z0-9][a-z0-9-]*\.(js|css)$)");
            std::smatch m;
            if (!std::regex_match(name, m, allowed)) return crow::response(404, "Not Found");
            // ES modules 必須以 JavaScript MIME type 提供
            return readAndRespond(name, m[1] == "js" ? "text/javascript; charset=utf-8" : "text/css; charset=utf-8");
        });
    }

private:
    static const std::filesystem::path& frontendDir() {
        static const std::filesystem::path dir =
            std::filesystem::read_symlink("/proc/self/exe").parent_path() / "frontend";
        return dir;
    }

    static crow::response readAndRespond(const std::string& name, const std::string& contentType) {
        std::ifstream file(frontendDir() / name, std::ios::binary);
        if (!file) {
            return crow::response(404, "Not Found");
        }

        std::ostringstream ss;
        ss << file.rdbuf();

        crow::response res;
        res.code = 200;
        res.set_header("Content-Type", contentType);
        // 更新後重新整理就能拿到新版 (模組彼此 import，不帶版本號)
        res.set_header("Cache-Control", "no-cache");
        res.body = ss.str();
        return res;
    }
};
