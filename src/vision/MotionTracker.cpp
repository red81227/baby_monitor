#include "vision/MotionTracker.h"

#include <cmath>

bool MotionTracker::update(const std::optional<cv::Point2f>& personCenter) {
    if (!personCenter) {
        anchor_.reset();
        return false;
    }
    if (!anchor_) {
        anchor_ = personCenter;
        return false;
    }

    const cv::Point2f d = *personCenter - *anchor_;
    if (std::hypot(d.x, d.y) > motionPx_) {
        anchor_ = personCenter;
        return true;
    }
    return false;
}
