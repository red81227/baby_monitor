#pragma once

#include "situation/Signals.h"
#include "vision/Frame.h"

#include <opencv2/core.hpp>

#include <optional>

// 訊號計算用的參數 (來自規則檔，可熱重載)
struct SignalParams {
    float keypointVisibility = 0.5f;      // 關鍵點可見度門檻
    std::optional<cv::Rect2f> bedRoi;     // 嬰兒床區域，以畫面比例表示 (x, y, w, h)
    float bedEdgeMargin = 0.1f;           // 距區域邊界小於「區域寬度 x margin」算靠近邊緣
};

// 從每張分析影格的移動分數與 (有執行時的) 人物偵測結果產生完整的 SignalFrame。
// 偵測只在部分影格執行，兩次偵測之間沿用上一次偵測得到的值；
// 上一次偵測距今超過 kDetectionStale 時，人物相關的訊號都視為未知。
// 時間一律取自影格的擷取時間，不讀系統時鐘 (離線重播時結果相同)。
class SignalExtractor {
public:
    static constexpr int64_t kDetectionStaleUs = 3'000'000;

    // detection 為空表示這張影格沒有執行偵測
    SignalFrame update(uint64_t seq, int64_t tCaptureUs, double activity, const DetectionResult* detection,
                       const SignalParams& params);

private:
    void applyDetection(const DetectionResult& d, int64_t tUs, const SignalParams& params);

    std::optional<int64_t> lastDetectionUs_;
    SignalFrame held_;  // 上一次偵測得到的人物相關訊號

    // 計算 keypoint_motion 用的上一次關鍵點
    std::optional<std::array<Keypoint, kKeypointCount>> prevKeypoints_;
    int64_t prevKeypointsUs_ = 0;
};
