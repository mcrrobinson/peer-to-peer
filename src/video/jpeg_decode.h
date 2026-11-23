#pragma once

#include "video/video_frame.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace p2p::video {

// Camera MJPEG -> NV12 (limited-range BT.709, what the encoder expects).
// Webcam JPEGs are full-range BT.601, usually 4:2:2. Linux only for now.
class JpegDecoder {
public:
    virtual ~JpegDecoder() = default;
    // |out| must already be allocated at the camera size.
    virtual bool decode(const uint8_t* jpeg, size_t len, VideoFrame& out) = 0;
    virtual QString name() const = 0;
};

// libavcodec + swscale on the CPU (~13 ms CPU per 1080p frame here).
std::unique_ptr<JpegDecoder> createSoftwareJpegDecoder();

// VA-API JPEG decode, VA video-processing repack to NV12, download, then the
// range/matrix conversion on the CPU (Mesa's VPP ignores colour properties
// for YUV -> YUV). nullptr, with |why| set, if the GPU can't do it.
std::unique_ptr<JpegDecoder> createVaapiJpegDecoder(QString* why);

} // namespace p2p::video
