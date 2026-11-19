#include "check.h"
#include "judge/VlmJudge.h"

#include <crow.h>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace {

// 假的 VLM 服務的行為
enum class Mode { Ok, OkFenced, Slow, BadJson, NotChat, ServerError, Hang };
std::atomic<Mode> g_mode{Mode::Ok};
std::atomic<int> g_requests{0};
std::atomic<bool> g_release{false};  // Hang 模式下放行
std::mutex g_bodyMu;
std::string g_lastBody;

std::string chatReply(const std::string& content) {
    crow::json::wvalue msg;
    msg["role"] = "assistant";
    msg["content"] = content;
    crow::json::wvalue choice;
    choice["message"] = std::move(msg);
    std::vector<crow::json::wvalue> choices;
    choices.push_back(std::move(choice));
    crow::json::wvalue body;
    body["model"] = "fake-vl";
    body["choices"] = std::move(choices);
    return body.dump();
}

std::vector<RingBuffer::FramePtr> clip(int n, int w = 1280, int h = 720) {
    cv::Mat img(h, w, CV_8UC3, cv::Scalar(40, 80, 120));
    std::vector<unsigned char> jpg;
    cv::imencode(".jpg", img, jpg);
    std::vector<RingBuffer::FramePtr> out;
    for (int i = 0; i < n; ++i) {
        auto f = std::make_shared<RawFrame>();
        f->seq = i + 1;
        f->tCapture = std::chrono::steady_clock::time_point(std::chrono::milliseconds(i * 100));
        f->jpeg = jpg;
        f->width = w;
        f->height = h;
        out.push_back(f);
    }
    return out;
}

VlmJudge::Job job(std::chrono::milliseconds timeout, std::function<void(const VlmJudge::Result&)> done) {
    VlmJudge::Job j;
    j.situationId = "awake_active";
    j.prompt = "請判斷寶寶是否醒來";
    j.signals[Signal::Activity] = 0.05;
    j.signals[Signal::FaceVisible] = 1.0;
    j.frames = clip(250);
    j.preUs = 20'000'000;
    j.postUs = 5'000'000;
    j.timeout = timeout;
    j.onDone = std::move(done);
    return j;
}

VlmJudge::Result judgeOnce(VlmJudge& judge, std::chrono::milliseconds timeout = 5000ms) {
    std::promise<VlmJudge::Result> p;
    auto f = p.get_future();
    judge.submit(job(timeout, [&](const VlmJudge::Result& r) { p.set_value(r); }));
    CHECK(f.wait_for(10s) == std::future_status::ready);
    return f.get();
}

} // namespace

