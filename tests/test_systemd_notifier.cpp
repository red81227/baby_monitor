#include "check.h"
#include "common/SystemdNotifier.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

// 綁定 datagram socket，name 以 '@' 開頭時為抽象 namespace
int bindReceiver(const std::string& name) {
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, name.data(), name.size());
    socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size());
    if (name[0] == '@') {
        sa.sun_path[0] = '\0';
    } else {
        ::unlink(name.c_str());
        len += 1;
    }
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&sa), len) == 0);
    return fd;
}

// 收集 timeout 內收到的所有訊息
std::vector<std::string> receiveFor(int fd, milliseconds timeout) {
    std::vector<std::string> out;
    const auto end = steady_clock::now() + timeout;
    while (true) {
        const auto left = duration_cast<milliseconds>(end - steady_clock::now()).count();
        if (left <= 0) break;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left)) <= 0) continue;
        char buf[256];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) out.emplace_back(buf, static_cast<std::size_t>(n));
    }
    return out;
}

int count(const std::vector<std::string>& msgs, const std::string& m) {
    int n = 0;
    for (const auto& s : msgs) n += s == m;
    return n;
}

void clearEnv() {
    unsetenv("NOTIFY_SOCKET");
    unsetenv("WATCHDOG_USEC");
    unsetenv("WATCHDOG_PID");
}

} // namespace

int main() {
    // 純函式：所有 heartbeat 都在週期內才算正常
    {
        Heartbeat a, b;
        const auto now = Heartbeat::Clock::now();
        CHECK(allHeartbeatsFresh({&a, &b}, milliseconds(100), now));
        CHECK(!allHeartbeatsFresh({&a, &b}, milliseconds(100), now + milliseconds(150)));
        CHECK(allHeartbeatsFresh({}, milliseconds(1), now));
    }

    // 沒有 NOTIFY_SOCKET：停用，呼叫都是 no-op
    {
        clearEnv();
        SystemdNotifier n;
        CHECK(!n.enabled());
        CHECK(n.watchdogPeriod().count() == 0);
        CHECK(!n.notify("READY=1"));
        Heartbeat hb;
        n.startWatchdog({&hb});
        n.stop();
    }

    // 一般路徑的 socket：READY=1；沒有 WATCHDOG_USEC 時不送 watchdog
    {
        clearEnv();
        const std::string path = "/tmp/bm-notify-test-" + std::to_string(::getpid());
        const int fd = bindReceiver(path);
        setenv("NOTIFY_SOCKET", path.c_str(), 1);
        SystemdNotifier n;
        CHECK(n.enabled());
        CHECK(n.watchdogPeriod().count() == 0);
        CHECK(n.notify("READY=1"));
        Heartbeat hb;
        n.startWatchdog({&hb});
        const auto msgs = receiveFor(fd, milliseconds(200));
        CHECK(msgs.size() == 1 && msgs[0] == "READY=1");
        n.stop();
        ::close(fd);
        ::unlink(path.c_str());
    }

    // 抽象 namespace + watchdog：正常時以週期一半送 WATCHDOG=1，任一停滯就停止送出
    {
        clearEnv();
        const std::string name = "@bm-notify-test-" + std::to_string(::getpid());
        const int fd = bindReceiver(name);
        setenv("NOTIFY_SOCKET", name.c_str(), 1);
        setenv("WATCHDOG_USEC", "200000", 1);  // 200ms → 每 100ms 檢查
        SystemdNotifier n;
        CHECK(n.enabled());
        CHECK(n.watchdogPeriod() == microseconds(200000));
        CHECK(n.notify("READY=1"));

        Heartbeat capture, analysis;
        std::atomic<bool> captureAlive{true}, stop{false};
        std::thread beater([&] {
            while (!stop) {
                if (captureAlive) capture.beat();
                analysis.beat();
                std::this_thread::sleep_for(milliseconds(10));
            }
        });
        n.startWatchdog({&capture, &analysis});

        auto msgs = receiveFor(fd, milliseconds(1000));
        CHECK(count(msgs, "READY=1") == 1);
        const int beats = count(msgs, "WATCHDOG=1");
        CHECK(beats >= 8 && beats <= 12);  // 約每 100ms 一次

        // 擷取停滯：超過週期後不再送出
        captureAlive = false;
        receiveFor(fd, milliseconds(300));  // 丟掉停滯判定前的訊息
        msgs = receiveFor(fd, milliseconds(600));
        CHECK(count(msgs, "WATCHDOG=1") == 0);

        // 恢復：重新送出
        captureAlive = true;
        msgs = receiveFor(fd, milliseconds(400));
        CHECK(count(msgs, "WATCHDOG=1") >= 2);

        // stop 立即返回
        const auto t0 = steady_clock::now();
        n.stop();
        CHECK(steady_clock::now() - t0 < milliseconds(200));
        stop = true;
        beater.join();
        ::close(fd);
    }

    // WATCHDOG_PID 不是本程式：不啟用 watchdog
    {
        clearEnv();
        setenv("NOTIFY_SOCKET", "@bm-unused", 1);
        setenv("WATCHDOG_USEC", "200000", 1);
        setenv("WATCHDOG_PID", "1", 1);
        SystemdNotifier n;
        CHECK(n.watchdogPeriod().count() == 0);
    }

    clearEnv();
    return 0;
}
