#include "vision/YoloDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

std::optional<Detection> decodeBestDetection(const float* data, int rows,
                                             float scale, float padX, float padY,
                                             int frameW, int frameH,
                                             float confThreshold, int classId, int stride) {
    std::optional<Detection> best;
    const float* bestRow = nullptr;

    auto toX = [&](float v) { return std::clamp((v - padX) / scale, 0.0f, static_cast<float>(frameW)); };
    auto toY = [&](float v) { return std::clamp((v - padY) / scale, 0.0f, static_cast<float>(frameH)); };

    for (int i = 0; i < rows; ++i) {
        const float* r = data + static_cast<std::ptrdiff_t>(i) * stride;
        const float score = r[4];
        if (static_cast<int>(std::lround(r[5])) != classId || score < confThreshold) continue;
        if (best && score <= best->confidence) continue;

        const float x1 = toX(r[0]), y1 = toY(r[1]);
        const float x2 = toX(r[2]), y2 = toY(r[3]);
        if (x2 <= x1 || y2 <= y1) continue; // 完全落在影格外

        best = Detection{cv::Rect2f(x1, y1, x2 - x1, y2 - y1), score, std::nullopt};
        bestRow = r;
    }

    if (best && stride == kPoseStride) {
        std::array<Keypoint, kKeypointCount> kps;
        const float* k = bestRow + kDetectStride;
        for (int j = 0; j < kKeypointCount; ++j, k += 3) kps[j] = Keypoint{toX(k[0]), toY(k[1]), k[2]};
        best->keypoints = kps;
    }
    return best;
}
