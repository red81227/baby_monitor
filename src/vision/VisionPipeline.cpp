#include "vision/VisionPipeline.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <iostream>

VisionPipeline::VisionPipeline(MonitorState& state, YoloDetector& detector, float motionPx,
                               std::function<void()> onCameraLost)
    : state_(state), detector_(detector), tracker_(motionPx), onCameraLost_(std::move(onCameraLost)) {}

VisionPipeline::~VisionPipeline() {
    stop();
}

bool VisionPipeline::openCamera(int cameraIndex) {
    if (!cap_.open(cameraIndex)) return false;
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    return true;
}

void VisionPipeline::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&VisionPipeline::processLoop, this);
}

void VisionPipeline::stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    if (cap_.isOpened()) cap_.release();
}

void VisionPipeline::processLoop() {
    const cv::Scalar green(0, 200, 0), red(0, 0, 255);
    const std::vector<int> jpegParams{cv::IMWRITE_JPEG_QUALITY, 80};

    while (running_.load()) {
        cv::Mat frame;
        cap_ >> frame; // 阻塞到下一張影格
        if (frame.empty()) {
            if (!running_.load()) break;
            std::cerr << "錯誤: 攝影機中斷，無法再取得影格\n";
            cameraLost_.store(true);
            if (onCameraLost_) onCameraLost_();
            break;
        }

        const auto person = detector_.detectPerson(frame);
        const bool motion = tracker_.update(person ? std::optional(person->center()) : std::nullopt);
        state_.updateDetection(person.has_value(), motion);

        if (person) {
            const cv::Scalar color = motion ? red : green;
            cv::rectangle(frame, cv::Rect(person->box), color, 2);
            cv::putText(frame, cv::format("person %.2f", person->confidence),
                        cv::Point(static_cast<int>(person->box.x), std::max(15, static_cast<int>(person->box.y) - 5)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
        }
        if (motion) {
            cv::putText(frame, "MOTION", cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1, red, 2);
        }

        std::vector<unsigned char> jpeg;
        if (cv::imencode(".jpg", frame, jpeg, jpegParams)) {
            state_.updateFrameJpeg(std::move(jpeg));
        }
    }
}
