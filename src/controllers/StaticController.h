#pragma once

#include <crow.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// 提供 frontend/ 靜態檔案；以執行檔所在目錄為根，與啟動時的工作目錄無關
class StaticController {
public:
    static void registerRoutes(crow::SimpleApp& app) {
        CROW_ROUTE(app, "/")([]() {
            return readAndRespond("index.html", "text/html; charset=utf-8");
        });

        CROW_ROUTE(app, "/app.js")([]() {
            return readAndRespond("app.js", "application/javascript; charset=utf-8");
        });

        CROW_ROUTE(app, "/style.css")([]() {
            return readAndRespond("style.css", "text/css; charset=utf-8");
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
        res.body = ss.str();
        return res;
    }
};
