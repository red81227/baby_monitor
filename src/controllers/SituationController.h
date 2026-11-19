#pragma once

#include "alerts/AlertManager.h"
#include "controllers/RoiController.h"
#include "events/EventRecorder.h"
#include "judge/PatrolScheduler.h"
#include "judge/VlmJudge.h"
#include "situation/RulesStore.h"
#include "situation/SituationMonitor.h"

#include <crow.h>

#include <cstdio>
#include <string>

// 唯讀的狀況 / 警報 / 事件片段 API (規則只能以修改檔案的方式變更，這裡不提供任何寫入；
// 唯一的例外 ROI 由 RoiController 寫入)
class SituationController {
public:
    struct Deps {
        RulesStore& rules;
        SituationMonitor& monitor;
        AlertManager& alerts;
        EventRecorder& recorder;
        VlmJudge& judge;
        PatrolScheduler& patrols;
    };

    static std::string conditionText(const Condition& c) {
        std::string s(signalInfo(c.signal).name);
        char buf[48];
        switch (c.op) {
            case Condition::Op::Is: s += c.value != 0 ? " is true" : " is false"; break;
            case Condition::Op::Above: std::snprintf(buf, sizeof(buf), " above %g", c.value); s += buf; break;
            case Condition::Op::Below: std::snprintf(buf, sizeof(buf), " below %g", c.value); s += buf; break;
        }
        if (c.forUs > 0) {
            std::snprintf(buf, sizeof(buf), " for %gs", c.forUs / 1e6);
            s += buf;
        }
        if (c.graceUs > 0) {
            std::snprintf(buf, sizeof(buf), " grace %gs", c.graceUs / 1e6);
            s += buf;
        }
        return s;
    }

