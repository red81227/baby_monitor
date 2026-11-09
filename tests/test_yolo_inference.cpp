#include "check.h"
#include "vision/YoloDetector.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

// 以真實模型跑 tests/data/bus.jpg (810x1080，含多人與一台公車)
// 找不到模型時回傳 77，CTest 標記為 skipped
int main() {
    const char* modelEnv = std::getenv("BM_MODEL_PATH");
    const std::string modelPath = modelEnv ? modelEnv : "models/yolo26n.onnx";
    if (!std::filesystem::is_regular_file(modelPath)) {
        std::cout << "SKIP: 找不到模型 " << modelPath << "\n";
        return 77;
    }

    const char* imageEnv = std::getenv("BM_TEST_IMAGE");
    const cv::Mat image = cv::imread(imageEnv ? imageEnv : "tests/data/bus.jpg");
    CHECK(!image.empty());
    CHECK(image.cols == 810 && image.rows == 1080);

    YoloDetector detector(modelPath, 0.45f);
    std::cout << "execution provider: " << detector.executionProvider() << "\n";

    // 直式 810x1080
    auto person = detector.detectPerson(image);
    CHECK(person.has_value());
    std::cout << "person conf=" << person->confidence << " box=" << person->box << "\n";
    CHECK(person->confidence >= 0.45f);
    CHECK(person->box.x >= 0 && person->box.y >= 0);
    CHECK(person->box.x + person->box.width <= 810.0f);
    CHECK(person->box.y + person->box.height <= 1080.0f);
    // 人物框應該是直立的 (高大於寬)，且有一定大小
    CHECK(person->box.height > person->box.width);
    CHECK(person->box.height > 200.0f);

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

    // 全黑畫面：沒有人物
    cv::Mat black(480, 640, CV_8UC3, cv::Scalar(0, 0, 0));
    CHECK(!detector.detectPerson(black).has_value());

    // 模型不存在：例外訊息包含路徑
    try {
        YoloDetector missing("/nonexistent.onnx", 0.45f);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("/nonexistent.onnx") != std::string::npos);
    }

    return 0;
}
