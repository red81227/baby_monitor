#include "vision/YoloDetector.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

Ort::SessionOptions baseOptions() {
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return opts;
}

} // namespace

YoloDetector::YoloDetector(const std::string& modelPath, float personConf)
    : personConf_(personConf) {
    if (!std::filesystem::is_regular_file(modelPath)) {
        throw std::runtime_error("無法載入模型 " + modelPath + ": 檔案不存在");
    }

    // 先嘗試 CUDA，失敗就改用 CPU
    try {
        Ort::SessionOptions opts = baseOptions();
        OrtCUDAProviderOptions cuda{};
        opts.AppendExecutionProvider_CUDA(cuda);
        session_ = Ort::Session(env_, modelPath.c_str(), opts);
        provider_ = "CUDA";
    } catch (const Ort::Exception& e) {
        std::cerr << "[WARN] 無法使用 CUDA 推論，改用 CPU (速度較慢): " << e.what() << "\n";
        try {
            Ort::SessionOptions opts = baseOptions();
            session_ = Ort::Session(env_, modelPath.c_str(), opts);
            provider_ = "CPU";
        } catch (const Ort::Exception& e2) {
            throw std::runtime_error("無法載入模型 " + modelPath + ": " + e2.what());
        }
    }

    Ort::AllocatorWithDefaultOptions allocator;
    inputName_ = session_.GetInputNameAllocated(0, allocator).get();
    outputName_ = session_.GetOutputNameAllocated(0, allocator).get();

    // 確認是 YOLO26 的 [1, N, 6] 輸出格式
    auto outShape = session_.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (outShape.size() != 3 || outShape[2] != 6) {
        throw std::runtime_error("模型 " + modelPath + " 的輸出格式不是 YOLO26 的 [1, N, 6]");
    }

    std::cout << "[INFO] YOLO 模型已載入: " << modelPath << " (執行裝置: " << provider_ << ")\n";
}

std::optional<Detection> YoloDetector::detectPerson(const cv::Mat& bgr) {
    // Letterbox：等比例縮放，靠左上放置，其餘補灰 (pad = 0)
    const float scale = std::min(static_cast<float>(kInputSize) / bgr.cols,
                                 static_cast<float>(kInputSize) / bgr.rows);
    const int newW = std::max(1, static_cast<int>(bgr.cols * scale));
    const int newH = std::max(1, static_cast<int>(bgr.rows * scale));

    cv::Mat padded(kInputSize, kInputSize, CV_8UC3, cv::Scalar(114, 114, 114));
    cv::resize(bgr, padded(cv::Rect(0, 0, newW, newH)), cv::Size(newW, newH));

    // BGR -> RGB、HWC -> NCHW、正規化到 [0, 1]
    cv::Mat blob = cv::dnn::blobFromImage(padded, 1.0 / 255.0, cv::Size(), cv::Scalar(), true, false, CV_32F);

    const std::array<int64_t, 4> inputShape{1, 3, kInputSize, kInputSize};
    Ort::Value input = Ort::Value::CreateTensor<float>(
        memInfo_, blob.ptr<float>(), blob.total(), inputShape.data(), inputShape.size());

    const char* inName = inputName_.c_str();
    const char* outName = outputName_.c_str();
    auto outputs = session_.Run(Ort::RunOptions{nullptr}, &inName, &input, 1, &outName, 1);

    const auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    return decodeBestDetection(outputs[0].GetTensorData<float>(), static_cast<int>(shape[1]),
                               scale, 0.0f, 0.0f, bgr.cols, bgr.rows, personConf_, kPersonClass);
}
