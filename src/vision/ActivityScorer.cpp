#include "vision/ActivityScorer.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

double ActivityScorer::score(const cv::Mat& image) {
    if (image.empty()) return 0.0;

    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image;
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
