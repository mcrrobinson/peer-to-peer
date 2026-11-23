#pragma once

#include "media/codec_caps.h"
#include "video/video_frame.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace p2p::video {

// Low-latency encoder over FFmpeg:
//   hardware: hevc_vaapi / av1_vaapi (Linux), hevc_videotoolbox (macOS)
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
        Colour colour = Colour::Bt709Limited; // signalled in the bitstream (VUI)
    };
    using Output = std::function<void(const uint8_t* data, size_t size, bool keyframe)>;

    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool init(const Config& cfg, QString* error);
    // Encodes one NV12 frame; |out| is called for each produced packet. A
    // GPU frame (VideoFrame::gpu) goes to a hardware encoder without a copy,
    // scaled to the encoder's size on the GPU; otherwise the frame must be
    // the encoder's size and have CPU planes.
    // |frameId|: the caller's number for this frame (consecutive), for
    // reference control; by default the previous one + 1.
    bool encode(const VideoFrame& frame, bool forceKeyframe, const Output& out,
                std::optional<uint32_t> frameId = std::nullopt);
    QString name() const;
    bool hardware() const;
    // Whether encode() takes any GPU frame as is (scaling it on the GPU):
    // hardware encoders on Linux (VA-API surfaces), the native VideoToolbox
    // one on macOS. Otherwise GPU frames need CPU planes at the right size.
    bool takesGpuFrames() const;
    // Whether setBitrateKbps() works without re-opening (native HEVC only;
    // FFmpeg's encoders apply rate settings only when opened).
    bool liveBitrate() const;
    void setBitrateKbps(int kbps);
    // During |out|: frames back to the frame this one predicts from (0 for
    // a keyframe). FFmpeg's encoders: always the previous one.
    int refBack() const;
    // Loss recovery without a keyframe (native HEVC): the next frame
    // predicts from |goodFrameId|, which the receiver decoded, or from an
    // earlier frame of its chain. False if not possible: send a keyframe.
    bool recoverFrom(uint32_t goodFrameId);
    // How far apart the frames kept for recovery are (native HEVC).
    void setAnchorInterval(int frames);

    // Hardware HEVC: our own encoder -- VA-API on Linux (hevc_vaapi_encoder.h),
    // VideoToolbox on macOS (hevc_vt_encoder.h) -- instead of FFmpeg's
    // (--encoder native|ffmpeg). Falls back to FFmpeg if the driver can't.
    // Default on; set before opening encoders.
    static void setNativeHevc(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace p2p::video
