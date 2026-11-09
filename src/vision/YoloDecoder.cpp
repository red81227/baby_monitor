#include "vision/YoloDecoder.h"

#include <algorithm>
#include <cmath>

std::optional<Detection> decodeBestDetection(const float* data, int rows,
                                             float scale, float padX, float padY,
                                             int frameW, int frameH,
                                             float confThreshold, int classId) {
    std::optional<Detection> best;

    for (int i = 0; i < rows; ++i) {
        const float* r = data + i * 6;
        const float score = r[4];
        if (static_cast<int>(std::lround(r[5])) != classId || score < confThreshold) continue;
        if (best && score <= best->confidence) continue;

        auto toX = [&](float v) { return std::clamp((v - padX) / scale, 0.0f, static_cast<float>(frameW)); };
        auto toY = [&](float v) { return std::clamp((v - padY) / scale, 0.0f, static_cast<float>(frameH)); };
        const float x1 = toX(r[0]), y1 = toY(r[1]);
        const float x2 = toX(r[2]), y2 = toY(r[3]);
        if (x2 <= x1 || y2 <= y1) continue; // 完全落在影格外

        best = Detection{cv::Rect2f(x1, y1, x2 - x1, y2 - y1), score};
    }

    return best;
}
