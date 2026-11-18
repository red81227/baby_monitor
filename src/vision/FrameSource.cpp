#include "vision/FrameSource.h"

#include "vision/Jpeg.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

std::string fourccToString(int fourcc) {
    std::string s;
    for (int i = 0; i < 4; ++i) s += static_cast<char>((fourcc >> (8 * i)) & 0xFF);
    return s;
}

} // namespace

V4l2FrameSource::V4l2FrameSource(std::string device, int requestWidth, int requestHeight)
    : device_(std::move(device)), requestWidth_(requestWidth), requestHeight_(requestHeight) {
    // OpenCV V4L2 後端的 select() 逾時預設 10 秒；縮短讓「攝影機卡住」與關閉都能較快反應
    setenv("OPENCV_VIDEOIO_V4L_SELECT_TIMEOUT", "2", 0);
    desc_.device = device_;
}

bool V4l2FrameSource::open() {
    close();
    if (!cap_.open(device_, cv::CAP_V4L2)) {
        error_ = "無法開啟攝影機 " + device_;
        return false;
    }

    // 順序：FOURCC → 寬高 → BUFFERSIZE → CONVERT_RGB (見 design.md D1 的 S1 結論)
    cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, requestWidth_);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, requestHeight_);
    // 2 個 buffer：1 個時驅動在我們讀取期間沒有 buffer 可寫，實測影格率減半 (30 → 15 fps)
    cap_.set(cv::CAP_PROP_BUFFERSIZE, 2);
    cap_.set(cv::CAP_PROP_CONVERT_RGB, 0);

    desc_.fourcc = fourccToString(static_cast<int>(cap_.get(cv::CAP_PROP_FOURCC)));
    desc_.width = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    desc_.height = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    desc_.fps = cap_.get(cv::CAP_PROP_FPS);

    if (desc_.fourcc != "MJPG") {
        error_ = "攝影機 " + device_ + " 無法提供 MJPG 格式 (協商結果為 " + desc_.fourcc + ")";
        cap_.release();
        return false;
    }
    error_.clear();
    return true;
}

void V4l2FrameSource::close() {
    if (cap_.isOpened()) cap_.release();
}

std::chrono::steady_clock::time_point V4l2FrameSource::captureTime() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    auto t = now;

    // V4L2 buffer 時間戳 (毫秒，CLOCK_MONOTONIC)；只在落在 [now - 1s, now] 內時採用
    const double posMs = cap_.get(cv::CAP_PROP_POS_MSEC);
    if (std::isfinite(posMs) && posMs > 0) {
        const steady_clock::time_point ts{duration_cast<steady_clock::duration>(duration<double, std::milli>(posMs))};
        if (ts <= now && now - ts < seconds(1)) {
            t = ts;
        } else if (!warnedTimestamp_) {
            warnedTimestamp_ = true;
            std::cerr << "[WARN] 攝影機 " << device_ << " 的影格時間戳不是 monotonic，改用接收時間\n";
        }
    }

    if (t < lastT_) t = lastT_;  // 保證不倒退
    lastT_ = t;
    return t;
}

std::optional<RawFrame> V4l2FrameSource::grab() {
    if (!cap_.isOpened() || !cap_.grab()) return std::nullopt;
    const auto tCapture = captureTime();

    cv::Mat raw;
    if (!cap_.retrieve(raw) || raw.empty() || raw.depth() != CV_8U) return std::nullopt;

    RawFrame frame;
    frame.tCapture = tCapture;
    const auto* p = raw.ptr<unsigned char>();
    frame.jpeg.assign(p, p + raw.total() * raw.elemSize());
    if (frame.jpeg.size() < 2 || frame.jpeg[0] != 0xFF || frame.jpeg[1] != 0xD8) return std::nullopt;

    if (ensureHuffmanTables(frame.jpeg) && !warnedDht_) {
        warnedDht_ = true;
        std::cerr << "[INFO] 攝影機 " << device_ << " 的 MJPG 沒有 Huffman table，已補上標準表\n";
    }
    frame.width = desc_.width;
    frame.height = desc_.height;
    return frame;
}
