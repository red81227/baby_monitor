#pragma once

#include <optional>
#include <string>

// 所有執行期設定 (來自 BM_* 環境變數)
struct Config {
    std::string modelPath = "models/yolo26n.onnx"; // BM_MODEL_PATH
    int port = 18080;                              // BM_PORT
    float motionPx = 20.0f;                        // BM_MOTION_PX
    float personConf = 0.45f;                      // BM_PERSON_CONF
};

// 讀取並驗證環境變數；任一值無效時回傳 nullopt，並把錯誤訊息寫入 error
std::optional<Config> loadConfig(std::string& error);
