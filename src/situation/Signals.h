#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// 狀況規則可以引用的訊號 (見 README「訊號」)。
// 每個訊號以 optional<double> 表示：nullopt = 未知；布林訊號以 0 / 1 表示。
enum class Signal : int {
    PersonPresent,
    PersonCount,
    PersonConf,
    FaceVisible,
    BodyVisible,
    ProneLikely,
    KeypointMotion,
    NearBedEdge,
    Activity,
};

constexpr int kSignalCount = 9;

struct SignalInfo {
    Signal id;
    std::string_view name;  // 規則檔中使用的名稱
    bool isBool;
};

constexpr std::array<SignalInfo, kSignalCount> kSignals{{
    {Signal::PersonPresent, "person_present", true},
    {Signal::PersonCount, "person_count", false},
    {Signal::PersonConf, "person_conf", false},
    {Signal::FaceVisible, "face_visible", true},
    {Signal::BodyVisible, "body_visible", true},
    {Signal::ProneLikely, "prone_likely", true},
    {Signal::KeypointMotion, "keypoint_motion", false},
    {Signal::NearBedEdge, "near_bed_edge", true},
    {Signal::Activity, "activity", false},
}};

inline const SignalInfo& signalInfo(Signal s) { return kSignals[static_cast<int>(s)]; }

inline std::optional<Signal> signalByName(std::string_view name) {
    for (const auto& info : kSignals) {
        if (info.name == name) return info.id;
    }
    return std::nullopt;
}

// 某一張分析影格當下的訊號快照
struct SignalFrame {
    uint64_t seq = 0;
    int64_t tCaptureUs = 0;  // 來源影格的擷取時間 (monotonic 微秒)
    std::array<std::optional<double>, kSignalCount> values{};

    std::optional<double>& operator[](Signal s) { return values[static_cast<int>(s)]; }
    const std::optional<double>& operator[](Signal s) const { return values[static_cast<int>(s)]; }
};
