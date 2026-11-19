#include "judge/VlmJudge.h"

#include <crow/json.h>
#include <curl/curl.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace {

constexpr const char* kSystemPrompt =
    "你是嬰兒監視器的影像判讀助手。你會收到依時間排序的監視器影格與偵測系統的訊號值。"
    "只回覆一個 JSON 物件，格式為 {\"cause\": \"以繁體中文描述影格中實際看到的內容\", \"alert\": true 或 false}，"
    "先寫 cause 再依 cause 決定 alert，兩者必須一致；不要輸出任何其他文字。看不清楚或無法判斷時 alert 回覆 true。";

std::string base64(const std::vector<unsigned char>& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == data.size()) {
        const uint32_t v = uint32_t(data[i]) << 16;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// 主碼流 JPEG → 長邊不超過 maxSide 的 JPEG (只用於送給 VLM)
std::optional<std::string> toDataUrl(const RawFrame& frame, int maxSide) {
    const int longSide = std::max(frame.width, frame.height);
    int flag = cv::IMREAD_COLOR;
    if (longSide >= maxSide * 8) flag = cv::IMREAD_REDUCED_COLOR_8;
    else if (longSide >= maxSide * 4) flag = cv::IMREAD_REDUCED_COLOR_4;
    else if (longSide >= maxSide * 2) flag = cv::IMREAD_REDUCED_COLOR_2;
    cv::Mat img = cv::imdecode(frame.jpeg, flag);
    if (img.empty()) return std::nullopt;
    const int side = std::max(img.cols, img.rows);
    if (side > maxSide) {
        const double s = static_cast<double>(maxSide) / side;
        cv::resize(img, img, cv::Size(), s, s, cv::INTER_AREA);
    }
    std::vector<unsigned char> jpg;
    if (!cv::imencode(".jpg", img, jpg, {cv::IMWRITE_JPEG_QUALITY, 85})) return std::nullopt;
    return "data:image/jpeg;base64," + base64(jpg);
}

std::string describeSignals(const SignalFrame& f) {
    std::string out;
    char buf[64];
    for (const auto& info : kSignals) {
        const auto& v = f[info.id];
        if (!v) {
            std::snprintf(buf, sizeof(buf), "%s=未知", std::string(info.name).c_str());
        } else if (info.isBool) {
            std::snprintf(buf, sizeof(buf), "%s=%s", std::string(info.name).c_str(), *v != 0 ? "true" : "false");
        } else {
            std::snprintf(buf, sizeof(buf), "%s=%.4g", std::string(info.name).c_str(), *v);
        }
        out += out.empty() ? buf : std::string(", ") + buf;
    }
    return out;
}

struct HttpResult {
    long code = 0;
    std::string body;
    std::string error;  // 連線失敗、逾時等
    bool timedOut = false;
};

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

int abortCheck(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool>*>(clientp)->load() ? 1 : 0;
}

HttpResult httpRequest(const std::string& url, const std::string* postBody, std::chrono::milliseconds timeout,
                       const std::atomic<bool>& abortFlag) {
    HttpResult r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "curl 初始化失敗";
        return r;
    }
    curl_slist* headers = nullptr;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<int64_t>(1, timeout.count())));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortCheck);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(&abortFlag));
    curl_easy_setopt(curl, CURLOPT_PROXY, "");  // 本機服務不走 proxy
    if (postBody) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
    }
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.code);
    } else {
        r.timedOut = rc == CURLE_OPERATION_TIMEDOUT;
        r.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

std::string stripBase(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) base.resize(base.size() - 3);
    return base;
}

} // namespace

const char* VlmJudge::toString(Result::Status s) {
    switch (s) {
        case Result::Status::Ok: return "ok";
        case Result::Status::Timeout: return "timeout";
        case Result::Status::Unavailable: return "unavailable";
        case Result::Status::BadReply: return "bad_reply";
        case Result::Status::QueueFull: return "queue_full";
    }
    return "?";
}

std::string VlmJudge::chatUrl(const std::string& base) {
    return stripBase(base) + "/v1/chat/completions";
}

std::string VlmJudge::modelsUrl(const std::string& base) {
    return stripBase(base) + "/v1/models";
}

