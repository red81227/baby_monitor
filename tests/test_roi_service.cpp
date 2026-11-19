#include "check.h"
#include "roi/RoiService.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path g_dir;

const std::string kRules = R"(version: 2
bed_roi: [0.2, 0.15, 0.6, 0.75]
situations:
  - id: near_bed_side
    severity: warning
    route: direct
    when:
      - { signal: near_bed_edge, is: true, for: 5s }
    cooldown: 5m
)";

void writeFile(const fs::path& p, const std::string& text) {
    static int bump = 0;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
    fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(++bump));
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool near(const std::optional<cv::Rect2f>& r, float x, float y, float w, float h) {
    return r && std::abs(r->x - x) < 1e-5 && std::abs(r->y - y) < 1e-5 && std::abs(r->width - w) < 1e-5 &&
           std::abs(r->height - h) < 1e-5;
}

using Roi = std::optional<std::vector<double>>;

} // namespace

int main() {
    char tmpl[] = "/tmp/bm_test_roi_service_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    g_dir = tmpl;
    const fs::path rulesPath = g_dir / "situations.yaml";
    const fs::path roiPath = g_dir / "overrides" / "bed_roi.json";  // 目錄不存在：第一次寫入時建立
    writeFile(rulesPath, kRules);
    const auto rulesMtime = fs::last_write_time(rulesPath);

    RulesStore store(rulesPath.string(), 60, std::chrono::milliseconds(2000), roiPath.string());
    std::string error;
    CHECK(store.loadInitial(error));
    std::vector<std::string> log;
    RoiService svc(store, [&](const std::string& c) { log.push_back(c); }, [] { return int64_t(1795000000000); });

    // 設定成功：回應前已生效，回傳新的雜湊值；規則檔不被改動
    const std::string h0 = store.current()->hash;
    auto r = svc.set(Roi({0.1, 0.1, 0.5, 0.5}), h0, "192.168.1.23");
    CHECK(r.status == 200 && r.error.empty());
    CHECK(r.rules->hash != h0);
    CHECK(store.current()->hash == r.rules->hash);
    CHECK(store.current()->bedRoiSource == RoiSource::Override);
    CHECK(near(store.current()->params.bedRoi, 0.1f, 0.1f, 0.5f, 0.5f));
    CHECK(near(store.current()->fileBedRoi, 0.2f, 0.15f, 0.6f, 0.75f));
    CHECK(store.current()->version == 2);
    CHECK(readFile(rulesPath) == kRules);
    CHECK(fs::last_write_time(rulesPath) == rulesMtime);
    CHECK(!fs::exists(roiPath.string() + ".tmp"));
    {
        const std::string text = readFile(roiPath);
        CHECK(text.find("[0.100, 0.100, 0.500, 0.500]") != std::string::npos);
        CHECK(text.find("192.168.1.23") != std::string::npos);
    }
    // 修改紀錄：舊值與來源、新值與來源、來源 IP
    CHECK(log.size() == 1);
    CHECK(log[0].find("[0.200, 0.150, 0.600, 0.750]（規則檔）") != std::string::npos);
    CHECK(log[0].find("[0.100, 0.100, 0.500, 0.500]（網頁設定）") != std::string::npos);
    CHECK(log[0].find("192.168.1.23") != std::string::npos);
    // 背景輪詢不會再載入一次
    CHECK(!store.checkNow());

    // 四捨五入到小數 3 位，右下角超出時縮回畫面內
    std::string h = store.current()->hash;
    r = svc.set(Roi({0.0004, 0.12345, 0.9996, 0.5}), h, "ip");
    CHECK(r.status == 200);
    CHECK(near(store.current()->params.bedRoi, 0.0f, 0.123f, 1.0f, 0.5f));
    h = store.current()->hash;
    r = svc.set(Roi({0.0005, 0.0, 0.9995, 0.5}), h, "ip");  // 兩者都進位會超出 0.001
    CHECK(r.status == 200);
    const auto& b = *store.current()->params.bedRoi;
    CHECK(b.x + b.width <= 1.0f + 1e-6f);

    // 驗證失敗：400，生效的 ROI 不變，不寫紀錄
    h = store.current()->hash;
    const auto before = store.current();
    const size_t logBefore = log.size();
    for (const Roi& bad : {Roi({0.8, 0.1, 0.5, 0.5}),   // 超出畫面
                           Roi({0.1, 0.1, 0.01, 0.5}),  // 太小
                           Roi({0.1, 0.1, 0.5, 0.04}),  // 太小
                           Roi({-0.1, 0.1, 0.5, 0.5}),  // 負數
                           Roi({0.1, 0.1, 0.5}),        // 數量不對
                           Roi(std::nullopt),           // null：不能清除
                           Roi(std::vector<double>{})}) {
        r = svc.set(bad, h, "ip");
        CHECK(r.status == 400);
        CHECK(!r.error.empty());
        CHECK(store.current() == before);
    }
    CHECK(log.size() == logBefore);
    {
        r = svc.set(Roi(std::nullopt), h, "ip");
        CHECK(r.error.find("清除") != std::string::npos);
    }

    // 雜湊值不同 (兩個頁面同時編輯)：409，回傳目前的 ROI，不覆蓋
    {
        const std::string stale = h;
        CHECK(svc.set(Roi({0.3, 0.3, 0.3, 0.3}), h, "A").status == 200);  // A 先存
        const auto afterA = store.current();
        r = svc.set(Roi({0.4, 0.4, 0.3, 0.3}), stale, "B");                 // B 用舊值
        CHECK(r.status == 409);
        CHECK(r.rules == afterA);
        CHECK(near(r.rules->params.bedRoi, 0.3f, 0.3f, 0.3f, 0.3f));
        CHECK(svc.reset(stale, "B").status == 409);
        CHECK(store.current() == afterA);
        // 缺少 If-Match
        CHECK(svc.set(Roi({0.4, 0.4, 0.3, 0.3}), "", "B").status == 400);
    }

    // 重啟後仍有效
    {
        RulesStore again(rulesPath.string(), 60, std::chrono::milliseconds(2000), roiPath.string());
        CHECK(again.loadInitial(error));
        CHECK(again.current()->bedRoiSource == RoiSource::Override);
        CHECK(near(again.current()->params.bedRoi, 0.3f, 0.3f, 0.3f, 0.3f));
    }

    // 還原：回到規則檔的值；紀錄寫入
    log.clear();
    r = svc.reset(store.current()->hash, "192.168.1.23");
    CHECK(r.status == 200);
    CHECK(!fs::exists(roiPath));
    CHECK(store.current()->bedRoiSource == RoiSource::Rules);
    CHECK(near(store.current()->params.bedRoi, 0.2f, 0.15f, 0.6f, 0.75f));
    CHECK(log.size() == 1 && log[0].find("還原") != std::string::npos);
    // 沒有可還原的設定：成功，不變，不寫紀錄
    {
        const auto cur = store.current();
        r = svc.reset(cur->hash, "ip");
        CHECK(r.status == 200);
        CHECK(store.current() == cur);
        CHECK(log.size() == 1);
    }

    // 規則載入失敗時拒絕寫入，ROI 檔不變
    {
        CHECK(svc.set(Roi({0.1, 0.1, 0.5, 0.5}), store.current()->hash, "ip").status == 200);
        const std::string roiText = readFile(roiPath);
        writeFile(rulesPath, "version: 3\nsituations: [\n");
        CHECK(!store.checkNow());
        const std::string hh = store.current()->hash;
        r = svc.set(Roi({0.2, 0.2, 0.5, 0.5}), hh, "ip");
        CHECK(r.status == 409);
        CHECK(r.error.find("YAML") != std::string::npos);
        CHECK(readFile(roiPath) == roiText);
        CHECK(svc.reset(hh, "ip").status == 409);
        CHECK(fs::exists(roiPath));
        writeFile(rulesPath, kRules);
        CHECK(store.checkNow());
    }

    // 目錄無法寫入：500，規則不變 (root 執行時權限不生效，略過)
    if (geteuid() != 0) {
        const fs::path roDir = g_dir / "readonly";
        fs::create_directories(roDir);
        RulesStore roStore(rulesPath.string(), 60, std::chrono::milliseconds(2000), (roDir / "bed_roi.json").string());
        CHECK(roStore.loadInitial(error));
        chmod(roDir.c_str(), 0555);
        RoiService roSvc(roStore, nullptr);
        const auto cur = roStore.current();
        r = roSvc.set(Roi({0.1, 0.1, 0.5, 0.5}), cur->hash, "ip");
        CHECK(r.status == 500);
        CHECK(r.error.find("無法寫入") != std::string::npos);
        CHECK(roStore.current() == cur);
        chmod(roDir.c_str(), 0755);
    }

    // 請求內容解析
    {
        Roi out;
        std::string err;
        CHECK(RoiService::parseBody(R"({"roi": [0.1, 0.2, 0.3, 0.4]})", out, err) && out && out->size() == 4);
        CHECK(RoiService::parseBody(R"({"roi": null})", out, err) && !out);
        CHECK(RoiService::parseBody(R"({"roi": []})", out, err) && !out);
        CHECK(!RoiService::parseBody("not json", out, err));
        CHECK(!RoiService::parseBody("[1,2,3,4]", out, err));
        CHECK(!RoiService::parseBody(R"({"bed_roi": [0.1, 0.2, 0.3, 0.4]})", out, err));
        CHECK(!RoiService::parseBody(R"({"roi": "0.1,0.2"})", out, err));
        CHECK(!RoiService::parseBody(R"({"roi": [0.1, "a", 0.3, 0.4]})", out, err));
    }

    // 跨站檢查
    {
        CHECK(RoiService::sameOrigin("", "192.168.1.5:18080"));  // 沒有 Origin (curl)
        CHECK(RoiService::sameOrigin("http://192.168.1.5:18080", "192.168.1.5:18080"));
        CHECK(RoiService::sameOrigin("http://BabyMon.local:18080", "babymon.local:18080"));
        CHECK(RoiService::sameOrigin("http://example.com", "example.com:80"));
        CHECK(RoiService::sameOrigin("https://example.com", "example.com"));
        CHECK(RoiService::sameOrigin("http://[::1]:18080", "[::1]:18080"));
        CHECK(!RoiService::sameOrigin("http://evil.example:18080", "192.168.1.5:18080"));  // 不同網站
        CHECK(!RoiService::sameOrigin("http://192.168.1.5:8080", "192.168.1.5:18080"));    // 不同 port
        CHECK(!RoiService::sameOrigin("https://192.168.1.5", "192.168.1.5:80"));           // 不同 scheme 預設 port
        CHECK(!RoiService::sameOrigin("null", "192.168.1.5:18080"));
        CHECK(!RoiService::sameOrigin("http://192.168.1.5:18080", ""));
        CHECK(!RoiService::sameOrigin("http://192.168.1.5:18080/x", "192.168.1.5:18080"));
    }

    fs::remove_all(g_dir);
    return 0;
}
