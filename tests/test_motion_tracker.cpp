#include "check.h"
#include "vision/MotionTracker.h"

#include <optional>

namespace {
std::optional<cv::Point2f> at(float x, float y) { return cv::Point2f(x, y); }
} // namespace

int main() {
    // 第一次偵測到人物只建立基準，不算移動
    {
        MotionTracker t(20);
        CHECK(!t.update(at(100, 100)));
        CHECK(!t.update(at(100, 100)));
    }

    // 人物明顯移動：超過門檻算移動，基準點更新為新位置
    {
        MotionTracker t(20);
        t.update(at(100, 100));
        CHECK(t.update(at(130, 100)));
        CHECK(!t.update(at(140, 100))); // 相對新基準 (130) 只移動 10
    }

    // 緩慢移動：每張 2 像素，累積超過 20 的那張才算移動
    {
        MotionTracker t(20);
        t.update(at(0, 0));
        int motionAt = -1;
        for (int i = 1; i <= 15; ++i) {
            if (t.update(at(2.0f * i, 0))) {
                motionAt = i;
                break;
            }
        }
        CHECK(motionAt == 11); // 22 > 20
    }

    // 偵測抖動：一直在門檻內來回，不算移動
    {
        MotionTracker t(20);
        t.update(at(100, 100));
        for (int i = 0; i < 100; ++i) {
            const float dx = (i % 2 == 0) ? 8.0f : -8.0f;
            const float dy = (i % 3 == 0) ? 5.0f : -5.0f;
            CHECK(!t.update(at(100 + dx, 100 + dy)));
        }
    }

    // 門檻是「超過」，剛好等於不算
    {
        MotionTracker t(20);
        t.update(at(0, 0));
        CHECK(!t.update(at(20, 0)));
    }

    // 人物消失：不算移動，並清除基準
    {
        MotionTracker t(20);
        t.update(at(100, 100));
        CHECK(!t.update(std::nullopt));
        // 回到不同位置：只建立新基準，不算移動
        CHECK(!t.update(at(400, 300)));
        // 之後再位移超過門檻才算移動
        CHECK(!t.update(at(410, 300)));
        CHECK(t.update(at(430, 300)));
    }

    // 從沒偵測到人物
    {
        MotionTracker t(20);
        CHECK(!t.update(std::nullopt));
        CHECK(!t.update(std::nullopt));
    }

    return 0;
}