std::vector<RingBuffer::FramePtr> VlmJudge::sample(const std::vector<RingBuffer::FramePtr>& frames, int maxImages) {
    if (static_cast<int>(frames.size()) <= maxImages) return frames;
    std::vector<RingBuffer::FramePtr> out;
    // 平均取樣，包含第一張與最後一張
    for (int i = 0; i < maxImages; ++i) {
        const size_t idx = maxImages == 1 ? frames.size() - 1
                                          : static_cast<size_t>(std::llround(double(i) * (frames.size() - 1) /
                                                                             (maxImages - 1)));
        out.push_back(frames[idx]);
    }
    return out;
}

bool VlmJudge::parseVerdict(const std::string& content, bool& alert, std::string& cause, std::string& error) {
    // 取出第一個完整的 JSON 物件 (模型可能包上 ```json 或前後加說明)
    const size_t start = content.find('{');
    if (start == std::string::npos) {
        error = "回覆中沒有 JSON 物件";
        return false;
    }
    int depth = 0;
    bool inString = false, escaped = false;
    size_t end = std::string::npos;
    for (size_t i = start; i < content.size(); ++i) {
        const char c = content[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) {
            end = i;
            break;
        }
    }
    if (end == std::string::npos) {
        error = "回覆中的 JSON 物件不完整";
        return false;
    }
    const auto json = crow::json::load(content.substr(start, end - start + 1));
    if (!json || json.t() != crow::json::type::Object) {
        error = "回覆中的 JSON 無法解析";
        return false;
    }
    if (!json.has("alert") || (json["alert"].t() != crow::json::type::True && json["alert"].t() != crow::json::type::False)) {
        error = "回覆缺少布林值 alert";
        return false;
    }
    alert = json["alert"].b();
    cause = json.has("cause") && json["cause"].t() == crow::json::type::String ? std::string(json["cause"].s()) : "";
    return true;
}

VlmJudge::VlmJudge(Options options) : opts_(std::move(options)) {
    static const bool curlReady = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }();
    (void)curlReady;
}

VlmJudge::~VlmJudge() {
    stop();
}

void VlmJudge::start() {
    if (!enabled() || worker_.joinable()) return;
    stopping_ = false;
    abort_ = false;
    worker_ = std::thread(&VlmJudge::run, this);
    health_ = std::thread(&VlmJudge::healthLoop, this);
}

void VlmJudge::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    abort_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (health_.joinable()) health_.join();

    // 尚未判定的工作視為無法使用 (呼叫端依 on_timeout 處理)
    std::deque<Queued> left;
    {
        std::lock_guard<std::mutex> lock(mu_);
        left.swap(queue_);
    }
    for (auto& q : left) {
        Result r;
        r.status = Result::Status::Unavailable;
        r.error = "服務關閉中";
        r.model = opts_.model;
        if (q.job.onDone) q.job.onDone(r);
    }
}

std::string VlmJudge::lastHealthError() const {
    std::lock_guard<std::mutex> lock(healthMu_);
    return healthError_;
}

void VlmJudge::submit(Job job) {
    Result r;
    r.model = opts_.model;
    if (!enabled()) {
        r.status = Result::Status::Unavailable;
        r.error = "VLM 未設定 (BM_VLM_URL)";
        if (job.onDone) job.onDone(r);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        const size_t normal = static_cast<size_t>(
            std::count_if(queue_.begin(), queue_.end(), [](const Queued& q) { return !q.job.patrol; }));
        if (!stopping_ && (job.patrol || normal < opts_.queueLimit)) {
            const auto now = std::chrono::steady_clock::now();
            const auto deadline = now + job.timeout;
            if (job.patrol) {
                const auto pos = std::find_if(queue_.begin(), queue_.end(), [](const Queued& q) { return !q.job.patrol; });
                queue_.insert(pos, Queued{std::move(job), deadline, now});
            } else {
                queue_.push_back(Queued{std::move(job), deadline, now});
            }
            cv_.notify_all();
            return;
        }
    }
    r.status = Result::Status::QueueFull;
    r.error = "等待判定的數量已達上限 (" + std::to_string(opts_.queueLimit) + ")";
    if (job.onDone) job.onDone(r);
}

void VlmJudge::run() {
    for (;;) {
        Queued q;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            q = std::move(queue_.front());
            queue_.pop_front();
        }
        const Result r = judge(q);
        if (q.job.onDone) q.job.onDone(r);
    }
}

