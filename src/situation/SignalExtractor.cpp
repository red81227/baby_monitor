#include "situation/SignalExtractor.h"

#include <algorithm>
#include <cmath>

namespace {

// COCO 關鍵點索引
constexpr int kNose = 0, kLeftEye = 1, kRightEye = 2, kLeftEar = 3, kRightEar = 4;
constexpr int kLeftShoulder = 5, kRightShoulder = 6, kLeftHip = 11, kRightHip = 12;

double b(bool v) { return v ? 1.0 : 0.0; }

// 人物相關 (偵測時才更新) 的訊號
constexpr Signal kPersonSignals[] = {Signal::PersonPresent, Signal::PersonCount, Signal::PersonConf,
                                     Signal::FaceVisible,   Signal::BodyVisible, Signal::ProneLikely,
                                     Signal::KeypointMotion, Signal::NearBedEdge};

} // namespace

void SignalExtractor::applyDetection(const DetectionResult& d, int64_t tUs, const SignalParams& params) {
    for (Signal s : kPersonSignals) held_[s].reset();
    lastDetectionUs_ = tUs;

    held_[Signal::PersonPresent] = b(d.person.has_value());
    held_[Signal::PersonCount] = d.person ? std::max(1, d.personCount) : 0;
    if (!d.person) {
        prevKeypoints_.reset();
        return;
    }
    const Detection& p = *d.person;
    held_[Signal::PersonConf] = p.confidence;

    // 靠近嬰兒床邊緣：需要區域與影格尺寸
    if (params.bedRoi && d.width > 0 && d.height > 0) {
        const cv::Point2f c = p.center();
        const float cx = c.x / static_cast<float>(d.width);
        const float cy = c.y / static_cast<float>(d.height);
        const cv::Rect2f& r = *params.bedRoi;
        const bool inside = cx >= r.x && cx <= r.x + r.width && cy >= r.y && cy <= r.y + r.height;
        const float edgeDist = std::min({cx - r.x, r.x + r.width - cx, cy - r.y, r.y + r.height - cy});
        held_[Signal::NearBedEdge] = b(!inside || edgeDist < r.width * params.bedEdgeMargin);
    }

    if (!p.keypoints) {  // 純偵測模型
        prevKeypoints_.reset();
        return;
    }
    const auto& k = *p.keypoints;
    const float thr = params.keypointVisibility;
    auto vis = [&](int i) { return k[i].visibility >= thr; };

    const bool face = vis(kNose) || vis(kLeftEye) || vis(kRightEye);
    const bool body = vis(kLeftShoulder) || vis(kRightShoulder) || vis(kLeftHip) || vis(kRightHip);
    held_[Signal::FaceVisible] = b(face);
    held_[Signal::BodyVisible] = b(body);
    held_[Signal::ProneLikely] = b(body && !face && (vis(kLeftEar) || vis(kRightEar)));

    // 關鍵點移動：兩次都可見的點的平均位移 / 人物框對角線 / 秒
    if (prevKeypoints_ && tUs > prevKeypointsUs_ && tUs - prevKeypointsUs_ <= kDetectionStaleUs) {
        const auto& prev = *prevKeypoints_;
        double sum = 0;
        int n = 0;
        for (int i = 0; i < kKeypointCount; ++i) {
            if (k[i].visibility >= thr && prev[i].visibility >= thr) {
                sum += std::hypot(k[i].x - prev[i].x, k[i].y - prev[i].y);
                ++n;
            }
        }
        const double diag = std::hypot(p.box.width, p.box.height);
        const double dt = static_cast<double>(tUs - prevKeypointsUs_) / 1e6;
        if (n > 0 && diag > 0) held_[Signal::KeypointMotion] = (sum / n) / diag / dt;
    }
    prevKeypoints_ = k;
    prevKeypointsUs_ = tUs;
}

SignalFrame SignalExtractor::update(uint64_t seq, int64_t tCaptureUs, double activity,
                                    const DetectionResult* detection, const SignalParams& params) {
    if (detection) applyDetection(*detection, tCaptureUs, params);

    SignalFrame out;
    out.seq = seq;
    out.tCaptureUs = tCaptureUs;
    out[Signal::Activity] = activity;

    const bool fresh = lastDetectionUs_ && tCaptureUs - *lastDetectionUs_ <= kDetectionStaleUs;
    if (fresh) {
        for (Signal s : kPersonSignals) out[s] = held_[s];
    }
    return out;
}
