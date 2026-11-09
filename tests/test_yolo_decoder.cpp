#include "check.h"
#include "vision/YoloDecoder.h"

#include <vector>

namespace {

constexpr int kRows = 300;

struct Output {
    std::vector<float> data = std::vector<float>(kRows * 6, 0.0f);
    void set(int row, float x1, float y1, float x2, float y2, float score, float cls) {
        float* r = &data[row * 6];
        r[0] = x1; r[1] = y1; r[2] = x2; r[3] = y2; r[4] = score; r[5] = cls;
    }
};

std::optional<Detection> decode(const Output& o, float scale = 1.0f, float padX = 0, float padY = 0,
                                int w = 640, int h = 640, float conf = 0.45f) {
    return decodeBestDetection(o.data.data(), kRows, scale, padX, padY, w, h, conf);
}

} // namespace

int main() {
    // 全部為 0 (模型沒有輸出任何東西)：沒有人物
    {
        Output o;
        CHECK(!decode(o).has_value());
    }

    // 非 person 類別被忽略 (例如 class 5 = bus)，即使分數很高
    {
        Output o;
        o.set(0, 10, 10, 200, 200, 0.95f, 5);
        CHECK(!decode(o).has_value());
    }

    // 低於門檻被忽略
    {
        Output o;
        o.set(0, 10, 10, 100, 100, 0.40f, 0);
        CHECK(!decode(o).has_value());
        CHECK(decode(o, 1, 0, 0, 640, 640, 0.40f).has_value()); // 剛好等於門檻時保留
    }

    // 取分數最高的一筆 (不論順序)
    {
        Output o;
        o.set(0, 0, 0, 50, 50, 0.60f, 0);
        o.set(1, 100, 100, 300, 400, 0.90f, 0);
        o.set(2, 400, 400, 500, 500, 0.70f, 0);
        auto d = decode(o);
        CHECK(d.has_value());
        CHECK_NEAR(d->confidence, 0.90f, 1e-6f);
        CHECK_NEAR(d->box.x, 100.0f, 1e-4f);
        CHECK_NEAR(d->box.y, 100.0f, 1e-4f);
        CHECK_NEAR(d->box.width, 200.0f, 1e-4f);
        CHECK_NEAR(d->box.height, 300.0f, 1e-4f);
        CHECK_NEAR(d->center().x, 200.0f, 1e-4f);
        CHECK_NEAR(d->center().y, 250.0f, 1e-4f);
    }

    // scale 與 pad 轉換回原始座標：1280x960 縮到 640x480 (scale 0.5)，上方 pad 80
    {
        Output o;
        o.set(0, 100, 180, 300, 380, 0.8f, 0);
        auto d = decode(o, 0.5f, 0, 80, 1280, 960);
        CHECK(d.has_value());
        CHECK_NEAR(d->box.x, 200.0f, 1e-3f);
        CHECK_NEAR(d->box.y, 200.0f, 1e-3f);
        CHECK_NEAR(d->box.width, 400.0f, 1e-3f);
        CHECK_NEAR(d->box.height, 400.0f, 1e-3f);
    }

    // 超出影格範圍時 clamp
    {
        Output o;
        o.set(0, -20, -10, 700, 500, 0.8f, 0);
        auto d = decode(o, 1.0f, 0, 0, 640, 480);
        CHECK(d.has_value());
        CHECK_NEAR(d->box.x, 0.0f, 1e-6f);
        CHECK_NEAR(d->box.y, 0.0f, 1e-6f);
        CHECK_NEAR(d->box.x + d->box.width, 640.0f, 1e-4f);
        CHECK_NEAR(d->box.y + d->box.height, 480.0f, 1e-4f);
    }

    // 完全落在影格外 (例如 letterbox 補邊區) 的框被忽略
    {
        Output o;
        o.set(0, 10, 500, 100, 600, 0.9f, 0); // 640x480 影格的下方補邊區
        CHECK(!decode(o, 1.0f, 0, 0, 640, 480).has_value());
    }

    return 0;
}
