#pragma once

#include "vision/Frame.h"

#include <cstdint>
#include <string>

// /ws/live 的訊息格式 (見 README「即時影像」)
namespace live {

constexpr uint32_t kFrameMagic = 0x314D4246;  // "FBM1" (little-endian)
constexpr std::size_t kFrameHeaderSize = 20;  // u32 magic + u64 seq + u64 tCaptureUs

// binary：[u32 magic][u64 seq][u64 tCaptureUs][JPEG bytes]，little-endian
std::string encodeFrame(const RawFrame& frame);

// text：{"type":"detection","seq":N,"tCaptureUs":T,"person":bool,"box":[x,y,w,h]|null,
//        "confidence":c,"motion":bool,"width":W,"height":H,"keypoints":[[x,y,v] x 17]|null}
std::string encodeDetection(const DetectionResult& result);

} // namespace live
