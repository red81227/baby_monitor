#pragma once

#include <opencv2/core.hpp>

#include <optional>

// 以「人物中心相對基準點的累積位移」判定移動。
// - 沒有人物：清除基準點，不算移動
// - 沒有基準點：以目前位置為基準，不算移動 (啟動與人物重新出現時)
// - 與基準點距離 > motionPx：算移動，基準點移到目前位置
class MotionTracker {
public:
    explicit MotionTracker(float motionPx) : motionPx_(motionPx) {}

    bool update(const std::optional<cv::Point2f>& personCenter);

private:
    float motionPx_;
    std::optional<cv::Point2f> anchor_;
};
