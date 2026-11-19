#include "situation/Rules.h"

#include "common/Sha256.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

const char* toString(Route r) {
    switch (r) {
        case Route::Direct: return "direct";
        case Route::Llm: return "llm";
        case Route::Suppress: return "suppress";
        case Route::Patrol: return "patrol";
        case Route::System: return "system";
    }
    return "?";
}

const char* toString(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Critical: return "critical";
    }
    return "?";
}

const char* toString(RoiSource s) {
    switch (s) {
        case RoiSource::None: return "none";
        case RoiSource::Rules: return "rules";
        case RoiSource::Override: return "override";
    }
    return "?";
}

std::string bedRoiFilePath(const std::string& overridesDir) {
    return (fs::path(overridesDir) / "bed_roi.json").string();
}

bool validateBedRoi(double x, double y, double w, double h, std::string& reason) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || x < 0 || y < 0 ||
        w <= 0 || h <= 0 || x + w > 1.0 + 1e-9 || y + h > 1.0 + 1e-9) {
        reason = "bed_roi 必須以畫面比例表示 (0–1)，且寬高大於 0、不超出畫面";
        return false;
    }
    return true;
}

bool Condition::holds(double v) const {
    switch (op) {
        case Op::Is: return (v != 0.0) == (value != 0.0);
        case Op::Above: return v > value;
        case Op::Below: return v < value;
    }
    return false;
}

