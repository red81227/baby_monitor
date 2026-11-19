#include "check.h"
#include "config/Config.h"

#include <cstdlib>
#include <string>

namespace {

void clearEnv() {
    for (const char* name : {"BM_MODEL_PATH", "BM_PORT", "BM_MOTION_PX", "BM_PERSON_CONF", "BM_STREAM_PORT",
                             "BM_CAMERA_DEVICE", "BM_CAPTURE_WIDTH", "BM_CAPTURE_HEIGHT",
                             "BM_DETECT_MIN_HZ", "BM_DETECT_MAX_HZ", "BM_RULES_PATH", "BM_VLM_URL",
                             "BM_VLM_MODEL", "BM_RING_SECONDS", "BM_EVENT_DIR", "BM_EVENT_MAX_MB", "BM_LOG_DIR", "BM_OVERRIDES_DIR",
                             "BM_STALL_EXIT_SECONDS"}) {
        unsetenv(name);
    }
}

// 只設定一個變數，預期載入失敗且錯誤訊息包含變數名稱
void expectInvalid(const char* name, const char* value) {
    clearEnv();
    setenv(name, value, 1);
    std::string error;
    CHECK(!loadConfig(error).has_value());
    CHECK(error.find(name) != std::string::npos);
}

} // namespace

int main() {
    // 預設值
    clearEnv();
    std::string error;
    auto cfg = loadConfig(error);
    CHECK(cfg.has_value());
    CHECK(cfg->modelPath == "models/yolo26n-pose.onnx");
    CHECK(cfg->port == 18080);
    CHECK_NEAR(cfg->motionPx, 20.0f, 1e-6f);
    CHECK_NEAR(cfg->personConf, 0.45f, 1e-6f);
    CHECK(cfg->cameraDevice == "/dev/video0");
    CHECK(cfg->captureWidth == 1280);
    CHECK(cfg->captureHeight == 720);
    CHECK_NEAR(cfg->detectMinHz, 1.0f, 1e-6f);
    CHECK_NEAR(cfg->detectMaxHz, 10.0f, 1e-6f);
    CHECK(cfg->rulesPath == "config/situations.yaml");
    CHECK(cfg->vlmUrl.empty());  // VLM 停用
    CHECK(cfg->vlmModel.empty());
    CHECK(cfg->ringSeconds == 60);
    CHECK(cfg->eventDir == "events");
    CHECK(cfg->eventMaxMb == 2048);
    CHECK(cfg->logDir == "logs");
    CHECK(cfg->overridesDir == "overrides");
    CHECK(cfg->stallExitSeconds == 0);  // 停滯結束停用

    // 自訂值
    setenv("BM_MODEL_PATH", "/opt/bm/yolo26n.onnx", 1);
    setenv("BM_PORT", "9090", 1);
    setenv("BM_MOTION_PX", "10", 1);
    setenv("BM_PERSON_CONF", "0.3", 1);
    setenv("BM_CAMERA_DEVICE", "/dev/video2", 1);
    setenv("BM_CAPTURE_WIDTH", "1920", 1);
    setenv("BM_CAPTURE_HEIGHT", "1080", 1);
    setenv("BM_DETECT_MIN_HZ", "0.5", 1);
    setenv("BM_DETECT_MAX_HZ", "15", 1);
    cfg = loadConfig(error);
    CHECK(cfg.has_value());
    CHECK(cfg->modelPath == "/opt/bm/yolo26n.onnx");
    CHECK(cfg->port == 9090);
    CHECK_NEAR(cfg->motionPx, 10.0f, 1e-6f);
    CHECK_NEAR(cfg->personConf, 0.3f, 1e-6f);
    CHECK(cfg->cameraDevice == "/dev/video2");
    CHECK(cfg->captureWidth == 1920);
    CHECK(cfg->captureHeight == 1080);
    CHECK_NEAR(cfg->detectMinHz, 0.5f, 1e-6f);
    CHECK_NEAR(cfg->detectMaxHz, 15.0f, 1e-6f);

    // 規則 / VLM / 事件與紀錄
    clearEnv();
    setenv("BM_RULES_PATH", "/etc/bm/rules.yaml", 1);
    setenv("BM_VLM_URL", "http://127.0.0.1:8080", 1);
    setenv("BM_VLM_MODEL", "qwen2.5-vl-3b", 1);
    setenv("BM_RING_SECONDS", "120", 1);
    setenv("BM_EVENT_DIR", "/var/lib/bm/events", 1);
    setenv("BM_EVENT_MAX_MB", "512", 1);
    setenv("BM_LOG_DIR", "/var/log/bm", 1);
    setenv("BM_OVERRIDES_DIR", "/var/lib/bm/overrides", 1);
    cfg = loadConfig(error);
    CHECK(cfg.has_value());
    CHECK(cfg->rulesPath == "/etc/bm/rules.yaml");
    CHECK(cfg->vlmUrl == "http://127.0.0.1:8080");
    CHECK(cfg->vlmModel == "qwen2.5-vl-3b");
    CHECK(cfg->ringSeconds == 120);
    CHECK(cfg->eventDir == "/var/lib/bm/events");
    CHECK(cfg->eventMaxMb == 512);
    CHECK(cfg->logDir == "/var/log/bm");
    CHECK(cfg->overridesDir == "/var/lib/bm/overrides");

    // BM_STALL_EXIT_SECONDS：0 或 10-3600
    for (auto [v, expected] : {std::pair{"0", 0}, std::pair{"10", 10}, std::pair{"60", 60}, std::pair{"3600", 3600}}) {
        clearEnv();
        setenv("BM_STALL_EXIT_SECONDS", v, 1);
        cfg = loadConfig(error);
        CHECK(cfg.has_value() && cfg->stallExitSeconds == expected);
    }

    // BM_VLM_URL 空字串 = 停用
    clearEnv();
    setenv("BM_VLM_URL", "", 1);
    cfg = loadConfig(error);
    CHECK(cfg.has_value() && cfg->vlmUrl.empty());

    // 本機 / 私有網路位址都接受
    for (const char* url : {"http://localhost:11434", "http://127.0.0.1:8080/v1", "https://192.168.1.20",
                            "http://10.0.0.5:8000", "http://172.20.1.1:8080", "http://[::1]:8080",
                            "http://[fd12:3456::1]:8080"}) {
        clearEnv();
        setenv("BM_VLM_URL", url, 1);
        CHECK(loadConfig(error).has_value());
    }

    // 下限等於上限：固定頻率
    clearEnv();
    setenv("BM_DETECT_MIN_HZ", "5", 1);
    setenv("BM_DETECT_MAX_HZ", "5", 1);
    cfg = loadConfig(error);
    CHECK(cfg.has_value());
    CHECK_NEAR(cfg->detectMinHz, 5.0f, 1e-6f);
    CHECK_NEAR(cfg->detectMaxHz, 5.0f, 1e-6f);

    // 只設上限且低於預設下限 (1)：下限大於上限
    clearEnv();
    setenv("BM_DETECT_MAX_HZ", "0.5", 1);
    CHECK(!loadConfig(error).has_value());
    CHECK(error.find("BM_DETECT_MIN_HZ") != std::string::npos);

    // 邊界值仍有效
    clearEnv();
    setenv("BM_PORT", "65535", 1);
    setenv("BM_PERSON_CONF", "1", 1);
    CHECK(loadConfig(error).has_value());

    // BM_STREAM_PORT 已移除，設定了也不影響
    clearEnv();
    setenv("BM_STREAM_PORT", "abc", 1);
    CHECK(loadConfig(error).has_value());

    // 無效值
    expectInvalid("BM_MODEL_PATH", "");
    for (const char* v : {"abc", "0", "65536", "80x", "", "-1"}) expectInvalid("BM_PORT", v);
    for (const char* v : {"abc", "0", "-5", "nan", "inf", ""}) expectInvalid("BM_MOTION_PX", v);
    for (const char* v : {"1.5", "-0.1", "abc", "nan", ""}) expectInvalid("BM_PERSON_CONF", v);
    expectInvalid("BM_CAMERA_DEVICE", "");
    for (const char* name : {"BM_CAPTURE_WIDTH", "BM_CAPTURE_HEIGHT"}) {
        for (const char* v : {"0", "-1", "abc", "720p", "", "1.5", "9000"}) expectInvalid(name, v);
    }
    for (const char* name : {"BM_DETECT_MIN_HZ", "BM_DETECT_MAX_HZ"}) {
        for (const char* v : {"0", "-1", "abc", "nan", "inf", "", "1001"}) expectInvalid(name, v);
    }

    for (const char* name : {"BM_RULES_PATH", "BM_EVENT_DIR", "BM_LOG_DIR", "BM_OVERRIDES_DIR", "BM_VLM_MODEL"}) expectInvalid(name, "");
    // 無效網址、公開網址
    for (const char* v : {"not a url", "ftp://127.0.0.1", "http://", "http://127.0.0.1:0", "http://127.0.0.1:abc",
                          "http://8.8.8.8:8080", "https://1.1.1.1", "http://[2001:4860::8888]:80",
                          "http://user@127.0.0.1"}) {
        expectInvalid("BM_VLM_URL", v);
    }
    for (const char* v : {"0", "-1", "abc", "", "1.5", "3601"}) expectInvalid("BM_RING_SECONDS", v);
    for (const char* v : {"0", "-1", "abc", "", "1.5"}) expectInvalid("BM_EVENT_MAX_MB", v);
    for (const char* v : {"5", "9", "abc", "4000", "3601", "-1", "", "1.5", "60s"}) {
        expectInvalid("BM_STALL_EXIT_SECONDS", v);
    }

    // 下限大於上限：錯誤訊息包含 BM_DETECT_MIN_HZ
    clearEnv();
    setenv("BM_DETECT_MIN_HZ", "10", 1);
    setenv("BM_DETECT_MAX_HZ", "2", 1);
    CHECK(!loadConfig(error).has_value());
    CHECK(error.find("BM_DETECT_MIN_HZ") != std::string::npos);

    return 0;
}
