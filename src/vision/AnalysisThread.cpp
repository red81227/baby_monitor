#include "vision/AnalysisThread.h"

#include "vision/Jpeg.h"

#include <opencv2/imgcodecs.hpp>

#include <iostream>

namespace {

int decodeFlag(int factor) {
    switch (factor) {
        case 2: return cv::IMREAD_REDUCED_COLOR_2;
        case 4: return cv::IMREAD_REDUCED_COLOR_4;
        case 8: return cv::IMREAD_REDUCED_COLOR_8;
        default: return cv::IMREAD_COLOR;
    }
}

} // namespace

AnalysisThread::AnalysisThread(LatestSlot<RawFrame>& frames, YoloDetector& detector, MonitorState& state,
                               Heartbeat& heartbeat, float motionPx, double minHz, double maxHz,
                               ResultCallback onResult, AnalysisCallback onAnalysis, RoiProvider activityRoi)
    : frames_(frames),
      detector_(detector),
      state_(state),
      heartbeat_(heartbeat),
      tracker_(motionPx),
      scheduler_(minHz, maxHz, ActivityScorer::kActiveThreshold),
      onResult_(std::move(onResult)),
      onAnalysis_(std::move(onAnalysis)),
      activityRoi_(std::move(activityRoi)) {}

AnalysisThread::~AnalysisThread() {
    stop();
}

void AnalysisThread::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&AnalysisThread::run, this);
}

void AnalysisThread::stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

void AnalysisThread::run() {
    using Clock = std::chrono::steady_clock;
    uint64_t lastVersion = 0;

    while (running_.load()) {
        // 沒有新影格時也定期醒來：更新進度與偵測頻率 (攝影機中斷時 detectHz 會逐漸降到 0)
        const auto entry = frames_.waitNewerThan(lastVersion, Clock::now() + std::chrono::milliseconds(500));
        heartbeat_.beat();
        if (!entry.value) {
            if (frames_.closed()) break;
            state_.setDetectHz(scheduler_.actualHz(Clock::now()));
            continue;
        }
        lastVersion = entry.version;
        const RawFrame& frame = *entry.value;

        // 子碼流：解碼時直接縮小
        const int factor = reducedDecodeFactor(frame.width, frame.height);
        const cv::Mat sub = cv::imdecode(frame.jpeg, decodeFlag(factor));
        if (sub.empty()) {
            std::cerr << "[WARN] 影格 seq=" << frame.seq << " 無法解碼，略過\n";
            continue;
        }
        const cv::Size fullSize = frame.width > 0 ? cv::Size(frame.width, frame.height)
                                                  : cv::Size(sub.cols * factor, sub.rows * factor);

        auto now = Clock::now();
        const double activity = scorer_.score(sub, activityRoi_ ? activityRoi_() : std::nullopt);
        scheduler_.onScore(activity, now);

        DetectionResult result;
        bool detected = false;
        if (scheduler_.shouldDetect(now)) {
            detected = true;
            result.seq = frame.seq;
            result.tCapture = frame.tCapture;
            result.width = fullSize.width;
            result.height = fullSize.height;
            result.person = detector_.detectPerson(sub, factor, fullSize, &result.personCount);
            result.motion = tracker_.update(result.person ? std::optional(result.person->center()) : std::nullopt);

            now = Clock::now();
            scheduler_.onDetected(now);
            state_.updateDetection(result.person.has_value(), result.motion);
            if (onResult_) onResult_(result);
        }
        if (onAnalysis_) onAnalysis_(frame.seq, toMicros(frame.tCapture), activity, detected ? &result : nullptr);
        state_.setDetectHz(scheduler_.actualHz(now));
    }
}
