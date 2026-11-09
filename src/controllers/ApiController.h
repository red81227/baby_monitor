#pragma once

#include "models/MonitorState.h"

#include <crow.h>

class ApiController {
public:
    static void registerRoutes(crow::SimpleApp& app, MonitorState& state) {
        CROW_ROUTE(app, "/api/status")([&state]() {
            const auto status = state.getStatus();
            crow::json::wvalue payload;
            payload["motionDetected"] = status.motionDetected;
            payload["personDetected"] = status.personDetected;
            payload["lastMotionEpochMs"] = static_cast<int64_t>(status.lastMotionEpochMs);
            payload["latestFrameBytes"] = static_cast<uint64_t>(status.latestFrameBytes);
            crow::response res{payload};
            res.set_header("Cache-Control", "no-store");
            return res;
        });

        CROW_ROUTE(app, "/api/frame.jpg")([&state]() {
            auto frame = state.getFrameJpegCopy();
            if (frame.empty()) {
                return crow::response(503, "Frame not ready");
            }

            crow::response res;
            res.code = 200;
            res.set_header("Content-Type", "image/jpeg");
            res.set_header("Cache-Control", "no-store");
            res.body.assign(reinterpret_cast<const char*>(frame.data()), frame.size());
            return res;
        });
    }
};
