#include "check.h"
#include "config/Config.h"

#include <cstdlib>
#include <string>

namespace {

void clearEnv() {
    for (const char* name : {"BM_MODEL_PATH", "BM_PORT", "BM_MOTION_PX", "BM_PERSON_CONF", "BM_STREAM_PORT"}) {
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
    CHECK(cfg->modelPath == "models/yolo26n.onnx");
    CHECK(cfg->port == 18080);
    CHECK_NEAR(cfg->motionPx, 20.0f, 1e-6f);
    CHECK_NEAR(cfg->personConf, 0.45f, 1e-6f);

    // 自訂值
    setenv("BM_MODEL_PATH", "/opt/bm/yolo26n.onnx", 1);
    setenv("BM_PORT", "9090", 1);
    setenv("BM_MOTION_PX", "10", 1);
    setenv("BM_PERSON_CONF", "0.3", 1);
    cfg = loadConfig(error);
    CHECK(cfg.has_value());
    CHECK(cfg->modelPath == "/opt/bm/yolo26n.onnx");
    CHECK(cfg->port == 9090);
    CHECK_NEAR(cfg->motionPx, 10.0f, 1e-6f);
    CHECK_NEAR(cfg->personConf, 0.3f, 1e-6f);

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

    return 0;
}