void VlmJudge::healthLoop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stopping_) {
        lock.unlock();
        const HttpResult h = httpRequest(modelsUrl(opts_.url), nullptr, std::chrono::milliseconds(3000), abort_);
        const bool ok = h.error.empty() && h.code == 200;
        available_ = ok;
        {
            std::lock_guard<std::mutex> hl(healthMu_);
            healthError_ = ok ? "" : (h.error.empty() ? "HTTP " + std::to_string(h.code) : h.error);
        }
        lock.lock();
        cv_.wait_for(lock, opts_.healthInterval, [this] { return stopping_; });
    }
}

std::string VlmJudge::buildRequest(const Job& job) const {
    std::vector<crow::json::wvalue> content;
    char range[96];
    const auto images = sample(job.frames, opts_.maxImages);
    if (job.patrol) {
        std::snprintf(range, sizeof(range), "影格依時間排序，共 %zu 張，是最近的畫面，相隔約 1 秒。", images.size());
    } else {
        std::snprintf(range, sizeof(range), "影格依時間排序，共 %zu 張，取自觸發前 %.0f 秒到觸發後 %.0f 秒。",
                      images.size(), job.preUs / 1e6, job.postUs / 1e6);
    }
    crow::json::wvalue text;
    text["type"] = "text";
    text["text"] = job.prompt + (job.patrol ? "\n\n目前的訊號：" : "\n\n觸發時的訊號：") + describeSignals(job.signals) +
                   "\n" + range;
    content.push_back(std::move(text));
    for (const auto& f : images) {
        const auto url = toDataUrl(*f, opts_.maxSide);
        if (!url) continue;
        crow::json::wvalue img;
        img["type"] = "image_url";
        img["image_url"]["url"] = *url;
        content.push_back(std::move(img));
    }

    std::vector<crow::json::wvalue> messages;
    crow::json::wvalue sys;
    sys["role"] = "system";
    sys["content"] = kSystemPrompt;
    messages.push_back(std::move(sys));
    crow::json::wvalue user;
    user["role"] = "user";
    user["content"] = std::move(content);
    messages.push_back(std::move(user));

    crow::json::wvalue body;
    if (!opts_.model.empty()) body["model"] = opts_.model;
    body["messages"] = std::move(messages);
    body["temperature"] = 0;
    body["max_tokens"] = 256;
    body["stream"] = false;
    return body.dump();
}

VlmJudge::Result VlmJudge::judge(const Queued& q) {
    Result r;
    r.model = opts_.model;
    const auto now = std::chrono::steady_clock::now();
    if (now >= q.deadline) {
        r.status = Result::Status::Timeout;
        r.error = "排隊等待時已逾時";
        return r;
    }
    const std::string body = buildRequest(q.job);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(q.deadline - std::chrono::steady_clock::now());
    const HttpResult h = httpRequest(chatUrl(opts_.url), &body, remaining, abort_);
    r.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - now).count();

    if (!h.error.empty()) {
        r.status = h.timedOut ? Result::Status::Timeout : Result::Status::Unavailable;
        r.error = h.error;
        return r;
    }
    r.rawReply = h.body.size() > 4000 ? h.body.substr(0, 4000) : h.body;
    if (h.code != 200) {
        r.status = Result::Status::Unavailable;
        r.error = "HTTP " + std::to_string(h.code);
        return r;
    }

    // OpenAI 相容格式：choices[0].message.content
    const auto json = crow::json::load(h.body);
    std::string text;
    if (json && json.has("choices") && json["choices"].t() == crow::json::type::List && json["choices"].size() > 0 &&
        json["choices"][0].has("message") && json["choices"][0]["message"].has("content") &&
        json["choices"][0]["message"]["content"].t() == crow::json::type::String) {
        text = json["choices"][0]["message"]["content"].s();
        if (json.has("model") && json["model"].t() == crow::json::type::String) r.model = json["model"].s();
    } else {
        r.status = Result::Status::BadReply;
        r.error = "回覆不是 chat completions 格式";
        return r;
    }
    r.rawReply = text;
    if (!parseVerdict(text, r.alert, r.cause, r.error)) {
        r.status = Result::Status::BadReply;
        return r;
    }
    r.status = Result::Status::Ok;
    return r;
}
