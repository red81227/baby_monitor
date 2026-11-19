#include "vision/ActivityScorer.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

double ActivityScorer::score(const cv::Mat& image, const std::optional<cv::Rect2f>& roi) {
    if (image.empty()) return 0.0;

    cv::Mat src = image;
    if (roi) {
        const cv::Rect area(static_cast<int>(std::lround(roi->x * image.cols)),
                            static_cast<int>(std::lround(roi->y * image.rows)),
                            static_cast<int>(std::lround(roi->width * image.cols)),
                            static_cast<int>(std::lround(roi->height * image.rows)));
        const cv::Rect clipped = area & cv::Rect(0, 0, image.cols, image.rows);
        if (clipped.width < 2 || clipped.height < 2) return 0.0;
        src = image(clipped);
    }
    if (roi != prevRoi_) {
        prev_.release();
        prevRoi_ = roi;
    }

    cv::Mat gray;
    if (src.channels() == 3) {
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = src;
    }

    const int h = std::max(1, static_cast<int>(std::lround(static_cast<double>(kWidth) * gray.rows / gray.cols)));
    cv::Mat small;
    cv::resize(gray, small, cv::Size(kWidth, h), 0, 0, cv::INTER_AREA);

    double result = 0.0;
    if (!prev_.empty() && prev_.size() == small.size()) {
        cv::Mat diff;
        cv::absdiff(small, prev_, diff);
        const int changed = cv::countNonZero(diff > kPixelDiff);
        result = static_cast<double>(changed) / static_cast<double>(small.total());
    }
    prev_ = small;
    return result;
}