const Situation* Rules::find(const std::string& id) const {
    for (const auto& s : situations) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const Patrol* Rules::findPatrol(const std::string& id) const {
    for (const auto& p : patrols) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::optional<int64_t> parseDurationUs(const std::string& text) {
    static const std::regex re(R"(^\s*(-?)(\d+(?:\.\d+)?)\s*(ms|s|m|h)\s*$)");
    std::smatch m;
    if (!std::regex_match(text, m, re)) return std::nullopt;
    const double n = std::stod(m[2].str());
    const std::string unit = m[3].str();
    const double us = unit == "ms" ? n * 1e3 : unit == "s" ? n * 1e6 : unit == "m" ? n * 60e6 : n * 3600e6;
    if (us > 1e15) return std::nullopt;
    const int64_t v = static_cast<int64_t>(std::llround(us));
    return m[1].str() == "-" ? -v : v;
}

namespace {

// 驗證錯誤：帶有出錯節點的位置與狀況 id
struct RuleError : std::runtime_error {
    RuleError(const YAML::Node& node, const std::string& situation, const std::string& msg)
        : std::runtime_error(msg), line(node.IsDefined() ? node.Mark().line + 1 : 0), situation(situation) {}
    int line;
    std::string situation;
};

std::string readFile(const fs::path& p, bool& ok) {
    std::ifstream in(p, std::ios::binary);
    ok = static_cast<bool>(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class Parser {
public:
    Parser(const fs::path& path, int ringSeconds) : path_(path), ringUs_(int64_t(ringSeconds) * 1'000'000) {}

    Rules parse(const YAML::Node& root) {
        Rules rules;
        if (!root.IsMap()) throw RuleError(root, "", "規則檔最外層必須是對應表 (key: value)");
        checkKeys(root, "", {"version", "keypoint_visibility", "bed_roi", "bed_edge_margin", "situations", "patrols"});

        const YAML::Node version = root["version"];
        if (!version) throw RuleError(root, "", "缺少 version");
        rules.version = asInt(version, "", "version");

        if (const auto n = root["keypoint_visibility"]) {
            rules.params.keypointVisibility = static_cast<float>(asNumber(n, "", "keypoint_visibility"));
            if (rules.params.keypointVisibility < 0 || rules.params.keypointVisibility > 1) {
                throw RuleError(n, "", "keypoint_visibility 必須介於 0 與 1");
            }
        }
        if (const auto n = root["bed_roi"]) {
            if (!n.IsSequence() || n.size() != 4) throw RuleError(n, "", "bed_roi 必須是 [x, y, w, h] 四個數字");
            const double x = asNumber(n[0], "", "bed_roi"), y = asNumber(n[1], "", "bed_roi");
            const double w = asNumber(n[2], "", "bed_roi"), h = asNumber(n[3], "", "bed_roi");
            if (std::string reason; !validateBedRoi(x, y, w, h, reason)) throw RuleError(n, "", reason);
            rules.params.bedRoi = cv::Rect2f(float(x), float(y), float(w), float(h));
        }
        if (const auto n = root["bed_edge_margin"]) {
            rules.params.bedEdgeMargin = static_cast<float>(asNumber(n, "", "bed_edge_margin"));
            if (rules.params.bedEdgeMargin < 0 || rules.params.bedEdgeMargin > 0.5) {
                throw RuleError(n, "", "bed_edge_margin 必須介於 0 與 0.5");
            }
        }

        const YAML::Node list = root["situations"];
        if (!list || !list.IsSequence() || list.size() == 0) {
            throw RuleError(list ? list : root, "", "situations 必須是至少有一個狀況的清單");
        }
        std::set<std::string> ids;
        for (const auto& node : list) {
            Situation s = parseSituation(node);
            if (!ids.insert(s.id).second) throw RuleError(node, s.id, "狀況 id 重複");
            if (s.severity == Severity::Critical && (!s.enabled || s.shadow)) {
                rules.warnings.push_back("critical 狀況 " + s.id + (s.shadow ? " 為影子模式" : " 已停用") +
                                         "，不會送出提醒");
            }
            rules.situations.push_back(std::move(s));
        }
        if (const YAML::Node patrols = root["patrols"]) {
            if (!patrols.IsSequence()) throw RuleError(patrols, "", "patrols 必須是巡檢的清單");
            for (const auto& node : patrols) {
                Patrol p = parsePatrol(node);
                if (!ids.insert(p.id).second) throw RuleError(node, p.id, "id 重複 (巡檢與狀況的 id 不可相同)");
                if (p.severity == Severity::Critical && (!p.enabled || p.shadow)) {
                    rules.warnings.push_back("critical 巡檢 " + p.id + (p.shadow ? " 為影子模式" : " 已停用") +
                                             "，不會送出提醒");
                }
                rules.patrols.push_back(std::move(p));
            }
        }
        rules.watchedFiles = watched_;
        return rules;
    }

private:
    static void checkKeys(const YAML::Node& map, const std::string& sid, std::set<std::string> allowed) {
        for (const auto& kv : map) {
            const std::string key = kv.first.as<std::string>();
            if (!allowed.count(key)) throw RuleError(kv.first, sid, "不認得的欄位 " + key);
        }
    }

    static double asNumber(const YAML::Node& n, const std::string& sid, const std::string& what) {
        if (!n.IsScalar()) throw RuleError(n, sid, what + " 必須是數字");
        try {
            const double v = n.as<double>();
            if (!std::isfinite(v)) throw YAML::Exception(n.Mark(), "");
            return v;
        } catch (const YAML::Exception&) {
            throw RuleError(n, sid, what + " 必須是數字 (目前為 \"" + n.Scalar() + "\")");
        }
    }

    static int asInt(const YAML::Node& n, const std::string& sid, const std::string& what) {
        if (!n.IsScalar()) throw RuleError(n, sid, what + " 必須是整數");
        try {
            return n.as<int>();
        } catch (const YAML::Exception&) {
            throw RuleError(n, sid, what + " 必須是整數 (目前為 \"" + n.Scalar() + "\")");
        }
    }

    static bool asBool(const YAML::Node& n, const std::string& sid, const std::string& what) {
        if (!n.IsScalar() || (n.Scalar() != "true" && n.Scalar() != "false")) {
            throw RuleError(n, sid, what + " 必須是 true 或 false");
        }
        return n.Scalar() == "true";
    }

    static std::string asString(const YAML::Node& n, const std::string& sid, const std::string& what) {
        if (!n.IsScalar() || n.Scalar().empty()) throw RuleError(n, sid, what + " 必須是非空字串");
        return n.Scalar();
    }

    static int64_t asDuration(const YAML::Node& n, const std::string& sid, const std::string& what, bool allowNegative) {
        const std::string text = n.IsScalar() ? n.Scalar() : "";
        const auto us = parseDurationUs(text);
        if (!us) throw RuleError(n, sid, what + " 必須是時間，例如 500ms、10s、5m (目前為 \"" + text + "\")");
        if (*us < 0 && !allowNegative) throw RuleError(n, sid, what + " 不可為負數");
        return *us;
    }

    template <typename T>
    static T asEnum(const YAML::Node& n, const std::string& sid, const std::string& what,
                    std::initializer_list<std::pair<const char*, T>> options) {
        const std::string v = n.IsScalar() ? n.Scalar() : "";
        std::string names;
        for (const auto& [name, value] : options) {
            if (v == name) return value;
            names += names.empty() ? name : std::string("、") + name;
        }
        throw RuleError(n, sid, what + " 必須是 " + names + " 之一 (目前為 \"" + v + "\")");
    }

    Condition parseCondition(const YAML::Node& n, const std::string& sid) {
        if (!n.IsMap()) throw RuleError(n, sid, "條件必須是對應表，例如 { signal: face_visible, is: false, for: 10s }");
        checkKeys(n, sid, {"signal", "is", "above", "below", "for", "grace"});
        const YAML::Node sig = n["signal"];
        if (!sig) throw RuleError(n, sid, "條件缺少 signal");
        const std::string name = asString(sig, sid, "signal");
        const auto signal = signalByName(name);
        if (!signal) {
            std::string all;
            for (const auto& info : kSignals) all += (all.empty() ? "" : "、") + std::string(info.name);
            throw RuleError(sig, sid, "不認得的訊號 " + name + " (可用的訊號: " + all + ")");
        }
        const bool isBool = signalInfo(*signal).isBool;

        Condition c{*signal, Condition::Op::Is, 0, 0};
        int ops = 0;
        if (const auto v = n["is"]) {
            ++ops;
            if (!isBool) throw RuleError(v, sid, "訊號 " + name + " 是數字，請用 above 或 below");
            c.op = Condition::Op::Is;
            c.value = asBool(v, sid, "is") ? 1.0 : 0.0;
        }
        for (auto [key, op] : {std::pair{"above", Condition::Op::Above}, std::pair{"below", Condition::Op::Below}}) {
            if (const auto v = n[key]) {
                ++ops;
                if (isBool) throw RuleError(v, sid, "訊號 " + name + " 是布林值，請用 is: true / false");
                c.op = op;
                c.value = asNumber(v, sid, key);
            }
        }
        if (ops != 1) throw RuleError(n, sid, "條件必須剛好有 is、above、below 其中一個");
        if (const auto f = n["for"]) c.forUs = asDuration(f, sid, "for", false);
        if (const auto g = n["grace"]) c.graceUs = asDuration(g, sid, "grace", false);
        return c;
    }

    std::string readPrompt(const YAML::Node& p, const std::string& sid, std::string& text) {
        const fs::path rel = asString(p, sid, "prompt");
        const fs::path full = rel.is_absolute() ? rel : path_.parent_path() / rel;
        bool ok = false;
        text = readFile(full, ok);
        if (!ok || !fs::is_regular_file(full)) throw RuleError(p, sid, "找不到 prompt 檔 " + full.string());
        watched_.push_back(full.string());
        return full.string();
    }

    void parseClip(const YAML::Node& n, const std::string& sid, int64_t& preUs, int64_t& postUs) {
        if (const auto clip = n["clip"]) {
            if (!clip.IsSequence() || clip.size() != 2) throw RuleError(clip, sid, "clip 必須是 [前段, 後段]，例如 [-20s, 5s]");
            const int64_t pre = asDuration(clip[0], sid, "clip 前段", true);
            const int64_t post = asDuration(clip[1], sid, "clip 後段", false);
            if (pre > 0) throw RuleError(clip[0], sid, "clip 前段必須是 0 或負數 (觸發前)，例如 -20s");
            if (post > 60'000'000) throw RuleError(clip[1], sid, "clip 後段不可超過 60s");
            preUs = -pre;
            postUs = post;
        }
        if (preUs > ringUs_) {
            throw RuleError(n["clip"] ? n["clip"] : n, sid,
                            "clip 前段 (" + std::to_string(preUs / 1'000'000) + "s) 超過 BM_RING_SECONDS (" +
                                std::to_string(ringUs_ / 1'000'000) + "s)");
        }
    }

    void parseTimeout(const YAML::Node& n, const std::string& sid, int64_t& timeoutUs) {
        if (const auto t = n["timeout"]) {
            timeoutUs = asDuration(t, sid, "timeout", false);
            if (timeoutUs <= 0 || timeoutUs > 300'000'000) throw RuleError(t, sid, "timeout 必須介於 0 與 300s");
        }
    }

    static std::string parseId(const YAML::Node& n, const char* what) {
        const YAML::Node idNode = n["id"];
        if (!idNode) throw RuleError(n, "", std::string(what) + "缺少 id");
        const std::string id = asString(idNode, "", "id");
        static const std::regex idRe("^[a-z0-9_]+$");
        if (!std::regex_match(id, idRe)) throw RuleError(idNode, id, "id 只能使用小寫英文、數字與底線");
        return id;
    }

    static int asIntInRange(const YAML::Node& n, const std::string& sid, const std::string& what, int lo, int hi) {
        const int v = asInt(n, sid, what);
        if (v < lo || v > hi) {
            throw RuleError(n, sid, what + " 必須介於 " + std::to_string(lo) + " 與 " + std::to_string(hi));
        }
        return v;
    }

    Patrol parsePatrol(const YAML::Node& n) {
        if (!n.IsMap()) throw RuleError(n, "", "每個巡檢必須是對應表");
        Patrol p;
        p.id = parseId(n, "巡檢");
        const std::string& sid = p.id;
        checkKeys(n, sid, {"id", "severity", "every", "frames", "confirm", "cooldown", "clip", "prompt", "while",
                           "timeout", "shadow", "enabled"});
        for (const char* req : {"severity", "every", "prompt", "cooldown"}) {
            if (!n[req]) throw RuleError(n, sid, std::string("缺少 ") + req);
        }
        p.severity = asEnum<Severity>(n["severity"], sid, "severity",
                                      {{"info", Severity::Info}, {"warning", Severity::Warning},
                                       {"critical", Severity::Critical}});
        p.everyUs = asDuration(n["every"], sid, "every", false);
        if (p.everyUs < 5'000'000 || p.everyUs > 3600'000'000LL) throw RuleError(n["every"], sid, "every 必須介於 5s 與 1h");
        if (const auto v = n["frames"]) p.frames = asIntInRange(v, sid, "frames", 1, 6);
        if (const auto v = n["confirm"]) p.confirm = asIntInRange(v, sid, "confirm", 1, 5);
        p.cooldownUs = asDuration(n["cooldown"], sid, "cooldown", false);
        parseClip(n, sid, p.clipPreUs, p.clipPostUs);
        p.promptPath = readPrompt(n["prompt"], sid, p.promptText);
        if (const auto w = n["while"]) {
            if (!w.IsSequence() || w.size() == 0) throw RuleError(w, sid, "while 必須是至少有一個條件的清單 (省略表示一律巡檢)");
            for (const auto& c : w) p.whileConds.push_back(parseCondition(c, sid));
        }
        parseTimeout(n, sid, p.timeoutUs);
        if (p.timeoutUs >= p.everyUs) throw RuleError(n["timeout"] ? n["timeout"] : n, sid, "timeout 必須小於 every");
        if (const auto v = n["shadow"]) p.shadow = asBool(v, sid, "shadow");
        if (const auto v = n["enabled"]) p.enabled = asBool(v, sid, "enabled");
        return p;
    }

    Situation parseSituation(const YAML::Node& n) {
        if (!n.IsMap()) throw RuleError(n, "", "每個狀況必須是對應表");
        Situation s;
        s.id = parseId(n, "狀況");
        const std::string& sid = s.id;

        checkKeys(n, sid, {"id", "severity", "route", "when", "cooldown", "clip", "prompt", "timeout", "on_timeout",
                           "shadow", "enabled", "suppress"});

        for (const char* req : {"severity", "route", "when", "cooldown"}) {
            if (!n[req]) throw RuleError(n, sid, std::string("缺少 ") + req);
        }
        s.severity = asEnum<Severity>(n["severity"], sid, "severity",
                                      {{"info", Severity::Info}, {"warning", Severity::Warning},
                                       {"critical", Severity::Critical}});
        s.route = asEnum<Route>(n["route"], sid, "route",
                                {{"direct", Route::Direct}, {"llm", Route::Llm}, {"suppress", Route::Suppress}});

        const YAML::Node when = n["when"];
        if (!when.IsSequence() || when.size() == 0) throw RuleError(when, sid, "when 必須是至少有一個條件的清單");
        for (const auto& c : when) s.when.push_back(parseCondition(c, sid));

        s.cooldownUs = asDuration(n["cooldown"], sid, "cooldown", false);

        parseClip(n, sid, s.clipPreUs, s.clipPostUs);

        if (const auto p = n["prompt"]) s.promptPath = readPrompt(p, sid, s.promptText);
        if (s.route == Route::Llm && s.promptPath.empty()) throw RuleError(n, sid, "路由為 llm 的狀況必須指定 prompt");

        parseTimeout(n, sid, s.timeoutUs);
        if (const auto t = n["on_timeout"]) {
            s.onTimeoutAlert = asEnum<bool>(t, sid, "on_timeout", {{"alert", true}, {"drop", false}});
        }
        if (const auto v = n["shadow"]) s.shadow = asBool(v, sid, "shadow");
        if (const auto v = n["enabled"]) s.enabled = asBool(v, sid, "enabled");

        const YAML::Node sup = n["suppress"];
        if (s.route == Route::Suppress) {
            if (!sup || !sup.IsMap()) throw RuleError(n, sid, "路由為 suppress 的狀況必須有 suppress: { below: ..., linger: ... }");
            checkKeys(sup, sid, {"below", "linger"});
            if (!sup["below"]) throw RuleError(sup, sid, "suppress 缺少 below");
            // below 只接受 warning / critical：critical 警報因此一定無法被壓制
            s.suppressBelow = asEnum<Severity>(sup["below"], sid, "suppress.below",
                                               {{"warning", Severity::Warning}, {"critical", Severity::Critical}});
            if (const auto l = sup["linger"]) s.lingerUs = asDuration(l, sid, "suppress.linger", false);
        } else if (sup) {
            throw RuleError(sup, sid, "只有路由為 suppress 的狀況可以設定 suppress");
        }

        // 底線保護
        if (s.severity == Severity::Critical && s.route != Route::Direct) {
            throw RuleError(n["route"], sid,
                            std::string("critical 狀況的路由必須是 direct (不可交給 LLM 決定或用來壓制)，目前為 ") +
                                toString(s.route));
        }
        return s;
    }

    fs::path path_;
    int64_t ringUs_;
    std::vector<std::string> watched_;
};

} // namespace

namespace {

// ROI 檔：JSON (YAML 的子集)，只認 bed_roi，其他欄位只是資訊、忽略
cv::Rect2f parseRoiFile(const std::string& text, const std::string& roiPath) {
    const std::string prefix = "ROI 檔 " + roiPath + ": ";
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(prefix + "格式錯誤: " + e.msg);
    }
    if (!root.IsMap()) throw std::runtime_error(prefix + "最外層必須是物件");
    const YAML::Node n = root["bed_roi"];
    if (!n || !n.IsSequence() || n.size() != 4) throw std::runtime_error(prefix + "bed_roi 必須是 [x, y, w, h] 四個數字");
    double v[4];
    for (size_t i = 0; i < 4; ++i) {
        try {
            if (!n[i].IsScalar()) throw YAML::Exception(n[i].Mark(), "");
            v[i] = n[i].as<double>();
        } catch (const YAML::Exception&) {
            throw std::runtime_error(prefix + "bed_roi 必須是 [x, y, w, h] 四個數字");
        }
    }
    if (std::string reason; !validateBedRoi(v[0], v[1], v[2], v[3], reason)) throw std::runtime_error(prefix + reason);
    return cv::Rect2f(float(v[0]), float(v[1]), float(v[2]), float(v[3]));
}

} // namespace

std::optional<Rules> loadRules(const std::string& path, int ringSeconds, std::string& error,
                               const std::string& roiPath) {
    const std::string prefix = "錯誤: 規則檔 " + path;
    bool ok = false;
    const std::string text = readFile(path, ok);
    if (!ok || !fs::is_regular_file(path)) {
        error = prefix + " 無法讀取 (檔案不存在或沒有權限)";
        return std::nullopt;
    }

    try {
        const YAML::Node root = YAML::Load(text);
        Parser parser(path, ringSeconds);
        Rules rules = parser.parse(root);
        rules.path = path;
        rules.watchedFiles.insert(rules.watchedFiles.begin(), path);
        rules.fileBedRoi = rules.params.bedRoi;
        rules.bedRoiSource = rules.params.bedRoi ? RoiSource::Rules : RoiSource::None;

        std::string all = text;
        if (!roiPath.empty()) {
            // ROI 檔不論是否存在都監看：被建立時也要重新載入
            rules.roiPath = roiPath;
            rules.watchedFiles.insert(rules.watchedFiles.begin() + 1, roiPath);
            std::error_code ec;
            if (fs::exists(roiPath, ec)) {
                bool roiOk = false;
                const std::string roiText = readFile(roiPath, roiOk);
                if (!roiOk || !fs::is_regular_file(roiPath, ec)) {
                    error = "錯誤: ROI 檔 " + roiPath + " 無法讀取 (沒有權限或不是檔案)";
                    return std::nullopt;
                }
                try {
                    rules.params.bedRoi = parseRoiFile(roiText, roiPath);
                } catch (const std::runtime_error& e) {
                    error = std::string("錯誤: ") + e.what();
                    return std::nullopt;
                }
                rules.bedRoiSource = RoiSource::Override;
                all += "\n--roi\n" + roiText;
            }
        }
        for (const auto& s : rules.situations) all += "\n--prompt " + s.id + "\n" + s.promptText;
        for (const auto& p : rules.patrols) all += "\n--prompt " + p.id + "\n" + p.promptText;
        rules.hash = sha256Hex(all).substr(0, 12);
        return rules;
    } catch (const RuleError& e) {
        error = prefix;
        if (e.line > 0) error += " 第 " + std::to_string(e.line) + " 行";
        if (!e.situation.empty()) error += " (狀況 " + e.situation + ")";
        error += ": " + std::string(e.what());
    } catch (const YAML::Exception& e) {
        error = prefix + " 第 " + std::to_string(e.mark.line + 1) + " 行: YAML 格式錯誤: " + e.msg;
    }
    return std::nullopt;
}
