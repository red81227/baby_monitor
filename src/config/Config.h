#pragma once

#include <optional>
#include <string>

// 所有執行期設定 (來自 BM_* 環境變數)
struct Config {
    std::string modelPath = "models/yolo26n-pose.onnx"; // BM_MODEL_PATH
    int port = 18080;                              // BM_PORT
    float motionPx = 20.0f;                        // BM_MOTION_PX
    float personConf = 0.45f;                      // BM_PERSON_CONF
    std::string cameraDevice = "/dev/video0";      // BM_CAMERA_DEVICE
    int captureWidth = 1280;                       // BM_CAPTURE_WIDTH (要求值，實際以攝影機協商為準)
    int captureHeight = 720;                       // BM_CAPTURE_HEIGHT
    float detectMinHz = 1.0f;                      // BM_DETECT_MIN_HZ (畫面靜止時的人物偵測頻率)
    float detectMaxHz = 10.0f;                     // BM_DETECT_MAX_HZ (有動靜時的人物偵測頻率)
};

// 讀取並驗證環境變數；任一值無效時回傳 nullopt，並把錯誤訊息寫入 error
std::optional<Config> loadConfig(std::string& error);
