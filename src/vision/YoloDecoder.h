#pragma once

#include <opencv2/core.hpp>

#include <optional>

struct Detection {
    cv::Rect2f box; // 原始影格座標
    float confidence;

    cv::Point2f center() const { return {box.x + box.width / 2, box.y + box.height / 2}; }
};

// YOLO26 (內含 NMS) 的輸出解碼。
// data: rows x 6，每列為 x1, y1, x2, y2, score, class，座標在 letterbox 輸入空間。
// 原始座標 = (letterbox 座標 - pad) / scale，並 clamp 到 frameW x frameH 內。
// 回傳 classId 類別中、score >= confThreshold 且分數最高的一筆；沒有則回傳 nullopt。
std::optional<Detection> decodeBestDetection(const float* data, int rows,
                                             float scale, float padX, float padY,
                                             int frameW, int frameH,
                                             float confThreshold, int classId = 0);
