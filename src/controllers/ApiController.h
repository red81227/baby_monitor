#pragma once

#include "models/MonitorState.h"

#include <crow.h>

#include <functional>
#include <string>

class ApiController {
public:
    // extras：在 /api/status 加上其他模組的欄位 (既有欄位不變)
    using StatusExtras = std::function<void(crow::json::wvalue&)>;

    static void registerRoutes(crow::SimpleApp& app, MonitorState& state, StatusExtras extras = nullptr) {
        CROW_ROUTE(app, "/api/status")([&state, extras]() {
            const auto snap = state.snapshot();
            crow::json::wvalue payload;
            payload["motionDetected"] = snap->motionDetected;
            payload["personDetected"] = snap->personDetected;
            payload["lastMotionEpochMs"] = static_cast<int64_t>(snap->lastMotionEpochMs);
            payload["latestFrameBytes"] = static_cast<uint64_t>(snap->frame ? snap->frame->jpeg.size() : 0);
            payload["cameraConnected"] = snap->cameraConnected;
            payload["seq"] = static_cast<uint64_t>(snap->seq);
            payload["detectHz"] = snap->detectHz;
            if (extras) extras(payload);
            crow::response res{payload};
            res.set_header("Cache-Control", "no-store");
            return res;
        });

        CROW_ROUTE(app, "/api/frame.jpg")([&state]() {
            const auto snap = state.snapshot();
            if (!snap->frame || snap->frame->jpeg.empty()) {
                return crow::response(503, "Frame not ready");
            }

            const auto& jpeg = snap->frame->jpeg;
            crow::response res;
            res.code = 200;
            res.set_header("Content-Type", "image/jpeg");
            res.set_header("Cache-Control", "no-store");
            res.set_header("X-Frame-Seq", std::to_string(snap->frame->seq));
            res.body.assign(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
            return res;
        });
    }
};
