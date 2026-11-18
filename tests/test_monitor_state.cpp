#include "check.h"
#include "models/MonitorState.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

std::shared_ptr<const RawFrame> makeFrame(uint64_t seq, std::size_t bytes, unsigned char fill) {
    auto f = std::make_shared<RawFrame>();
    f->seq = seq;
    f->tCapture = std::chrono::steady_clock::now();
    f->jpeg.assign(bytes, fill);
    f->width = 1280;
    f->height = 720;
    return f;
}

} // namespace

int main() {
    using namespace std::chrono;

    // 初始值
    MonitorState state;
    auto s = state.snapshot();
    CHECK(s);
    CHECK(!s->motionDetected);
    CHECK(!s->personDetected);
    CHECK(s->lastMotionEpochMs == 0);
    CHECK(!s->cameraConnected);
    CHECK(s->seq == 0);
    CHECK(s->detectHz == 0.0);
    CHECK(s->frame == nullptr);

    // 有人但沒移動：lastMotionEpochMs 不變
    state.updateDetection(true, false);
    s = state.snapshot();
    CHECK(s->personDetected && !s->motionDetected);
    CHECK(s->lastMotionEpochMs == 0);

    // 發生移動：lastMotionEpochMs 為接近當下的時間
    const long long before = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    state.updateDetection(true, true);
    const long long after = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    s = state.snapshot();
    CHECK(s->motionDetected);
    CHECK(s->lastMotionEpochMs >= before && s->lastMotionEpochMs <= after);

    // 移動結束後保留最後一次移動時間
    const long long lastMotion = s->lastMotionEpochMs;
    state.updateDetection(false, false);
    s = state.snapshot();
    CHECK(!s->motionDetected && !s->personDetected);
    CHECK(s->lastMotionEpochMs == lastMotion);

    // 新影格：seq、frame、cameraConnected 一起更新；快照共用同一份影格 (不複製)
    auto f1 = makeFrame(1, 3, 1);
    state.updateFrame(f1);
    s = state.snapshot();
    CHECK(s->cameraConnected);
    CHECK(s->seq == 1);
    CHECK(s->frame.get() == f1.get());

    // 舊快照不可變：之後的更新不影響已取得的快照
    auto old = state.snapshot();
    state.updateFrame(makeFrame(2, 5, 2));
    state.setDetectHz(3.5);
    state.setCameraConnected(false);
    CHECK(old->seq == 1 && old->frame->jpeg.size() == 3 && old->cameraConnected && old->detectHz == 0.0);
    s = state.snapshot();
    CHECK(s->seq == 2 && s->frame->jpeg.size() == 5);
    CHECK(s->detectHz == 3.5);
    // 攝影機中斷：保留最後的影格與 seq，只標記為中斷
    CHECK(!s->cameraConnected);
    CHECK(s->frame != nullptr);

    // 中斷後的新影格：恢復為已連線
    state.updateFrame(makeFrame(3, 5, 3));
    CHECK(state.snapshot()->cameraConnected);
    CHECK(state.snapshot()->seq == 3);

    // 多執行緒同時讀寫 1 秒：快照內的 seq 與影格一致，影格內容沒有寫到一半的資料，seq 不倒退
    std::atomic<bool> stop{false};
    std::thread frameWriter([&] {
        uint64_t seq = 3;
        while (!stop) {
            ++seq;
            state.updateFrame(makeFrame(seq, 1000 + seq % 100, static_cast<unsigned char>(seq)));
        }
    });
    std::thread detectWriter([&] {
        unsigned n = 0;
        while (!stop) {
            state.updateDetection(n % 2, n % 3 == 0);
            state.setDetectHz(n % 10);
            ++n;
        }
    });
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            uint64_t lastSeq = 0;
            while (!stop) {
                auto snap = state.snapshot();
                CHECK(snap->frame);
                CHECK(snap->seq == snap->frame->seq);
                CHECK(snap->seq >= lastSeq);
                lastSeq = snap->seq;
                const auto& jpeg = snap->frame->jpeg;
                for (auto b : jpeg) CHECK(b == jpeg[0]);
            }
        });
    }
    std::this_thread::sleep_for(seconds(1));
    stop = true;
    frameWriter.join();
    detectWriter.join();
    for (auto& t : readers) t.join();

    return 0;
}
