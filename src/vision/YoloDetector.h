#pragma once

#include "vision/YoloDecoder.h"

#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>

#include <optional>
#include <string>

// 以 ONNX Runtime 執行 YOLO26n (pose 或純偵測)，回傳影格中信心最高的人物；pose 模型時附上關鍵點
class YoloDetector {
public:
    // 模型不存在、無法載入或輸出形狀不支援時拋出 std::runtime_error (訊息含模型路徑)
    YoloDetector(const std::string& modelPath, float personConf);

    // bgr：要推論的影像 (通常是子碼流)。
    // reduceFactor / fullSize：bgr 是主碼流縮小 reduceFactor 倍的結果時，回傳的框換算成
    // 主碼流座標並限制在 fullSize 內；fullSize 為空時取 bgr 尺寸 x reduceFactor。
    // personCount 不為空時寫入達到信心門檻的人物數量。
    std::optional<Detection> detectPerson(const cv::Mat& bgr, int reduceFactor = 1, cv::Size fullSize = {},
                                          int* personCount = nullptr);

    // 實際使用的執行裝置："CUDA" 或 "CPU"
    const std::string& executionProvider() const { return provider_; }

    // 模型類型："pose" (有關鍵點) 或 "detect"
    const std::string& modelType() const { return modelType_; }

private:
    static constexpr int kInputSize = 640;
    static constexpr int kPersonClass = 0;

    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "BabyMonitor"};
    Ort::Session session_{nullptr};
    Ort::MemoryInfo memInfo_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::string inputName_;
    std::string outputName_;
    std::string provider_;
    std::string modelType_;
    int stride_ = kDetectStride;
    float personConf_;
};
