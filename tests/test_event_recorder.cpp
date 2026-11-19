#include "check.h"
#include "common/TimeUtil.h"
#include "events/EventRecorder.h"

#include <atomic>
#include <filesystem>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSec = 1'000'000;
int64_t g_base = 0;  // 以目前的 monotonic 時間為基準，讓片段 id 的日期合理

RingBuffer::FramePtr frame(uint64_t seq, int64_t tUs, size_t bytes = 1000) {
    auto f = std::make_shared<RawFrame>();
    f->seq = seq;
    f->tCapture = std::chrono::steady_clock::time_point(std::chrono::microseconds(g_base + tUs));
    f->jpeg.assign(bytes, static_cast<unsigned char>(seq));
    return f;
}

void fill(RingBuffer& ring, uint64_t& seq, int64_t from, int64_t to, size_t bytes = 1000) {
    for (int64_t t = from; t < to; t += kSec / 10) ring.push(frame(++seq, t, bytes));
}

EventRecorder::Request req(const std::string& situation, int64_t tUs, int64_t pre = 20 * kSec,
                           int64_t post = 5 * kSec) {
    return EventRecorder::Request{situation, g_base + tUs, pre, post, 3, "abc123"};
}

} // namespace

int main() {
    g_base = nowMonoUs() - 1000 * kSec;
    char tmpl[] = "/tmp/bm_test_events_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    const fs::path dir = fs::path(tmpl) / "events";

    // 取出片段：等到片段結束時間後才產生，內容為 [t-20s, t+5s] 的影格、依時間排序
    {
        RingBuffer ring(60 * kSec);
        EventRecorder rec(ring, dir.string(), 1'000'000'000);
        uint64_t seq = 0;
        fill(ring, seq, 0, 42 * kSec);

        std::optional<EventRecorder::Clip> done;
        const std::string id = rec.request(req("face_covered", 40 * kSec), [&](const auto& c) { done = c; });
        CHECK(EventRecorder::isValidId(id));
        CHECK(id.find("-face_covered-001") != std::string::npos);

        rec.processDue(0);
        CHECK(!done);  // 還沒到 t+5s
        fill(ring, seq, 42 * kSec, 46 * kSec);
        rec.processDue(0);
        CHECK(done && done->written && done->id == id);
        CHECK(done->frames.size() == 251);  // 20s..45s @ 10fps (含兩端)
        CHECK(toMicros(done->frames.front()->tCapture) == g_base + 20 * kSec);
        CHECK(toMicros(done->frames.back()->tCapture) == g_base + 45 * kSec);
        for (size_t i = 1; i < done->frames.size(); ++i) {
            CHECK(done->frames[i]->tCapture > done->frames[i - 1]->tCapture);
        }

        // 磁碟上的片段
        CHECK(fs::exists(dir / id / "000001.jpg"));
        CHECK(fs::exists(dir / id / "000251.jpg"));
        CHECK(!fs::exists(dir / id / "000252.jpg"));
        const auto meta = rec.readMeta(id);
        CHECK(meta && meta->find("\"situation\":\"face_covered\"") != std::string::npos);
        CHECK(meta->find("\"rulesVersion\":3") != std::string::npos);
        CHECK(meta->find("\"seq\":" + std::to_string(done->frames.front()->seq)) != std::string::npos);
        const auto jpg = rec.readFrame(id, "000001.jpg");
        CHECK(jpg && jpg->size() == 1000);

        // 不合法的 id 或檔名
        CHECK(!rec.readMeta("../etc"));
        CHECK(!rec.readMeta("20261118-120000-nope-001"));
        CHECK(!rec.readFrame(id, "../meta.json"));
        CHECK(!rec.readFrame(id, "meta.json"));
    }

    // 攝影機中斷期間：只包含實際擷取到的影格
    {
        RingBuffer ring(60 * kSec);
        EventRecorder rec(ring, dir.string(), 1'000'000'000);
        uint64_t seq = 0;
        fill(ring, seq, 0, 10 * kSec);
        fill(ring, seq, 15 * kSec, 30 * kSec);  // 10–15 秒沒有影格
        std::optional<EventRecorder::Clip> done;
        rec.request(req("not_in_bed", 20 * kSec, 15 * kSec, 5 * kSec), [&](const auto& c) { done = c; });
        rec.processDue(0);
        CHECK(done && done->written);
        CHECK(done->frames.size() == 50 + 101);  // 5–10s 50 張 + 15–25s 101 張
    }

    // 攝影機停住、影格一直沒到片段結束時間：停止時以現有影格產生
    {
        RingBuffer ring(60 * kSec);
        EventRecorder rec(ring, dir.string(), 1'000'000'000);
        uint64_t seq = 0;
        fill(ring, seq, 0, 10 * kSec);
        std::optional<EventRecorder::Clip> done;
        rec.request(req("awake_active", 9 * kSec), [&](const auto& c) { done = c; });
        rec.processDue(0);
        CHECK(!done);
        rec.processDue(0, true);
        CHECK(done && done->frames.size() == 100);
    }

    // 空間上限：超過時從最舊的片段開始刪除
    {
        const fs::path small = fs::path(tmpl) / "small";
        RingBuffer ring(60 * kSec);
        EventRecorder rec(ring, small.string(), 300'000);  // 每個片段約 101 x 1000 bytes
        uint64_t seq = 0;
        fill(ring, seq, 0, 60 * kSec);
        std::vector<std::string> ids;
        for (int i = 0; i < 5; ++i) {
            ids.push_back(rec.request(req("awake_active", (10 + i) * kSec, 5 * kSec, 5 * kSec), nullptr));
            rec.processDue(0);
        }
        int remaining = 0;
        for (const auto& id : ids) remaining += fs::exists(small / id) ? 1 : 0;
        CHECK(remaining == 2);
        CHECK(!fs::exists(small / ids[0]) && !fs::exists(small / ids[2]));
        CHECK(fs::exists(small / ids[3]) && fs::exists(small / ids[4]));
    }

    // 背景執行緒：影格到齊後自動產生
    {
        RingBuffer ring(60 * kSec);
        EventRecorder rec(ring, dir.string(), 1'000'000'000);
        rec.start();
        uint64_t seq = 0;
        fill(ring, seq, 0, 5 * kSec);
        std::atomic<bool> done{false};
        rec.request(req("prone", 4 * kSec, 2 * kSec, kSec), [&](const auto&) { done = true; });
        fill(ring, seq, 5 * kSec, 6 * kSec);
        for (int i = 0; i < 50 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(done);
        rec.stop();
    }

    fs::remove_all(tmpl);
    return 0;
}
