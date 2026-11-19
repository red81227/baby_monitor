#include "check.h"
#include "situation/SignalExtractor.h"

#include <cmath>

namespace {

constexpr int64_t kSec = 1'000'000;

// 1280x720 影格中、框為 (400,200)-(700,600) 的人物；所有關鍵點預設可見度 0.9
DetectionResult person(float vis = 0.9f, float dx = 0) {
    DetectionResult r;
    r.width = 1280;
    r.height = 720;
    r.personCount = 1;
    Detection d{cv::Rect2f(400 + dx, 200, 300, 400), 0.8f, std::nullopt};
    std::array<Keypoint, kKeypointCount> kps;
    for (int i = 0; i < kKeypointCount; ++i) kps[i] = Keypoint{450.0f + dx + i * 10, 250.0f + i * 15, vis};
    d.keypoints = kps;
    r.person = d;
    return r;
}

DetectionResult nobody() {
    DetectionResult r;
    r.width = 1280;
    r.height = 720;
    return r;
}

bool isTrue(const std::optional<double>& v) { return v && *v == 1.0; }
bool isFalse(const std::optional<double>& v) { return v && *v == 0.0; }

} // namespace

int main() {
    SignalParams params;

    // 清楚的人物：臉、身體都可見，不像趴睡
    {
        SignalExtractor ex;
        const auto d = person();
        const SignalFrame f = ex.update(1, 10 * kSec, 0.02, &d, params);
        CHECK(f.seq == 1 && f.tCaptureUs == 10 * kSec);
        CHECK(isTrue(f[Signal::PersonPresent]));
        CHECK(f[Signal::PersonCount] == 1.0);
        CHECK_NEAR(*f[Signal::PersonConf], 0.8, 1e-6);
        CHECK(isTrue(f[Signal::FaceVisible]));
        CHECK(isTrue(f[Signal::BodyVisible]));
        CHECK(isFalse(f[Signal::ProneLikely]));
        CHECK(!f[Signal::KeypointMotion]);  // 第一次偵測，沒有比較對象
        CHECK(!f[Signal::NearBedEdge]);     // 沒有設定嬰兒床區域
        CHECK_NEAR(*f[Signal::Activity], 0.02, 1e-9);
    }

    // 臉被遮住：鼻、雙眼低於門檻，雙肩可見
    {
        SignalExtractor ex;
        auto d = person();
        for (int i : {0, 1, 2, 3, 4}) (*d.person->keypoints)[i].visibility = 0.1f;
        const SignalFrame f = ex.update(1, 0, 0, &d, params);
        CHECK(isFalse(f[Signal::FaceVisible]));
        CHECK(isTrue(f[Signal::BodyVisible]));
        CHECK(isFalse(f[Signal::ProneLikely]));  // 耳朵也看不到

        // 只看得到耳朵：可能是趴睡
        (*d.person->keypoints)[3].visibility = 0.7f;
        const SignalFrame g = ex.update(2, 100'000, 0, &d, params);
        CHECK(isTrue(g[Signal::ProneLikely]));

        // 可見度門檻由規則檔調整
        SignalParams strict;
        strict.keypointVisibility = 0.95f;
        auto clear = person(0.9f);
        const SignalFrame h = SignalExtractor().update(1, 0, 0, &clear, strict);
        CHECK(isFalse(h[Signal::FaceVisible]));
        CHECK(isFalse(h[Signal::BodyVisible]));
    }

    // 沒有人物：person_present false、person_count 0，其餘人物訊號未知
    {
        SignalExtractor ex;
        const auto d = nobody();
        const SignalFrame f = ex.update(1, 0, 0.5, &d, params);
        CHECK(isFalse(f[Signal::PersonPresent]));
        CHECK(f[Signal::PersonCount] == 0.0);
        for (Signal s : {Signal::PersonConf, Signal::FaceVisible, Signal::BodyVisible, Signal::ProneLikely,
                         Signal::KeypointMotion, Signal::NearBedEdge}) {
            CHECK(!f[s]);
        }
        CHECK(f[Signal::Activity] == 0.5);
    }

    // 純偵測模型：依賴關鍵點的訊號未知，其他照常
    {
        SignalExtractor ex;
        auto d = person();
        d.person->keypoints.reset();
        d.personCount = 2;
        const SignalFrame f = ex.update(1, 0, 0.1, &d, params);
        CHECK(isTrue(f[Signal::PersonPresent]));
        CHECK(f[Signal::PersonCount] == 2.0);
        CHECK(f[Signal::PersonConf].has_value());
        CHECK(f[Signal::Activity].has_value());
        for (Signal s : {Signal::FaceVisible, Signal::BodyVisible, Signal::ProneLikely, Signal::KeypointMotion}) {
            CHECK(!f[s]);
        }
    }

    // 嬰兒床區域：人物中心 (550, 400) = 畫面比例 (0.43, 0.556)
    {
        SignalParams p;
        p.bedRoi = cv::Rect2f(0.2f, 0.2f, 0.6f, 0.7f);  // 中心離最近的邊 0.23 > 0.06
        const auto d = person();
        CHECK(isFalse(SignalExtractor().update(1, 0, 0, &d, p)[Signal::NearBedEdge]));

        p.bedRoi = cv::Rect2f(0.4f, 0.2f, 0.5f, 0.7f);  // 離左邊 0.03 < 0.05
        CHECK(isTrue(SignalExtractor().update(1, 0, 0, &d, p)[Signal::NearBedEdge]));

        p.bedRoi = cv::Rect2f(0.6f, 0.0f, 0.4f, 1.0f);  // 中心在區域外
        CHECK(isTrue(SignalExtractor().update(1, 0, 0, &d, p)[Signal::NearBedEdge]));
    }

    // keypoint_motion：所有點平移 50 px、框對角線 500 px、間隔 0.5 秒 → 0.1 / 0.5 = 0.2 每秒
    {
        SignalExtractor ex;
        const auto a = person();
        const auto b = person(0.9f, 50);
        ex.update(1, 0, 0, &a, params);
        const SignalFrame f = ex.update(2, kSec / 2, 0, &b, params);
        CHECK_NEAR(*f[Signal::KeypointMotion], 0.2, 1e-6);

        // 兩次偵測之間的影格沿用上一次的值
        const SignalFrame between = ex.update(3, kSec / 2 + 100'000, 0.03, nullptr, params);
        CHECK_NEAR(*between[Signal::KeypointMotion], 0.2, 1e-6);
        CHECK(isTrue(between[Signal::PersonPresent]));
        CHECK_NEAR(*between[Signal::Activity], 0.03, 1e-9);

        // 不可見的點不計入
        auto c = person(0.9f, 50);
        for (int i = 0; i < 16; ++i) (*c.person->keypoints)[i].visibility = 0.1f;
        (*c.person->keypoints)[16].x += 100;  // 只有最後一點可見，移動 100 px
        const SignalFrame g = ex.update(4, kSec, 0, &c, params);
        CHECK_NEAR(*g[Signal::KeypointMotion], 100.0 / 500.0 / 0.5, 1e-6);

        // 中間沒有人物：重新開始
        const auto none = nobody();
        ex.update(5, kSec + 100'000, 0, &none, params);
        const SignalFrame h = ex.update(6, kSec + 200'000, 0, &b, params);
        CHECK(!h[Signal::KeypointMotion]);
    }

    // 過期：上一次偵測距今超過 3 秒，人物訊號都視為未知
    {
        SignalExtractor ex;
        const auto d = person();
        ex.update(1, 0, 0, &d, params);
        CHECK(ex.update(2, 3 * kSec, 0, nullptr, params)[Signal::PersonPresent].has_value());
        const SignalFrame f = ex.update(3, 3 * kSec + 1, 0.2, nullptr, params);
        CHECK(!f[Signal::PersonPresent]);
        CHECK(!f[Signal::FaceVisible]);
        CHECK(f[Signal::Activity] == 0.2);

        // 間隔超過 3 秒的兩次偵測不計算 keypoint_motion
        const auto moved = person(0.9f, 50);
        CHECK(!ex.update(4, 4 * kSec, 0, &moved, params)[Signal::KeypointMotion]);
    }

    // 名稱對照
    CHECK(signalByName("face_visible") == Signal::FaceVisible);
    CHECK(!signalByName("face_visable"));
    CHECK(signalInfo(Signal::Activity).name == "activity");
    CHECK(!signalInfo(Signal::Activity).isBool);

    return 0;
}
