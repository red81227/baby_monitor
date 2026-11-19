#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <optional>

// COCO 17 個關鍵點 (鼻、左右眼、左右耳、左右肩、左右肘、左右腕、左右髖、左右膝、左右踝)
constexpr int kKeypointCount = 17;

struct Keypoint {
    float x = 0;          // 原始影格座標
    float y = 0;
    float visibility = 0; // 0–1
};

struct Detection {
    cv::Rect2f box; // 原始影格座標
    float confidence;
    std::optional<std::array<Keypoint, kKeypointCount>> keypoints; // 純偵測模型時為空

    cv::Point2f center() const { return {box.x + box.width / 2, box.y + box.height / 2}; }
};

// 每列欄數：純偵測模型 6 欄；pose 模型 57 欄 (6 欄之後接 17 組 x, y, 可見度)
constexpr int kDetectStride = 6;
constexpr int kPoseStride = kDetectStride + kKeypointCount * 3;

// YOLO26 (內含 NMS) 的輸出解碼。
// data: rows x stride，每列開頭為 x1, y1, x2, y2, score, class，座標在 letterbox 輸入空間；
// stride 為 kPoseStride 時接著 17 組關鍵點，以與框相同的換算與 clamp 轉成原始座標。
// 原始座標 = (letterbox 座標 - pad) / scale，並 clamp 到 frameW x frameH 內。
// 回傳 classId 類別中、score >= confThreshold 且分數最高的一筆；沒有則回傳 nullopt。
// count 不為空時寫入符合條件 (類別、門檻、框不完全落在影格外) 的筆數；模型內含 NMS，同一人不重複計數。
std::optional<Detection> decodeBestDetection(const float* data, int rows,
                                             float scale, float padX, float padY,
                                             int frameW, int frameH,
                                             float confThreshold, int classId = 0,
                                             int stride = kDetectStride, int* count = nullptr);
