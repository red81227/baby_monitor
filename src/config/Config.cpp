#include "config/Config.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace {

// 整串都必須是數字才算有效
bool parseInt(const char* s, long& out) {
    errno = 0;
    char* end = nullptr;
    out = std::strtol(s, &end, 10);
    return *s != '\0' && *end == '\0' && errno == 0;
}

bool parseFloat(const char* s, float& out) {
    errno = 0;
    char* end = nullptr;
    out = std::strtof(s, &end);
    return *s != '\0' && *end == '\0' && errno == 0 && std::isfinite(out);
}

std::string invalid(const char* name, const char* value, const char* expected) {
    return std::string("錯誤: ") + name + " 必須是" + expected + " (目前為 \"" + value + "\")";
}

} // namespace

std::optional<Config> loadConfig(std::string& error) {
    Config cfg;

    if (const char* v = std::getenv("BM_MODEL_PATH")) {
        if (*v == '\0') {
            error = "錯誤: BM_MODEL_PATH 不可為空字串";
            return std::nullopt;
        }
        cfg.modelPath = v;
    }

    if (const char* v = std::getenv("BM_PORT")) {
        long port = 0;
        if (!parseInt(v, port) || port < 1 || port > 65535) {
            error = invalid("BM_PORT", v, " 1-65535 的整數");
            return std::nullopt;
        }
        cfg.port = static_cast<int>(port);
    }

    if (const char* v = std::getenv("BM_MOTION_PX")) {
        float px = 0;
        if (!parseFloat(v, px) || px <= 0) {
            error = invalid("BM_MOTION_PX", v, "正數 (像素)");
            return std::nullopt;
        }
        cfg.motionPx = px;
    }

    if (const char* v = std::getenv("BM_PERSON_CONF")) {
        float conf = 0;
        if (!parseFloat(v, conf) || conf < 0 || conf > 1) {
            error = invalid("BM_PERSON_CONF", v, " 0 到 1 之間的數字");
            return std::nullopt;
        }
        cfg.personConf = conf;
    }

    if (const char* v = std::getenv("BM_CAMERA_DEVICE")) {
        if (*v == '\0') {
            error = "錯誤: BM_CAMERA_DEVICE 不可為空字串";
            return std::nullopt;
        }
        cfg.cameraDevice = v;
    }

    for (auto [name, field] : {std::pair{"BM_CAPTURE_WIDTH", &cfg.captureWidth},
                               std::pair{"BM_CAPTURE_HEIGHT", &cfg.captureHeight}}) {
        if (const char* v = std::getenv(name)) {
            long n = 0;
            if (!parseInt(v, n) || n < 1 || n > 8192) {
                error = invalid(name, v, " 1-8192 的整數 (像素)");
                return std::nullopt;
            }
            *field = static_cast<int>(n);
        }
    }

    for (auto [name, field] : {std::pair{"BM_DETECT_MIN_HZ", &cfg.detectMinHz},
                               std::pair{"BM_DETECT_MAX_HZ", &cfg.detectMaxHz}}) {
        if (const char* v = std::getenv(name)) {
            float hz = 0;
            if (!parseFloat(v, hz) || hz <= 0 || hz > 1000) {
                error = invalid(name, v, "大於 0、不超過 1000 的數字 (每秒次數)");
                return std::nullopt;
            }
            *field = hz;
        }
    }
    if (cfg.detectMinHz > cfg.detectMaxHz) {
        error = "錯誤: BM_DETECT_MIN_HZ (" + std::to_string(cfg.detectMinHz) +
                ") 不可大於 BM_DETECT_MAX_HZ (" + std::to_string(cfg.detectMaxHz) + ")";
        return std::nullopt;
    }

    return cfg;
}
