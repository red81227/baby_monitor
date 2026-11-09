#pragma once

#include "vision/YoloDecoder.h"

#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>

#include <optional>
#include <string>

// 以 ONNX Runtime 執行 YOLO26n，回傳影格中信心最高的人物
class YoloDetector {
public:
    // 模型不存在或無法載入時拋出 std::runtime_error (訊息含模型路徑)
    YoloDetector(const std::string& modelPath, float personConf);

    std::optional<Detection> detectPerson(const cv::Mat& bgr);

    // 實際使用的執行裝置："CUDA" 或 "CPU"
    const std::string& executionProvider() const { return provider_; }

private:
    static constexpr int kInputSize = 640;
    static constexpr int kPersonClass = 0;

    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "BabyMonitor"};
    Ort::Session session_{nullptr};
    Ort::MemoryInfo memInfo_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::string inputName_;
    std::string outputName_;
    std::string provider_;
    float personConf_;
};
