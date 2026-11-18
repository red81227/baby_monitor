#pragma once

#include "common/LatestSlot.h"
#include "vision/Frame.h"
#include "vision/LiveProtocol.h"

#include <crow.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// WebSocket /ws/live：推送主碼流原圖 (binary) 與偵測 metadata (text)。
// - 每個連線最多 1 張影格在途；客戶端每收到一張回 "ack"。ack 前到的新影格覆蓋待送槽 (latest-wins)
// - metadata 很小，直接送；在途影格超過 kStallTimeout 沒有 ack 的連線會被關閉，避免無限累積
// - 沒有連線時推送執行緒只等待，不處理任何影格
// 執行緒安全：送出與 onclose 移除連線使用同一把 mutex。Crow 在 onclose 之後才 delete 連線，
// 所以持有 mutex 時 map 裡的指標都有效 (見 design.md D6 的 S2 結論)。
class LiveHub {
public:
    static constexpr auto kStallTimeout = std::chrono::seconds(10);

    explicit LiveHub(LatestSlot<RawFrame>& frames) : frames_(frames) {}
    ~LiveHub() { stop(); }

    void registerRoutes(crow::SimpleApp& app) {
        CROW_WEBSOCKET_ROUTE(app, "/ws/live")
            .onopen([this](crow::websocket::connection& conn) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopping_) {
                        conn.close("shutting down");
                        return;
                    }
                    clients_[&conn];
                    if (!lastDetection_.empty()) conn.send_text(lastDetection_);
                }
                cv_.notify_all();
            })
            .onclose([this](crow::websocket::connection& conn, const std::string&) {
                std::lock_guard<std::mutex> lock(mutex_);
                clients_.erase(&conn);
            })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool isBinary) {
                if (isBinary || data != "ack") return;
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = clients_.find(&conn);
                if (it == clients_.end()) return;
                Client& c = it->second;
                c.inFlight = false;
                if (c.pending) {
                    sendFrameLocked(conn, c, std::move(c.pending));
                }
            });
    }

    void start() {
        worker_ = std::thread(&LiveHub::run, this);
    }

    // 停止推送並忘記所有連線 (連線本身由 Crow 在 app.stop() 時關閉)
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ && !worker_.joinable()) return;
            stopping_ = true;
            clients_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // 分析執行緒：每次偵測完成後呼叫
    void publishDetection(const DetectionResult& result) {
        std::string msg = live::encodeDetection(result);
        std::lock_guard<std::mutex> lock(mutex_);
        lastDetection_ = msg;
        for (auto& [conn, c] : clients_) conn->send_text(msg);
    }

    std::size_t clientCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_.size();
    }

private:
    using Clock = std::chrono::steady_clock;
    using Message = std::shared_ptr<const std::string>;

    struct Client {
        bool inFlight = false;
        Clock::time_point sentAt{};
        Message pending;
    };

    void sendFrameLocked(crow::websocket::connection& conn, Client& c, Message msg) {
        conn.send_binary(*msg);  // Crow 需要自己的一份 std::string
        c.inFlight = true;
        c.sentAt = Clock::now();
        c.pending.reset();
    }

    void run() {
        uint64_t lastVersion = 0;
        while (true) {
            {
                // 沒有觀看者：等到有連線或停止，不碰影格
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !clients_.empty(); });
                if (stopping_) return;
            }

            const auto entry = frames_.waitNewerThan(lastVersion, Clock::now() + std::chrono::milliseconds(200));
            if (!entry.value) {
                if (frames_.closed()) return;
                closeStalled();
                continue;
            }
            lastVersion = entry.version;
            auto msg = std::make_shared<const std::string>(live::encodeFrame(*entry.value));

            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            for (auto& [conn, c] : clients_) {
                if (c.inFlight) {
                    c.pending = msg;  // 覆蓋尚未送出的舊影格
                } else {
                    sendFrameLocked(*conn, c, msg);
                }
            }
            closeStalledLocked();
        }
    }

    void closeStalled() {
        std::lock_guard<std::mutex> lock(mutex_);
        closeStalledLocked();
    }

    void closeStalledLocked() {
        const auto now = Clock::now();
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->second.inFlight && now - it->second.sentAt > kStallTimeout) {
                it->first->close("stalled");
                it = clients_.erase(it);  // 之後的 onclose 找不到就略過
            } else {
                ++it;
            }
        }
    }

    LatestSlot<RawFrame>& frames_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<crow::websocket::connection*, Client> clients_;
    std::string lastDetection_;
    bool stopping_ = false;
    std::thread worker_;
};
