#pragma once

#include "video/video_frame.h"

#include <memory>

namespace p2p::video {

// macOS: an NV12 CVPixelBufferRef (420v / 420f, IOSurface-backed) as a
// GpuImage. Retains the buffer while the image lives. Shown without a copy
// (VideoView's Metal import), taken by the VideoToolbox encoder as is (scaled
// there when sizes differ); CPU readers get a copy (download).
std::shared_ptr<GpuImage> wrapCvPixelBuffer(void* pixelBuffer);

// Copies |pixelBuffer|'s two planes into |f|'s CPU planes (f.width x
// f.height, which must not exceed the buffer). False if it isn't NV12.
bool copyCvPixelBuffer(void* pixelBuffer, VideoFrame& f);

} // namespace p2p::video