int main() {
    // 網址組合與取樣、回覆解析 (不需要伺服器)
    CHECK(VlmJudge::chatUrl("http://127.0.0.1:8080") == "http://127.0.0.1:8080/v1/chat/completions");
    CHECK(VlmJudge::chatUrl("http://127.0.0.1:8080/v1/") == "http://127.0.0.1:8080/v1/chat/completions");
    CHECK(VlmJudge::modelsUrl("http://localhost:11434/") == "http://localhost:11434/v1/models");
    {
        const auto frames = clip(250, 64, 36);
        const auto s = VlmJudge::sample(frames, 6);
        CHECK(s.size() == 6);
        CHECK(s.front()->seq == 1 && s.back()->seq == 250);
        CHECK(VlmJudge::sample(clip(3, 64, 36), 6).size() == 3);
    }
    {
        bool alert = false;
        std::string cause, error;
        CHECK(VlmJudge::parseVerdict(R"({"alert": true, "cause": "醒了"})", alert, cause, error));
        CHECK(alert && cause == "醒了");
        CHECK(VlmJudge::parseVerdict("```json\n{\"alert\": false, \"cause\": \"翻身 {繼續睡}\"}\n```", alert, cause, error));
        CHECK(!alert && cause == "翻身 {繼續睡}");
        CHECK(VlmJudge::parseVerdict(R"({"alert": false})", alert, cause, error) && cause.empty());
        CHECK(!VlmJudge::parseVerdict("我覺得寶寶醒了", alert, cause, error));
        CHECK(!VlmJudge::parseVerdict(R"({"alert": "yes"})", alert, cause, error));
        CHECK(!VlmJudge::parseVerdict(R"({"cause": "x"})", alert, cause, error));
        CHECK(!VlmJudge::parseVerdict(R"({"alert": true)", alert, cause, error));
    }

    // 未設定 VLM：立即以 Unavailable 回呼
    {
        VlmJudge off({});
        CHECK(!off.enabled());
        const auto r = judgeOnce(off);
        CHECK(r.status == VlmJudge::Result::Status::Unavailable);
    }

    // 假的 VLM 服務
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Critical);
    CROW_ROUTE(app, "/v1/models")([] { return crow::response(200, R"({"data":[{"id":"fake-vl"}]})"); });
    CROW_ROUTE(app, "/v1/chat/completions").methods(crow::HTTPMethod::Post)([](const crow::request& req) {
        ++g_requests;
        {
            std::lock_guard<std::mutex> lock(g_bodyMu);
            g_lastBody = req.body;
        }
        switch (g_mode.load()) {
            case Mode::Ok: return crow::response(200, chatReply(R"({"alert": true, "cause": "睜眼並揮動手腳"})"));
            case Mode::OkFenced:
                return crow::response(200, chatReply("```json\n{\"alert\": false, \"cause\": \"翻身後繼續睡\"}\n```"));
            case Mode::Slow:
                std::this_thread::sleep_for(1500ms);
                return crow::response(200, chatReply(R"({"alert": false, "cause": "slow"})"));
            case Mode::BadJson: return crow::response(200, chatReply("寶寶看起來在睡覺"));
            case Mode::NotChat: return crow::response(200, R"({"result": "?"})");
            case Mode::ServerError: return crow::response(500, "boom");
            case Mode::Hang:
                for (int i = 0; i < 300 && !g_release; ++i) std::this_thread::sleep_for(10ms);
                return crow::response(200, chatReply(R"({"alert": false, "cause": "late"})"));
        }
        return crow::response(500);
    });
    const uint16_t port = 18997;
    auto server = app.port(port).bindaddr("127.0.0.1").concurrency(4).run_async();
    app.wait_for_server_start();
    const std::string url = "http://127.0.0.1:" + std::to_string(port);

    VlmJudge::Options opts;
    opts.url = url;
    opts.model = "fake-vl";
    opts.healthInterval = 100ms;
    VlmJudge judge(opts);
    judge.start();

    // 可用性檢查
    for (int i = 0; i < 50 && !judge.available(); ++i) std::this_thread::sleep_for(20ms);
    CHECK(judge.available());

    // 正常判定：需要提醒
    {
        g_mode = Mode::Ok;
        const auto r = judgeOnce(judge);
        CHECK(r.status == VlmJudge::Result::Status::Ok);
        CHECK(r.alert && r.cause == "睜眼並揮動手腳");
        CHECK(r.model == "fake-vl");
        CHECK(r.rawReply.find("睜眼") != std::string::npos);

        // 送出的請求：模型名稱、system + user、prompt 與訊號、最多 6 張長邊 <= 640 的影像
        std::string body;
        {
            std::lock_guard<std::mutex> lock(g_bodyMu);
            body = g_lastBody;
        }
        const auto req = crow::json::load(body);
        CHECK(req && req["model"].s() == "fake-vl");
        CHECK(req["messages"].size() == 2);
        CHECK(std::string(req["messages"][0]["role"].s()) == "system");
        const auto& content = req["messages"][1]["content"];
        CHECK(content.size() == 7);  // 1 段文字 + 6 張影像
        const std::string text = content[0]["text"].s();
        CHECK(text.find("請判斷寶寶是否醒來") != std::string::npos);
        CHECK(text.find("activity=0.05") != std::string::npos);
        CHECK(text.find("person_present=未知") != std::string::npos);
        const std::string dataUrl = content[1]["image_url"]["url"].s();
        CHECK(dataUrl.rfind("data:image/jpeg;base64,", 0) == 0);
        // 解碼影像確認尺寸
        static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<unsigned char> bytes;
        uint32_t acc = 0;
        int bits = 0;
        for (char c : dataUrl.substr(23)) {
            if (c == '=') break;
            acc = (acc << 6) | static_cast<uint32_t>(tbl.find(c));
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                bytes.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
            }
        }
        const cv::Mat img = cv::imdecode(bytes, cv::IMREAD_COLOR);
        CHECK(!img.empty() && std::max(img.cols, img.rows) == 640);
    }

    // 回覆包在 ```json 中：判定為不提醒
    {
        g_mode = Mode::OkFenced;
        const auto r = judgeOnce(judge);
        CHECK(r.status == VlmJudge::Result::Status::Ok && !r.alert && r.cause == "翻身後繼續睡");
    }

    // 逾時
    {
        g_mode = Mode::Slow;
        const auto r = judgeOnce(judge, 500ms);
        CHECK(r.status == VlmJudge::Result::Status::Timeout);
        std::this_thread::sleep_for(1200ms);  // 讓伺服器處理完
    }

    // 回覆格式錯誤：保留原始回覆
    {
        g_mode = Mode::BadJson;
        const auto r = judgeOnce(judge);
        CHECK(r.status == VlmJudge::Result::Status::BadReply);
        CHECK(r.rawReply == "寶寶看起來在睡覺");
        g_mode = Mode::NotChat;
        CHECK(judgeOnce(judge).status == VlmJudge::Result::Status::BadReply);
        g_mode = Mode::ServerError;
        CHECK(judgeOnce(judge).status == VlmJudge::Result::Status::Unavailable);
    }

    // 佇列滿：1 筆判定中 + 4 筆等待，第 6 筆立即以 QueueFull 回呼
    {
        g_mode = Mode::Hang;
        g_release = false;
        const int before = g_requests;
        std::atomic<int> done{0};
        std::vector<VlmJudge::Result::Status> statuses;
        std::mutex mu;
        auto record = [&](const VlmJudge::Result& r) {
            std::lock_guard<std::mutex> lock(mu);
            statuses.push_back(r.status);
            ++done;
        };
        judge.submit(job(20s, record));
        for (int i = 0; i < 100 && g_requests == before; ++i) std::this_thread::sleep_for(10ms);
        CHECK(g_requests == before + 1);  // 第一筆已送到伺服器
        for (int i = 0; i < 5; ++i) judge.submit(job(20s, record));
        {
            std::lock_guard<std::mutex> lock(mu);
            CHECK(statuses.size() == 1 && statuses[0] == VlmJudge::Result::Status::QueueFull);
        }
        g_release = true;
        for (int i = 0; i < 500 && done < 6; ++i) std::this_thread::sleep_for(10ms);
        CHECK(done == 6);
    }

    // 巡檢優先：佇列已有 4 筆一般判定時，巡檢工作仍可排入，且排在一般判定之前
    {
        g_mode = Mode::Hang;
        g_release = false;
        const int before = g_requests;
        std::mutex mu;
        std::vector<std::string> order;
        std::vector<VlmJudge::Result::Status> statuses;
        auto record = [&](const std::string& name) {
            return [&, name](const VlmJudge::Result& r) {
                std::lock_guard<std::mutex> lock(mu);
                order.push_back(name);
                statuses.push_back(r.status);
            };
        };
        judge.submit(job(20s, record("first")));
        for (int i = 0; i < 100 && g_requests == before; ++i) std::this_thread::sleep_for(10ms);
        for (int i = 0; i < 4; ++i) judge.submit(job(20s, record("normal")));
        auto p = job(20s, record("patrol"));
        p.patrol = true;
        p.frames = clip(2);
        judge.submit(std::move(p));
        {
            std::lock_guard<std::mutex> lock(mu);
            CHECK(order.empty());  // 巡檢不因佇列已滿被拒絕
        }
        g_release = true;
        for (int i = 0; i < 500; ++i) {
            {
                std::lock_guard<std::mutex> lock(mu);
                if (order.size() == 6) break;
            }
            std::this_thread::sleep_for(10ms);
        }
        std::lock_guard<std::mutex> lock(mu);
        CHECK(order.size() == 6);
        CHECK(order[0] == "first" && order[1] == "patrol");
        for (auto st : statuses) CHECK(st == VlmJudge::Result::Status::Ok);
    }
    // 巡檢的請求內容：說明是最近的畫面與目前的訊號
    {
        g_mode = Mode::Ok;
        std::promise<VlmJudge::Result> pr;
        auto f = pr.get_future();
        auto p = job(5s, [&](const VlmJudge::Result& r) { pr.set_value(r); });
        p.patrol = true;
        p.frames = clip(2);
        judge.submit(std::move(p));
        CHECK(f.wait_for(10s) == std::future_status::ready);
        std::lock_guard<std::mutex> lock(g_bodyMu);
        CHECK(g_lastBody.find("目前的訊號") != std::string::npos);
        CHECK(g_lastBody.find("最近的畫面") != std::string::npos);
        CHECK(g_lastBody.find("觸發時的訊號") == std::string::npos);
    }

    // 關閉時中斷進行中的請求，等待中的工作以 Unavailable 回呼，不會卡住
    {
        g_mode = Mode::Hang;
        g_release = false;
        std::atomic<int> unavailable{0};
        auto record = [&](const VlmJudge::Result& r) {
            if (r.status == VlmJudge::Result::Status::Unavailable) ++unavailable;
        };
        judge.submit(job(20s, record));
        judge.submit(job(20s, record));
        std::this_thread::sleep_for(100ms);
        const auto t0 = std::chrono::steady_clock::now();
        judge.stop();
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);
        CHECK(unavailable == 2);
        g_release = true;
    }

    app.stop();
    server.wait();

    // 連線失敗：沒有服務在監聽的 port
    {
        VlmJudge::Options o;
        o.url = "http://127.0.0.1:1";
        o.healthInterval = 50ms;
        VlmJudge down(o);
        down.start();
        const auto r = judgeOnce(down);
        CHECK(r.status == VlmJudge::Result::Status::Unavailable);
        CHECK(!r.error.empty());
        std::this_thread::sleep_for(150ms);
        CHECK(!down.available());
        CHECK(!down.lastHealthError().empty());
    }
    return 0;
}
