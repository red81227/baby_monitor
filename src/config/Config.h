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
    std::string rulesPath = "config/situations.yaml";  // BM_RULES_PATH (狀況規則檔)
    std::string vlmUrl;                            // BM_VLM_URL (空字串 = VLM 停用；只接受本機或私有網路位址)
    std::string vlmModel;                          // BM_VLM_MODEL
    int ringSeconds = 60;                          // BM_RING_SECONDS (主碼流環形緩衝長度)
    std::string eventDir = "events";               // BM_EVENT_DIR (事件片段目錄)
    long eventMaxMb = 2048;                        // BM_EVENT_MAX_MB (事件片段空間上限)
    std::string logDir = "logs";                   // BM_LOG_DIR (觀測 / 警報紀錄目錄)
    std::string overridesDir = "overrides";        // BM_OVERRIDES_DIR (網頁設定的保存目錄，目前只有 ROI)
    int stallExitSeconds = 0;                      // BM_STALL_EXIT_SECONDS (0 = 停用；擷取或分析停滯超過此秒數就結束程式)
};

// 讀取並驗證環境變數；任一值無效時回傳 nullopt，並把錯誤訊息寫入 error
std::optional<Config> loadConfig(std::string& error);

// BM_VLM_URL 的檢查：http(s)://host[:port][/path]，host 必須是 loopback 或私有網路位址
// (名稱會解析，所有解析結果都必須符合)。不符合時回傳 false 並寫入原因。
bool checkLocalUrl(const std::string& url, std::string& reason);
