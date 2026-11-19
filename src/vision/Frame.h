#pragma once

#include "vision/YoloDecoder.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

// 擷取到的一張影格：主碼流 = 攝影機輸出的 JPEG 原樣 (不解碼、不重新編碼)
struct RawFrame {
    uint64_t seq = 0;                                // 從 1 開始、嚴格遞增；重連後延續
    std::chrono::steady_clock::time_point tCapture;  // 擷取當下的 monotonic 時間
    std::vector<unsigned char> jpeg;
    int width = 0;  // 主碼流寬高
    int height = 0;
};

// 對某一張影格完成人物偵測與移動判定的結果
struct DetectionResult {
    uint64_t seq = 0;  // 來源影格
    std::chrono::steady_clock::time_point tCapture;
    std::optional<Detection> person;  // 座標以主碼流像素為單位
    int personCount = 0;              // 達到信心門檻的人物數量 (person 為其中信心最高者)
    bool motion = false;
    int width = 0;  // 來源影格的主碼流寬高 (給前端判斷框的座標範圍)
    int height = 0;
};

// steady_clock 時間點轉為微秒 (對外傳送時使用)
inline int64_t toMicros(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}
