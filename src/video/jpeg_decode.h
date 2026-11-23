#pragma once

#include "video/video_frame.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace p2p::video {

// Camera MJPEG -> NV12 in the JPEG's own colours (full-range BT.601,
// Colour::Bt601Full): no colour conversion on the send path; the encoder
// signals it and the receiver's renderer converts. Linux only for now.
class JpegDecoder {
public:
    virtual ~JpegDecoder() = default;
    // |out| comes with width / height set (the camera size); the decoder
    // either fills its CPU planes or attaches a GPU image.
    virtual bool decode(const uint8_t* jpeg, size_t len, VideoFrame& out) = 0;
    virtual QString name() const = 0;
};

// libavcodec on the CPU, repacked to NV12 with swscale.
std::unique_ptr<JpegDecoder> createSoftwareJpegDecoder();

// VA-API JPEG decode on the process's shared VA device; frames stay on the
// GPU (VideoFrame::gpu): the encoder takes an NV12 surface repacked and
// scaled by VA video processing, CPU readers download on demand. nullptr,
// with |why| set, if the GPU can't do it.
std::unique_ptr<JpegDecoder> createVaapiJpegDecoder(QString* why);

} // namespace p2p::video
