#pragma once

#include "video/video_frame.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace p2p::video {

// HEVC encode straight on VideoToolbox (macOS), in place of FFmpeg's
// hevc_videotoolbox, for what FFmpeg doesn't expose: low-latency rate
// control (follows a new target without a keyframe; FFmpeg's default rate
// control ignores downward changes) and long-term references for loss
// recovery without a keyframe. P frames only, one frame in flight, Annex B
// out with VPS/SPS/PPS on keyframes.
//
// References: VideoToolbox picks its own long-term reference (LTR) frames
// (about every 2 s, more often around losses; refresh frames are LTRs too).
// After a loss the next frame is an LTR refresh -- a P frame from an LTR
// the receiver decoded, all of which are acknowledged (recoverFrom) --
// otherwise a keyframe. Measured on an M3: decodes bit-identical to a full
// decode, ~1.15-1.3x an ordinary frame.
//
// Low-latency rate control drops whole frames when content won't fit the
// target; a dropped frame produces no output (and no frame id is used).
class HevcVtEncoder {
public:
    struct Config {
        int width = 1920, height = 1080, fps = 60;
        int bitrateKbps = 12000;
        Colour colour = Colour::Bt709Limited;
    };
    using Output = std::function<void(const uint8_t* data, size_t size, bool keyframe)>;

    HevcVtEncoder();
    ~HevcVtEncoder();
    HevcVtEncoder(const HevcVtEncoder&) = delete;
    HevcVtEncoder& operator=(const HevcVtEncoder&) = delete;

    // False (|why| set) without a hardware HEVC encoder with low-latency
    // rate control.
    bool init(const Config& cfg, QString* why);
    // Encodes frame |f| as frame |id| (the
    // caller's numbering, consecutive over frames that produced output);
    // |out| gets the access unit, on this thread, before this returns.
    // True with no output if the encoder dropped the frame. |f| is a GPU
    // frame with a CVPixelBuffer (taken as is, or scaled on the GPU to the
    // configured size) or CPU NV12 at the configured size.
    bool encode(const VideoFrame& f, uint32_t id, bool forceKeyframe, const Output& out);
    // Frames back from the last encoded frame to its reference (0: keyframe).
    int lastRefBack() const;
    // The receiver has decoded frame |goodId| and lost what came after: the
    // next frame refreshes from an LTR at or before |goodId| (lastRefBack()
    // then counts back to the newest of them). False if there is none since
    // the last keyframe (send a keyframe).
    bool recoverFrom(uint32_t goodId);
    // New target from the next frame on, without a keyframe (the output
    // follows within about a second).
    void setBitrateKbps(int kbps);
    // Frames the rate control dropped so far.
    uint64_t droppedFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace p2p::video
