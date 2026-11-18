#include "check.h"
#include "common/LatestSlot.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono;
    using Slot = LatestSlot<int>;

    // 尚未有值：latest 為空，等待逾時回傳 nullptr
    {
        Slot slot;
        CHECK(slot.latest().value == nullptr);
        CHECK(slot.latest().version == 0);
        const auto t0 = Slot::Clock::now();
        auto e = slot.waitNewerThan(0, t0 + milliseconds(30));
        CHECK(e.value == nullptr);
        CHECK(Slot::Clock::now() - t0 >= milliseconds(30));
    }

    // 覆蓋：連續 publish 只保留最後一個，版本號遞增
    {
        Slot slot;
        slot.publish(std::make_shared<const int>(1));
        slot.publish(std::make_shared<const int>(2));
        slot.publish(std::make_shared<const int>(3));
        auto e = slot.waitNewerThan(0, Slot::Clock::now());
        CHECK(e.value && *e.value == 3);
        CHECK(e.version == 3);
        // 沒有更新的值：逾時
        CHECK(slot.waitNewerThan(3, Slot::Clock::now() + milliseconds(10)).value == nullptr);
        // 舊版本號仍可立即拿到最新值
        CHECK(*slot.waitNewerThan(1, Slot::Clock::now()).value == 3);
    }

    // 讀取端持有的舊值在覆蓋後仍有效 (共用指標，不複製)
    {
        Slot slot;
        auto original = std::make_shared<const int>(7);
        slot.publish(original);
        auto held = slot.latest().value;
        CHECK(held.get() == original.get());
        slot.publish(std::make_shared<const int>(8));
        CHECK(*held == 7);
    }

    // 等待中的讀取端在 publish 時被喚醒，拿到最新值
    {
        Slot slot;
        std::atomic<int> got{0};
        std::thread reader([&] {
            auto e = slot.waitNewerThan(0, Slot::Clock::now() + seconds(5));
            if (e.value) got = *e.value;
        });
        std::this_thread::sleep_for(milliseconds(20));
        slot.publish(std::make_shared<const int>(42));
        reader.join();
        CHECK(got == 42);
    }

    // close 喚醒所有等待者，之後的等待立即回傳 nullptr
    {
        Slot slot;
        slot.publish(std::make_shared<const int>(1));
        std::vector<std::thread> waiters;
        std::atomic<int> woke{0};
        const auto t0 = Slot::Clock::now();
        for (int i = 0; i < 3; ++i) {
            waiters.emplace_back([&] {
                if (slot.waitNewerThan(1, Slot::Clock::now() + seconds(10)).value == nullptr) ++woke;
            });
        }
        std::this_thread::sleep_for(milliseconds(20));
        slot.close();
        for (auto& t : waiters) t.join();
        CHECK(woke == 3);
        CHECK(Slot::Clock::now() - t0 < seconds(2));
        CHECK(slot.closed());
        CHECK(slot.waitNewerThan(0, Slot::Clock::now() + seconds(10)).value == nullptr);
    }

    // 快速寫入、慢速讀取：讀到的版本嚴格遞增且會跳號，不會累積
    {
        Slot slot;
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            int n = 0;
            while (!stop) slot.publish(std::make_shared<const int>(++n));
        });
        uint64_t last = 0;
        int reads = 0;
        bool skipped = false;
        const auto end = Slot::Clock::now() + milliseconds(200);
        while (Slot::Clock::now() < end) {
            auto e = slot.waitNewerThan(last, Slot::Clock::now() + seconds(1));
            CHECK(e.value);
            CHECK(e.version > last);
            CHECK(static_cast<uint64_t>(*e.value) == e.version);
            if (e.version > last + 1) skipped = true;
            last = e.version;
            ++reads;
            std::this_thread::sleep_for(milliseconds(5));
        }
        stop = true;
        writer.join();
        CHECK(reads > 5);
        CHECK(skipped);
    }

    return 0;
}
