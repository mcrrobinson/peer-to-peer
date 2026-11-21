#pragma once

#include "media/codec_caps.h"
#include "video/video_frame.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace p2p::video {

// Low-latency encoder over FFmpeg:
//   hardware: hevc_vaapi / av1_vaapi (VideoToolbox on macOS later)
//   software: libx265 (ultrafast, zerolatency) / libsvtav1 (RTC preset, low-delay)
// Settings: P-frames only (no B-frames, no lookahead), CBR with a ~1 frame
// VBV, effectively infinite GOP with IDR on demand (picture loss).
class VideoEncoder {
public:
    struct Config {
        media::Codec codec = media::Codec::HEVC;
        bool hardware = true;
        int width = 1920;
        int height = 1080;
        int fps = 60;
        int bitrateKbps = 12000;
    };
    using Output = std::function<void(const uint8_t* data, size_t size, bool keyframe)>;

    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool init(const Config& cfg, QString* error);
    // Encodes one NV12 frame; |out| is called for each produced packet.
    bool encode(const VideoFrame& frame, bool forceKeyframe, const Output& out);
    QString name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace p2p::video
