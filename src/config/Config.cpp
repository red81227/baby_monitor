#include "config/Config.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

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

bool isLocalV4(const in_addr& a) {
    const uint32_t ip = ntohl(a.s_addr);
    return (ip >> 24) == 127                 // 127.0.0.0/8 loopback
           || (ip >> 24) == 10               // 10.0.0.0/8
           || (ip >> 20) == 0xAC1            // 172.16.0.0/12
           || (ip >> 16) == 0xC0A8           // 192.168.0.0/16
           || (ip >> 16) == 0xA9FE;          // 169.254.0.0/16 link-local
}

bool isLocalV6(const in6_addr& a) {
    if (IN6_IS_ADDR_LOOPBACK(&a) || IN6_IS_ADDR_LINKLOCAL(&a)) return true;
    if ((a.s6_addr[0] & 0xFE) == 0xFC) return true;  // fc00::/7 unique local
    if (IN6_IS_ADDR_V4MAPPED(&a)) {
        in_addr v4;
        std::memcpy(&v4, a.s6_addr + 12, 4);
        return isLocalV4(v4);
    }
    return false;
}

} // namespace

bool checkLocalUrl(const std::string& url, std::string& reason) {
    std::string rest;
    if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else if (url.rfind("https://", 0) == 0) {
        rest = url.substr(8);
    } else {
        reason = "必須以 http:// 或 https:// 開頭";
        return false;
    }
    const std::string authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty() || authority.find_first_of(" \t@") != std::string::npos) {
        reason = "網址格式無效";
        return false;
    }

    std::string host, port;
    if (authority[0] == '[') {  // [IPv6]:port
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            reason = "網址格式無效";
            return false;
        }
        host = authority.substr(1, close - 1);
        const std::string after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') {
                reason = "網址格式無效";
                return false;
            }
            port = after.substr(1);
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    if (host.empty()) {
        reason = "網址缺少主機名稱";
        return false;
    }
    if (!port.empty() || authority.back() == ':') {
        long p = 0;
        if (!parseInt(port.c_str(), p) || p < 1 || p > 65535) {
            reason = "port 必須是 1-65535 的整數";
            return false;
        }
    }

    in_addr v4;
    in6_addr v6;
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        if (isLocalV4(v4)) return true;
        reason = "主機 " + host + " 不是本機或私有網路位址";
        return false;
    }
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        if (isLocalV6(v6)) return true;
        reason = "主機 " + host + " 不是本機或私有網路位址";
        return false;
    }

    // 主機名稱：所有解析結果都必須是本機或私有網路位址
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        reason = "無法解析主機 " + host;
        return false;
    }
    bool ok = true;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            ok = ok && isLocalV4(reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr);
        } else if (ai->ai_family == AF_INET6) {
            ok = ok && isLocalV6(reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr);
        }
    }
    freeaddrinfo(res);
    if (!ok) reason = "主機 " + host + " 解析到的位址不是本機或私有網路位址";
    return ok;
}

namespace {

std::optional<std::string> readNonEmpty(const char* name, std::string& error, bool& failed) {
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    if (*v == '\0') {
        error = std::string("錯誤: ") + name + " 不可為空字串";
        failed = true;
        return std::nullopt;
    }
    return std::string(v);
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

    bool failed = false;
    for (auto [name, field] : {std::pair{"BM_RULES_PATH", &cfg.rulesPath}, std::pair{"BM_EVENT_DIR", &cfg.eventDir},
                               std::pair{"BM_LOG_DIR", &cfg.logDir}, std::pair{"BM_OVERRIDES_DIR", &cfg.overridesDir},
                               std::pair{"BM_VLM_MODEL", &cfg.vlmModel}}) {
        if (auto v = readNonEmpty(name, error, failed)) *field = *v;
        if (failed) return std::nullopt;
    }

    // 未設定或空字串 = VLM 停用
    if (const char* v = std::getenv("BM_VLM_URL"); v && *v != '\0') {
        std::string reason;
        if (!checkLocalUrl(v, reason)) {
            error = std::string("錯誤: BM_VLM_URL 無效: ") + reason + " (目前為 \"" + v + "\")";
            return std::nullopt;
        }
        cfg.vlmUrl = v;
    }

    if (const char* v = std::getenv("BM_RING_SECONDS")) {
        long n = 0;
        if (!parseInt(v, n) || n < 1 || n > 3600) {
            error = invalid("BM_RING_SECONDS", v, " 1-3600 的整數 (秒)");
            return std::nullopt;
        }
        cfg.ringSeconds = static_cast<int>(n);
    }

    if (const char* v = std::getenv("BM_EVENT_MAX_MB")) {
        long n = 0;
        if (!parseInt(v, n) || n < 1 || n > 10'000'000) {
            error = invalid("BM_EVENT_MAX_MB", v, "正整數 (MB)");
            return std::nullopt;
        }
        cfg.eventMaxMb = n;
    }

    if (const char* v = std::getenv("BM_STALL_EXIT_SECONDS")) {
        long n = 0;
        if (!parseInt(v, n) || (n != 0 && (n < 10 || n > 3600))) {
            error = invalid("BM_STALL_EXIT_SECONDS", v, " 0 (停用) 或 10-3600 的整數 (秒)");
            return std::nullopt;
        }
        cfg.stallExitSeconds = static_cast<int>(n);
    }

    return cfg;
}
