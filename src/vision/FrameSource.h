#pragma once

#include "vision/Frame.h"

#include <opencv2/videoio.hpp>

#include <optional>
#include <string>

// 影像來源介面：隱藏攝影機 / 擷取方式的細節。
// grab() 回傳的 RawFrame 尚未指定 seq (由 CaptureThread 指定)。
class FrameSource {
public:
    struct Description {
        std::string device;
        std::string fourcc;
        int width = 0;
        int height = 0;
        double fps = 0;
    };

    virtual ~FrameSource() = default;

    // 失敗時回傳 false，lastError() 為含裝置路徑的訊息
    virtual bool open() = 0;
    virtual void close() = 0;
    // 阻塞到下一張影格；攝影機中斷或讀取失敗時回傳 nullopt
    virtual std::optional<RawFrame> grab() = 0;
    virtual Description describe() const = 0;
    virtual const std::string& lastError() const = 0;
};

// 以 OpenCV V4L2 後端取得攝影機輸出的原始 MJPG (CONVERT_RGB=0)，不解碼。
// tCapture 使用 V4L2 buffer 的時間戳 (CLOCK_MONOTONIC，與 steady_clock 同時間軸)；
// 時間戳不可用或不合理時退回 steady_clock::now()，並保證不倒退。
class V4l2FrameSource : public FrameSource {
public:
    V4l2FrameSource(std::string device, int requestWidth, int requestHeight);

    bool open() override;
    void close() override;
    std::optional<RawFrame> grab() override;
    Description describe() const override { return desc_; }
    const std::string& lastError() const override { return error_; }

private:
    std::chrono::steady_clock::time_point captureTime();

    std::string device_;
    int requestWidth_;
    int requestHeight_;
    cv::VideoCapture cap_;
    Description desc_;
    std::string error_;
    std::chrono::steady_clock::time_point lastT_{};
    bool warnedTimestamp_ = false;
    bool warnedDht_ = false;
};
