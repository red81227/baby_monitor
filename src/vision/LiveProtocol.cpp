#include "vision/LiveProtocol.h"

#include <algorithm>
#include <cstdio>

namespace live {

namespace {

template <class... Args>
void appendf(std::string& out, const char* fmt, Args... args) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), fmt, args...);
    if (n > 0) out.append(buf, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof(buf) - 1));
}

void putLe(std::string& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

} // namespace

std::string encodeFrame(const RawFrame& frame) {
    std::string out;
    out.reserve(kFrameHeaderSize + frame.jpeg.size());
    putLe(out, kFrameMagic, 4);
    putLe(out, frame.seq, 8);
    putLe(out, static_cast<uint64_t>(toMicros(frame.tCapture)), 8);
    out.append(reinterpret_cast<const char*>(frame.jpeg.data()), frame.jpeg.size());
    return out;
}

std::string encodeDetection(const DetectionResult& r) {
    std::string out;
    out.reserve(r.person && r.person->keypoints ? 640 : 256);
    out += "{\"type\":\"detection\",\"seq\":" + std::to_string(r.seq);
    out += ",\"tCaptureUs\":" + std::to_string(toMicros(r.tCapture));
    out += ",\"person\":";
    out += r.person ? "true" : "false";
    out += ",\"box\":";
    if (r.person) {
        const auto& b = r.person->box;
        appendf(out, "[%.1f,%.1f,%.1f,%.1f]", b.x, b.y, b.width, b.height);
    } else {
        out += "null";
    }
    appendf(out, ",\"confidence\":%.3f", r.person ? r.person->confidence : 0.0f);
    out += ",\"motion\":";
    out += r.motion ? "true" : "false";
    out += ",\"width\":" + std::to_string(r.width) + ",\"height\":" + std::to_string(r.height);
    out += ",\"keypoints\":";
    if (r.person && r.person->keypoints) {
        out += '[';
        bool first = true;
        for (const auto& k : *r.person->keypoints) {
            if (!first) out += ',';
            first = false;
            appendf(out, "[%.1f,%.1f,%.2f]", k.x, k.y, k.visibility);
        }
        out += ']';
    } else {
        out += "null";
    }
    out += '}';
    return out;
}

} // namespace live