    static void registerRoutes(crow::SimpleApp& app, Deps d) {
        CROW_ROUTE(app, "/api/situations")([d]() {
            const auto st = d.rules.status();
            const auto active = d.monitor.activeStates();
            crow::json::wvalue j;
            j["path"] = st.rules ? st.rules->path : "";
            j["version"] = st.rules ? st.rules->version : 0;
            j["hash"] = st.rules ? st.rules->hash : "";
            j["loadedAtEpochMs"] = st.loadedAtEpochMs;
            RoiController::fillRoi(j, st.rules.get());
            if (st.lastError) j["error"] = *st.lastError;
            else j["error"] = nullptr;

            std::vector<crow::json::wvalue> warnings;
            if (st.rules) {
                for (const auto& w : st.rules->warnings) warnings.emplace_back(w);
            }
            j["warnings"] = std::move(warnings);

            j["vlm"]["enabled"] = d.judge.enabled();
            j["vlm"]["available"] = d.judge.enabled() && d.judge.available();
            j["vlm"]["model"] = d.judge.model();
            j["vlm"]["error"] = d.judge.enabled() ? d.judge.lastHealthError() : "未設定 BM_VLM_URL";

            std::vector<crow::json::wvalue> list;
            if (st.rules) {
                for (const auto& s : st.rules->situations) {
                    crow::json::wvalue x;
                    x["id"] = s.id;
                    x["severity"] = toString(s.severity);
                    x["route"] = toString(s.route);
                    x["shadow"] = s.shadow;
                    x["enabled"] = s.enabled;
                    const auto it = active.find(s.id);
                    x["active"] = it != active.end() && it->second.active;
                    std::vector<crow::json::wvalue> conds;
                    for (const auto& c : s.when) conds.emplace_back(conditionText(c));
                    x["when"] = std::move(conds);
                    x["cooldownS"] = s.cooldownUs / 1e6;
                    if (s.route == Route::Suppress) {
                        x["suppressBelow"] = toString(s.suppressBelow);
                        x["lingerS"] = s.lingerUs / 1e6;
                    }
                    x["prompt"] = s.promptPath;
                    list.push_back(std::move(x));
                }
            }
            j["situations"] = std::move(list);

            // 巡檢：設定來自規則，狀態來自 PatrolScheduler (規則剛換過、排程尚未同步時狀態為預設值)
            std::map<std::string, PatrolScheduler::Status> pst;
            for (auto& ps : d.patrols.status()) pst[ps.id] = ps;
            std::vector<crow::json::wvalue> plist;
            if (st.rules) {
                for (const auto& p : st.rules->patrols) {
                    crow::json::wvalue x;
                    x["id"] = p.id;
                    x["severity"] = toString(p.severity);
                    x["everyS"] = p.everyUs / 1e6;
                    x["frames"] = p.frames;
                    x["confirm"] = p.confirm;
                    x["cooldownS"] = p.cooldownUs / 1e6;
                    x["shadow"] = p.shadow;
                    x["enabled"] = p.enabled;
                    x["prompt"] = p.promptPath;
                    std::vector<crow::json::wvalue> conds;
                    for (const auto& c : p.whileConds) conds.emplace_back(conditionText(c));
                    x["while"] = std::move(conds);
                    const auto it = pst.find(p.id);
                    const PatrolScheduler::Status s = it != pst.end() ? it->second : PatrolScheduler::Status{};
                    x["paused"] = s.paused;
                    x["stalled"] = s.stalled;
                    if (s.stalled) x["stallReason"] = s.stallReason;
                    else x["stallReason"] = nullptr;
                    x["inFlight"] = s.inFlight;
                    x["positives"] = s.positives;
                    x["lastOkEpochMs"] = s.lastOkEpochMs;
                    if (s.lastAlert) x["lastAlert"] = *s.lastAlert;
                    else x["lastAlert"] = nullptr;
                    x["lastCause"] = s.lastCause;
                    if (s.lastError.empty()) x["lastError"] = nullptr;
                    else x["lastError"] = s.lastError;
                    plist.push_back(std::move(x));
                }
            }
            j["patrols"] = std::move(plist);

            if (const auto sig = d.monitor.lastSignals()) {
                for (const auto& info : kSignals) {
                    const auto& v = (*sig)[info.id];
                    const std::string name(info.name);
                    if (!v) j["signals"][name] = nullptr;
                    else if (info.isBool) j["signals"][name] = *v != 0.0;
                    else j["signals"][name] = *v;
                }
            } else {
                j["signals"] = nullptr;
            }
            crow::response res{j};
            res.set_header("Cache-Control", "no-store");
            return res;
        });

        CROW_ROUTE(app, "/api/alerts")([d]() {
            std::string body = "{\"alerts\":[";
            bool first = true;
            for (const auto& a : d.alerts.recentJson()) {
                body += first ? "" : ",";
                body += a;
                first = false;
            }
            body += "],\"lastAlertEpochMs\":" + std::to_string(d.alerts.lastAlertEpochMs()) + "}";
            crow::response res(200, body);
            res.set_header("Content-Type", "application/json");
            res.set_header("Cache-Control", "no-store");
            return res;
        });

        CROW_ROUTE(app, "/api/events/<string>")([d](const std::string& id) {
            if (!EventRecorder::isValidId(id)) return crow::response(400, "invalid event id");
            const auto meta = d.recorder.readMeta(id);
            if (!meta) return crow::response(404, "event not found");
            crow::response res(200, *meta);
            res.set_header("Content-Type", "application/json");
            return res;
        });

        CROW_ROUTE(app, "/api/events/<string>/<string>")([d](const std::string& id, const std::string& file) {
            if (!EventRecorder::isValidId(id)) return crow::response(400, "invalid event id");
            const auto jpg = d.recorder.readFrame(id, file);
            if (!jpg) return crow::response(404, "frame not found");
            crow::response res(200, *jpg);
            res.set_header("Content-Type", "image/jpeg");
            res.set_header("Cache-Control", "max-age=86400");
            return res;
        });
    }
};
