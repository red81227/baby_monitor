#include "check.h"
#include "events/RingBuffer.h"

namespace {

constexpr int64_t kSec = 1'000'000;

RingBuffer::FramePtr frame(uint64_t seq, int64_t tUs) {
    auto f = std::make_shared<RawFrame>();
    f->seq = seq;
    f->tCapture = std::chrono::steady_clock::time_point(std::chrono::microseconds(tUs));
    f->jpeg = {0xFF, 0xD8, static_cast<unsigned char>(seq), 0xFF, 0xD9};
    return f;
}

} // namespace

int main() {
    RingBuffer ring(60 * kSec);
    CHECK(!ring.newestUs());
    CHECK(ring.range(0, 100 * kSec).empty());

    // 30 fps 送 10 分鐘：只保留最近約 60 秒 (1800 張左右)
    const int64_t step = kSec / 30;
    uint64_t seq = 0;
    for (int64_t t = 0; t <= 600 * kSec; t += step) ring.push(frame(++seq, t));
    CHECK(ring.size() >= 1800 && ring.size() <= 1802);
    CHECK(*ring.newestUs() == (600 * kSec / step) * step);

    // 依時間範圍取出，依時間排序
    const int64_t newest = *ring.newestUs();
    auto clip = ring.range(newest - 20 * kSec, newest);
    CHECK(clip.size() >= 599 && clip.size() <= 601);
    for (size_t i = 1; i < clip.size(); ++i) CHECK(clip[i]->tCapture > clip[i - 1]->tCapture);
    CHECK(toMicros(clip.front()->tCapture) >= newest - 20 * kSec);
    CHECK(clip.back()->seq == seq);

    // 範圍超出緩衝：從緩衝中最早的影格開始
    auto all = ring.range(0, newest);
    CHECK(all.size() == ring.size());
    CHECK(toMicros(all.front()->tCapture) >= newest - 60 * kSec);

    // 共用指標：取出的影格與緩衝中的是同一份資料
    CHECK(all.back().get() == clip.back().get());

    // 範圍內沒有影格 (例如攝影機中斷)：回傳空的
    CHECK(ring.range(newest + 1, newest + 10 * kSec).empty());

    // 中間缺一段：只回傳實際有的影格
    RingBuffer gap(60 * kSec);
    gap.push(frame(1, 0));
    gap.push(frame(2, kSec));
    gap.push(frame(3, 10 * kSec));  // 1–10 秒間攝影機中斷
    auto g = gap.range(0, 10 * kSec);
    CHECK(g.size() == 3 && g[1]->seq == 2 && g[2]->seq == 3);

    return 0;
}
