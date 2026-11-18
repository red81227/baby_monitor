#include "check.h"
#include "config/Config.h"

#include <cstdlib>
#include <string>

namespace {

void clearEnv() {
    for (const char* name : {"BM_MODEL_PATH", "BM_PORT", "BM_MOTION_PX", "BM_PERSON_CONF", "BM_STREAM_PORT",
                             "BM_CAMERA_DEVICE", "BM_CAPTURE_WIDTH", "BM_CAPTURE_HEIGHT",
                             "BM_DETECT_MIN_HZ", "BM_DETECT_MAX_HZ"}) {
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

    // 下限大於上限：錯誤訊息包含 BM_DETECT_MIN_HZ
    clearEnv();
    setenv("BM_DETECT_MIN_HZ", "10", 1);
    setenv("BM_DETECT_MAX_HZ", "2", 1);
    CHECK(!loadConfig(error).has_value());
    CHECK(error.find("BM_DETECT_MIN_HZ") != std::string::npos);

    return 0;
}
