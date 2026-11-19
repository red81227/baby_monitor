#pragma once

#include <opencv2/core.hpp>

#include <optional>

// 移動分數：把子碼流縮成約 160 寬的灰階小圖，與上一張分析影格做絕對差，
// |diff| > kPixelDiff 的像素比例即為分數 (0–1)。第一張或尺寸改變時分數為 0。
class ActivityScorer {
public:
    static constexpr int kWidth = 160;
    static constexpr int kPixelDiff = 25;
    static constexpr double kActiveThreshold = 0.01;  // 1% 像素變化即視為有動靜

    // image：BGR 或灰階的子碼流
    // roi：只計算此區域 (以畫面比例表示的 x, y, w, h)；區域改變時重新建立基準，分數為 0
    double score(const cv::Mat& image, const std::optional<cv::Rect2f>& roi = std::nullopt);

private:
    cv::Mat prev_;
    std::optional<cv::Rect2f> prevRoi_;
};
