#include "common/SystemdNotifier.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>

SystemdNotifier::SystemdNotifier() {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (!path || *path == '\0') return;

    const std::size_t len = std::strlen(path);
    sockaddr_un sa{};
    if ((path[0] != '/' && path[0] != '@') || len >= sizeof(sa.sun_path)) {
        std::cerr << "[WARN] 不支援的 NOTIFY_SOCKET 位址: " << path << "\n";
        return;
    }
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, path, len);
    socklen_t addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
    if (path[0] == '@') {
        sa.sun_path[0] = '\0';  // 抽象 namespace：長度不含結尾的 NUL
    } else {
        addrLen += 1;
    }
    addr_.assign(reinterpret_cast<const char*>(&sa), reinterpret_cast<const char*>(&sa) + addrLen);

    fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        std::cerr << "[WARN] 無法建立 systemd 通知 socket: " << std::strerror(errno) << "\n";
        return;
    }

    if (const char* usec = std::getenv("WATCHDOG_USEC")) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(usec, &end, 10);
        const char* pid = std::getenv("WATCHDOG_PID");
        const bool forUs = !pid || std::strtoll(pid, nullptr, 10) == static_cast<long long>(::getpid());
        if (*usec != '\0' && *end == '\0' && v > 0 && forUs) watchdog_ = std::chrono::microseconds(v);
    }
}

SystemdNotifier::~SystemdNotifier() {
    stop();
    if (fd_ >= 0) ::close(fd_);
}

bool SystemdNotifier::notify(const std::string& message) {
    if (fd_ < 0) return false;
    const ssize_t n = ::sendto(fd_, message.data(), message.size(), MSG_NOSIGNAL,
                               reinterpret_cast<const sockaddr*>(addr_.data()), static_cast<socklen_t>(addr_.size()));
    return n == static_cast<ssize_t>(message.size());
}

void SystemdNotifier::startWatchdog(std::vector<const Heartbeat*> heartbeats) {
    if (fd_ < 0 || watchdog_.count() == 0 || worker_.joinable()) return;
    heartbeats_ = std::move(heartbeats);
    worker_ = std::thread(&SystemdNotifier::watchdogLoop, this);
}

void SystemdNotifier::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SystemdNotifier::watchdogLoop() {
    const auto interval = watchdog_ / 2;
    bool warned = false;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (allHeartbeatsFresh(heartbeats_, watchdog_)) {
            notify("WATCHDOG=1");
            warned = false;
        } else if (!warned) {
            warned = true;
            std::cerr << "[ERROR] 影像處理停滯，停止送出 systemd watchdog 通知\n";
        }
        cv_.wait_for(lock, interval, [this] { return stopping_; });
    }
}

bool allHeartbeatsFresh(const std::vector<const Heartbeat*>& heartbeats, std::chrono::microseconds period,
                        Heartbeat::Clock::time_point now) {
    return !firstStaleHeartbeat(heartbeats, period, now).has_value();
}
