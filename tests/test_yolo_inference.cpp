#include "check.h"
#include "vision/YoloDetector.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

// 以真實模型跑 tests/data/bus.jpg (810x1080，含多人與一台公車)
//   BM_MODEL_PATH         pose 模型 (必要；找不到時回傳 77，CTest 標記為 skipped)
//   BM_DETECT_MODEL_PATH  純偵測模型 (選用；存在時也一併測試)
namespace {

bool inside(const Keypoint& k, const cv::Rect2f& box, float margin) {
    return k.x >= box.x - margin && k.x <= box.x + box.width + margin &&
           k.y >= box.y - margin && k.y <= box.y + box.height + margin;
}

// 推論時間：暖機後連續 50 次 (D5)，只輸出、不設門檻
void benchmark(YoloDetector& detector, const cv::Mat& image) {
    using clock = std::chrono::steady_clock;
    for (int i = 0; i < 5; ++i) detector.detectPerson(image);
    double total = 0, worst = 0;
    constexpr int kRuns = 50;
    for (int i = 0; i < kRuns; ++i) {
        const auto t0 = clock::now();
        detector.detectPerson(image);
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        total += ms;
        worst = std::max(worst, ms);
    }
    std::cout << "benchmark [" << detector.modelType() << ", " << detector.executionProvider() << ", "
              << image.cols << "x" << image.rows << "]: avg " << total / kRuns << " ms, max " << worst
              << " ms (" << kRuns << " runs, 含前處理與解碼)\n";

    // 前處理 (letterbox + blob) 的時間，與 YoloDetector 相同的步驟；用來估算純推論佔多少
    double pre = 0;
    for (int i = 0; i < kRuns; ++i) {
        const auto t0 = clock::now();
        const float scale = std::min(640.0f / image.cols, 640.0f / image.rows);
        const int w = std::max(1, static_cast<int>(image.cols * scale));
        const int h = std::max(1, static_cast<int>(image.rows * scale));
        cv::Mat padded(640, 640, CV_8UC3, cv::Scalar(114, 114, 114));
        cv::resize(image, padded(cv::Rect(0, 0, w, h)), cv::Size(w, h));
        cv::Mat blob = cv::dnn::blobFromImage(padded, 1.0 / 255.0, cv::Size(), cv::Scalar(), true, false, CV_32F);
        pre += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    }
    std::cout << "  其中前處理 avg " << pre / kRuns << " ms\n";
}

void checkModel(const std::string& modelPath, const cv::Mat& image, bool expectPose) {
    YoloDetector detector(modelPath, 0.45f);
    std::cout << "== " << modelPath << " (provider: " << detector.executionProvider()
              << ", type: " << detector.modelType() << ")\n";
    CHECK(detector.modelType() == (expectPose ? "pose" : "detect"));

    // 直式 810x1080
    int personCount = 0;
    auto person = detector.detectPerson(image, 1, {}, &personCount);
    CHECK(person.has_value());
    // bus.jpg 中有多個人
    std::cout << "personCount=" << personCount << "\n";
    CHECK(personCount > 1);
    std::cout << "person conf=" << person->confidence << " box=" << person->box << "\n";
    CHECK(person->confidence >= 0.45f);
    CHECK(person->box.x >= 0 && person->box.y >= 0);
    CHECK(person->box.x + person->box.width <= 810.0f);
    CHECK(person->box.y + person->box.height <= 1080.0f);
    // 人物框應該是直立的 (高大於寬)，且有一定大小
    CHECK(person->box.height > person->box.width);
    CHECK(person->box.height > 200.0f);

    if (expectPose) {
        // 17 個關鍵點都在影像內；可見度高的點大多落在框內
        CHECK(person->keypoints.has_value());
        int visible = 0, visibleInBox = 0;
        for (const auto& k : *person->keypoints) {
            CHECK(k.x >= 0 && k.x <= 810.0f && k.y >= 0 && k.y <= 1080.0f);
            CHECK(k.visibility >= 0.0f && k.visibility <= 1.0f);
            if (k.visibility >= 0.5f) {
                ++visible;
                if (inside(k, person->box, 5.0f)) ++visibleInBox;
            }
        }
        std::cout << "keypoints visible=" << visible << " inBox=" << visibleInBox << "\n";
        CHECK(visible >= 8);
        CHECK(visibleInBox * 10 >= visible * 9);
    } else {
        CHECK(!person->keypoints.has_value());
    }

    // 橫式：把同一張圖縮小貼到 1280x720 的畫布右側，框應該跟著移動並縮放
    cv::Mat canvas(720, 1280, CV_8UC3, cv::Scalar(30, 30, 30));
    cv::Mat small;
    cv::resize(image, small, cv::Size(540, 720)); // 縮為 2/3
    small.copyTo(canvas(cv::Rect(700, 0, 540, 720)));
    auto person2 = detector.detectPerson(canvas);
    CHECK(person2.has_value());
    std::cout << "landscape person conf=" << person2->confidence << " box=" << person2->box << "\n";
    CHECK(person2->box.x >= 700.0f - 5.0f);
    CHECK(person2->box.x + person2->box.width <= 1240.0f + 5.0f);

    // 子碼流推論、主碼流座標：以 reduced decode 縮小 1/2 (405x540) 推論，框換回 810x1080
    std::vector<unsigned char> encoded;
    CHECK(cv::imencode(".jpg", image, encoded));
    const cv::Mat half = cv::imdecode(encoded, cv::IMREAD_REDUCED_COLOR_2);
    CHECK(half.cols == 405 && half.rows == 540);
    auto personHalf = detector.detectPerson(half, 2, image.size());
    CHECK(personHalf.has_value());
    std::cout << "half-res person conf=" << personHalf->confidence << " box=" << personHalf->box << "\n";
    CHECK(personHalf->box.x >= 0 && personHalf->box.y >= 0);
    CHECK(personHalf->box.x + personHalf->box.width <= 810.0f);
    CHECK(personHalf->box.y + personHalf->box.height <= 1080.0f);
    // 與全尺寸推論的結果大致一致 (IoU > 0.7)
    const cv::Rect2f inter = person->box & personHalf->box;
    const float iou = inter.area() / (person->box.area() + personHalf->box.area() - inter.area());
    std::cout << "IoU(full, half) = " << iou << "\n";
    CHECK(iou > 0.7f);
    if (expectPose) {
        // 兩邊都清楚可見的關鍵點，位置相差不超過數個像素
        CHECK(personHalf->keypoints.has_value());
        float worst = 0;
        for (int j = 0; j < kKeypointCount; ++j) {
            const auto& a = (*person->keypoints)[j];
            const auto& b = (*personHalf->keypoints)[j];
            if (a.visibility < 0.5f || b.visibility < 0.5f) continue;
            worst = std::max(worst, std::hypot(a.x - b.x, a.y - b.y));
        }
        std::cout << "max keypoint distance(full, half) = " << worst << " px\n";
        CHECK(worst < 10.0f);
    }
    // 沒有給 fullSize 時以 bgr 尺寸 x 倍率為範圍
    auto personHalf2 = detector.detectPerson(half, 2);
    CHECK(personHalf2.has_value());
    CHECK(personHalf2->box.x + personHalf2->box.width <= 810.0f);

    // 全黑畫面：沒有人物
    cv::Mat black(480, 640, CV_8UC3, cv::Scalar(0, 0, 0));
    CHECK(!detector.detectPerson(black).has_value());

    // 推論時間：實際運作時推論的是 1280x720 主碼流縮小 2 倍的 640x360 子碼流
    cv::Mat sub;
    cv::resize(canvas, sub, cv::Size(640, 360));
    benchmark(detector, sub);
}

} // namespace

int main() {
    const char* modelEnv = std::getenv("BM_MODEL_PATH");
    const std::string modelPath = modelEnv ? modelEnv : "models/yolo26n-pose.onnx";
    if (!std::filesystem::is_regular_file(modelPath)) {
        std::cout << "SKIP: 找不到模型 " << modelPath << "\n";
        return 77;
    }

    const char* imageEnv = std::getenv("BM_TEST_IMAGE");
    const cv::Mat image = cv::imread(imageEnv ? imageEnv : "tests/data/bus.jpg");
    CHECK(!image.empty());
    CHECK(image.cols == 810 && image.rows == 1080);

    checkModel(modelPath, image, true);

    const char* detectEnv = std::getenv("BM_DETECT_MODEL_PATH");
    if (detectEnv && std::filesystem::is_regular_file(detectEnv)) {
        checkModel(detectEnv, image, false);
    } else {
        std::cout << "(略過純偵測模型：找不到 BM_DETECT_MODEL_PATH)\n";
    }

    // 模型不存在：例外訊息包含路徑
    try {
        YoloDetector missing("/nonexistent.onnx", 0.45f);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("/nonexistent.onnx") != std::string::npos);
    }

    return 0;
}
