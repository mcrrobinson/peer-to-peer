#pragma once

#include "media/codec_caps.h"
#include "video/video_frame.h"

#include <QString>

#include <memory>

namespace p2p::video {

// Low-delay decoder over FFmpeg: VA-API hwaccel for HEVC/AV1 when available
// (frames are read back to NV12 for now; zero-copy import is phase 9),
// otherwise the software hevc / libdav1d decoders with slice threading
// only (frame threading would add a frame of delay per thread).
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool init(media::Codec codec, bool preferHardware, QString* error);
    // Returns a decoded NV12 frame, nullptr if none was produced or on error
    // (|error| set => the caller should request a keyframe).
    // |displayW|/|displayH| (0 = use the decoded size) crop away encoder
    // padding that the bitstream doesn't describe.
    std::shared_ptr<VideoFrame> decode(const uint8_t* data, size_t size, bool* error, int displayW = 0,
                                       int displayH = 0);
    bool hardware() const;
    QString name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace p2p::video
