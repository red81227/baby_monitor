#include "check.h"
#include "vision/CaptureThread.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono;

namespace {

// 可控制的假攝影機：connected=false 時 grab / open 都失敗
class FakeSource : public FrameSource {
public:
    std::atomic<bool> connected{true};
    std::atomic<int> opens{0};

    bool open() override {
        ++opens;
        return connected.load();
    }
    void close() override {}
    std::optional<RawFrame> grab() override {
        std::this_thread::sleep_for(milliseconds(5));
        if (!connected) return std::nullopt;
        RawFrame f;
        f.tCapture = steady_clock::now();
        f.jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
        f.width = 1280;
        f.height = 720;
        return f;
    }
    Description describe() const override { return {"/dev/fake", "MJPG", 1280, 720, 30}; }
    const std::string& lastError() const override { return error_; }

private:
    std::string error_;
};

template <class Pred>
bool waitUntil(Pred p, milliseconds timeout) {
    const auto end = steady_clock::now() + timeout;
    while (steady_clock::now() < end) {
        if (p()) return true;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return p();
}

} // namespace

int main() {
    // 退避：250ms 起倍增，上限 5s
    CHECK(reconnectBackoff(0) == milliseconds(250));
    CHECK(reconnectBackoff(1) == milliseconds(500));
    CHECK(reconnectBackoff(2) == milliseconds(1000));
    CHECK(reconnectBackoff(3) == milliseconds(2000));
    CHECK(reconnectBackoff(4) == milliseconds(4000));
    CHECK(reconnectBackoff(5) == milliseconds(5000));
    CHECK(reconnectBackoff(6) == milliseconds(5000));
    CHECK(reconnectBackoff(1000) == milliseconds(5000));
    for (unsigned i = 1; i < 40; ++i) {
        CHECK(reconnectBackoff(i) >= reconnectBackoff(i - 1));
        CHECK(reconnectBackoff(i) <= milliseconds(5000));
    }

    // 序號：從 1 開始，嚴格遞增
    SeqCounter seq;
    CHECK(seq.last() == 0);
    CHECK(seq.next() == 1);
    CHECK(seq.next() == 2);
    CHECK(seq.last() == 2);

    // 擷取執行緒：發布影格、中斷回報、重連後 seq 延續、stop 可中斷退避等待
    FakeSource src;
    LatestSlot<RawFrame> slot;
    MonitorState state;
    Heartbeat hb;
    CaptureThread capture(src, slot, state, hb);
    capture.start();

    CHECK(waitUntil([&] { return slot.latest().value && slot.latest().value->seq >= 5; }, seconds(2)));
    auto first = slot.latest().value;
    CHECK(state.snapshot()->cameraConnected);
    CHECK(state.snapshot()->seq >= first->seq);

    // 拔除
    src.connected = false;
    CHECK(waitUntil([&] { return !state.snapshot()->cameraConnected; }, seconds(1)));
    const uint64_t seqAtDisconnect = slot.latest().value->seq;
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(slot.latest().value->seq == seqAtDisconnect);  // 中斷期間 seq 停止
    CHECK(src.opens >= 1);                               // 250ms 後已嘗試重開
    CHECK(hb.age() < milliseconds(300));                 // 重連嘗試也更新進度

    // 插回：seq 從中斷前延續
    src.connected = true;
    CHECK(waitUntil([&] { return state.snapshot()->cameraConnected; }, seconds(3)));
    CHECK(waitUntil([&] { return slot.latest().value->seq > seqAtDisconnect; }, seconds(1)));

    // 中斷到退避進入 5s 等待時，stop 仍立刻返回
    src.connected = false;
    std::this_thread::sleep_for(milliseconds(4000));  // 經過 250+500+1000+2000ms，正在等 4s
    const auto t0 = steady_clock::now();
    capture.stop();
    CHECK(steady_clock::now() - t0 < milliseconds(500));

    return 0;
}
