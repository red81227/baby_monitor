#pragma once

#include <opencv2/core.hpp>

// 移動分數：把子碼流縮成約 160 寬的灰階小圖，與上一張分析影格做絕對差，
// |diff| > kPixelDiff 的像素比例即為分數 (0–1)。第一張或尺寸改變時分數為 0。
class ActivityScorer {
public:
    static constexpr int kWidth = 160;
    static constexpr int kPixelDiff = 25;
    static constexpr double kActiveThreshold = 0.01;  // 1% 像素變化即視為有動靜

    // image：BGR 或灰階的子碼流
    double score(const cv::Mat& image);

private:
    cv::Mat prev_;
};
