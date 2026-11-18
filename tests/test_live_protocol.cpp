#include "check.h"
#include "vision/LiveProtocol.h"

#include <crow/json.h>

#include <array>
#include <cstring>
#include <string>

int main() {
    using namespace std::chrono;

    RawFrame f;
    f.seq = 0x0102030405060708ULL;
    f.tCapture = steady_clock::time_point(microseconds(123456789));
    f.jpeg = {0xFF, 0xD8, 0xAB, 0xFF, 0xD9};
    f.width = 1280;
    f.height = 720;

    // binary：20 bytes 標頭 (little-endian) + JPEG 原樣
    const std::string msg = live::encodeFrame(f);
    CHECK(msg.size() == live::kFrameHeaderSize + f.jpeg.size());
    uint32_t magic = 0;
    uint64_t seq = 0, t = 0;
    std::memcpy(&magic, msg.data(), 4);  // 測試機為 little-endian (x86_64)
    std::memcpy(&seq, msg.data() + 4, 8);
    std::memcpy(&t, msg.data() + 12, 8);
    CHECK(magic == live::kFrameMagic);
    CHECK(std::string(msg.data(), 4) == "FBM1");
    CHECK(seq == f.seq);
    CHECK(t == 123456789ULL);
    CHECK(std::memcmp(msg.data() + 20, f.jpeg.data(), f.jpeg.size()) == 0);

    // text：有人物
    DetectionResult r;
    r.seq = 42;
    r.tCapture = steady_clock::time_point(microseconds(1000));
    r.person = Detection{cv::Rect2f(10.0f, 20.5f, 100.0f, 200.25f), 0.875f, std::nullopt};
    r.motion = true;
    r.width = 1280;
    r.height = 720;
    CHECK(live::encodeDetection(r) ==
          "{\"type\":\"detection\",\"seq\":42,\"tCaptureUs\":1000,\"person\":true,"
          "\"box\":[10.0,20.5,100.0,200.2],\"confidence\":0.875,\"motion\":true,\"width\":1280,\"height\":720,"
          "\"keypoints\":null}");

    // text：有人物且有關鍵點 (pose 模型)；既有欄位與沒有關鍵點時完全相同
    {
        std::array<Keypoint, kKeypointCount> kps;
        for (int j = 0; j < kKeypointCount; ++j) {
            kps[j] = Keypoint{100.0f + j, 200.25f + j, j / 16.0f};
        }
        kps[16] = Keypoint{1280.0f, 720.0f, 1.0f};
        r.person->keypoints = kps;
        const std::string json = live::encodeDetection(r);
        const std::string prefix =
            "{\"type\":\"detection\",\"seq\":42,\"tCaptureUs\":1000,\"person\":true,"
            "\"box\":[10.0,20.5,100.0,200.2],\"confidence\":0.875,\"motion\":true,\"width\":1280,\"height\":720,"
            "\"keypoints\":[";
        CHECK(json.compare(0, prefix.size(), prefix) == 0);
        CHECK(json.find("[100.0,200.2,0.00],[101.0,201.2,0.06],[102.0,202.2,0.12]") != std::string::npos);
        const std::string tail = ",[1280.0,720.0,1.00]]}";
        CHECK(json.size() > tail.size() && json.compare(json.size() - tail.size(), tail.size(), tail) == 0);

        // 合法 JSON，欄位內容正確
        const auto v = crow::json::load(json);
        CHECK(static_cast<bool>(v));
        CHECK(v["seq"].i() == 42);
        CHECK(v["person"].b());
        CHECK(v["box"].size() == 4);
        CHECK(v["keypoints"].size() == static_cast<std::size_t>(kKeypointCount));
        for (int j = 0; j < kKeypointCount; ++j) {
            CHECK(v["keypoints"][j].size() == 3);
            CHECK_NEAR(v["keypoints"][j][0].d(), kps[j].x, 0.051);
            CHECK_NEAR(v["keypoints"][j][1].d(), kps[j].y, 0.051);
            CHECK_NEAR(v["keypoints"][j][2].d(), kps[j].visibility, 0.0051);
        }
        r.person->keypoints.reset();
    }

    // text：沒有人物
    r.person.reset();
    r.motion = false;
    {
        const auto v = crow::json::load(live::encodeDetection(r));
        CHECK(static_cast<bool>(v));
        CHECK(!v["person"].b());
        CHECK(v["box"].t() == crow::json::type::Null);
        CHECK(v["keypoints"].t() == crow::json::type::Null);
    }
    CHECK(live::encodeDetection(r) ==
          "{\"type\":\"detection\",\"seq\":42,\"tCaptureUs\":1000,\"person\":false,"
          "\"box\":null,\"confidence\":0.000,\"motion\":false,\"width\":1280,\"height\":720,"
          "\"keypoints\":null}");

    return 0;
}
