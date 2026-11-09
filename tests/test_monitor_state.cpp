#include "check.h"
#include "models/MonitorState.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono;

    // 初始值
    MonitorState state;
    auto s = state.getStatus();
    CHECK(!s.motionDetected);
    CHECK(!s.personDetected);
    CHECK(s.lastMotionEpochMs == 0);
    CHECK(s.latestFrameBytes == 0);
    CHECK(state.getFrameJpegCopy().empty());

    // 有人但沒移動：lastMotionEpochMs 不變
    state.updateDetection(true, false);
    s = state.getStatus();
    CHECK(s.personDetected && !s.motionDetected);
    CHECK(s.lastMotionEpochMs == 0);

    // 發生移動：lastMotionEpochMs 為接近當下的時間
    const long long before = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    state.updateDetection(true, true);
    const long long after = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    s = state.getStatus();
    CHECK(s.motionDetected);
    CHECK(s.lastMotionEpochMs >= before && s.lastMotionEpochMs <= after);

    // 移動結束後保留最後一次移動時間
    const long long lastMotion = s.lastMotionEpochMs;
    state.updateDetection(false, false);
    s = state.getStatus();
    CHECK(!s.motionDetected && !s.personDetected);
    CHECK(s.lastMotionEpochMs == lastMotion);

    // 影格以複本回傳
    state.updateFrameJpeg({1, 2, 3});
    auto frame = state.getFrameJpegCopy();
    CHECK((frame == std::vector<unsigned char>{1, 2, 3}));
    frame[0] = 9;
    CHECK(state.getFrameJpegCopy()[0] == 1);
    CHECK(state.getStatus().latestFrameBytes == 3);

    // 多執行緒同時讀寫 1 秒 (每張影格的所有 byte 都相同，用來偵測讀到寫一半的資料)
    state.updateFrameJpeg(std::vector<unsigned char>(1000, 0));
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        unsigned char n = 0;
        while (!stop) {
            state.updateDetection(n % 2, n % 3 == 0);
            state.updateFrameJpeg(std::vector<unsigned char>(1000 + n, n));
            ++n;
        }
    });
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop) {
                auto copy = state.getFrameJpegCopy();
                // 每張影格的內容都必須一致 (沒有讀到寫到一半的資料)
                for (auto b : copy) CHECK(b == copy[0]);
                state.getStatus();
            }
        });
    }
    std::this_thread::sleep_for(seconds(1));
    stop = true;
    writer.join();
    for (auto& t : readers) t.join();

    return 0;
}
