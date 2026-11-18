#include "check.h"
#include "vision/Jpeg.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstddef>
#include <vector>

namespace {

// 移除所有 DHT 段，模擬省略 Huffman table 的 UVC 攝影機
std::vector<unsigned char> stripDht(const std::vector<unsigned char>& in) {
    std::vector<unsigned char> out(in.begin(), in.begin() + 2);
    std::size_t i = 2;
    while (i + 4 <= in.size()) {
        const unsigned char marker = in[i + 1];
        if (marker == 0xDA) break;
        const std::size_t len = (static_cast<std::size_t>(in[i + 2]) << 8) | in[i + 3];
        if (marker != 0xC4) out.insert(out.end(), in.begin() + i, in.begin() + i + 2 + len);
        i += 2 + len;
    }
    out.insert(out.end(), in.begin() + i, in.end());
    return out;
}

bool hasDht(const std::vector<unsigned char>& j) {
    for (std::size_t i = 0; i + 1 < j.size(); ++i)
        if (j[i] == 0xFF && j[i + 1] == 0xC4) return true;
    return false;
}

} // namespace

int main() {
    // 縮小倍率：長邊 < 1280 不縮小；否則長邊仍 >= 640 的最大倍率
    CHECK(reducedDecodeFactor(640, 480) == 1);
    CHECK(reducedDecodeFactor(640, 360) == 1);
    CHECK(reducedDecodeFactor(1280, 720) == 2);   // 640x360
    CHECK(reducedDecodeFactor(1920, 1080) == 2);  // 960x540 (1/4 = 480 < 640)
    CHECK(reducedDecodeFactor(2560, 1440) == 4);  // 640x360
    CHECK(reducedDecodeFactor(3840, 2160) == 4);  // 960x540
    CHECK(reducedDecodeFactor(5120, 2880) == 8);  // 640x360
    CHECK(reducedDecodeFactor(720, 1280) == 2);   // 直式以長邊判斷
    CHECK(reducedDecodeFactor(1279, 720) == 1);

    // 以 libjpeg 標準表編碼 (OpenCV 預設不做最佳化 Huffman)，再移除 DHT
    cv::Mat img(240, 320, CV_8UC3);
    cv::randu(img, cv::Scalar::all(0), cv::Scalar::all(255));
    cv::rectangle(img, cv::Rect(40, 40, 100, 80), cv::Scalar(10, 200, 30), cv::FILLED);
    std::vector<unsigned char> original;
    CHECK(cv::imencode(".jpg", img, original, {cv::IMWRITE_JPEG_QUALITY, 85}));
    CHECK(hasDht(original));

    // 已有 DHT：不修改
    auto unchanged = original;
    CHECK(!ensureHuffmanTables(unchanged));
    CHECK(unchanged == original);

    // 沒有 DHT：無法解碼；補上標準表後解碼結果與原檔完全相同
    auto stripped = stripDht(original);
    CHECK(!hasDht(stripped));
    CHECK(stripped.size() < original.size());
    CHECK(ensureHuffmanTables(stripped));
    CHECK(hasDht(stripped));
    const cv::Mat a = cv::imdecode(original, cv::IMREAD_COLOR);
    const cv::Mat b = cv::imdecode(stripped, cv::IMREAD_COLOR);
    CHECK(!b.empty());
    CHECK(a.size() == b.size());
    CHECK(cv::norm(a, b, cv::NORM_INF) == 0);
    // 補過一次後不再重複插入
    CHECK(!ensureHuffmanTables(stripped));

    // 不是 JPEG：不修改
    std::vector<unsigned char> junk{1, 2, 3, 4, 5};
    CHECK(!ensureHuffmanTables(junk));
    CHECK((junk == std::vector<unsigned char>{1, 2, 3, 4, 5}));

    // reduced decode：1280x720 以 1/2 解碼得到 640x360，長寬比不變
    cv::Mat big(720, 1280, CV_8UC3, cv::Scalar(50, 100, 150));
    std::vector<unsigned char> bigJpeg;
    CHECK(cv::imencode(".jpg", big, bigJpeg));
    const cv::Mat sub = cv::imdecode(bigJpeg, cv::IMREAD_REDUCED_COLOR_2);
    CHECK(sub.cols == 640 && sub.rows == 360);

    return 0;
}
