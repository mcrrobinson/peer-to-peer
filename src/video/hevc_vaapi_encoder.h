#pragma once

#include "video/hevc_headers.h"
#include "video/video_frame.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace p2p::video {

// HEVC encode straight on VA-API (Linux), with our own parameter sets and
// slice headers (hevc_headers.h). Same stream as FFmpeg's hevc_vaapi in our
// settings (CBR, ~1 frame VBV, P only, IDR on demand, one frame in flight),
// but the application owns what FFmpeg hides: the reference structure and
// rate control between keyframes.
//
// References: each frame predicts from the previous one. Besides it the
// DPB keeps up to three "anchors", earlier frames of the same chain spaced
// setAnchorInterval() apart, so that after a loss the next frame can
// predict from a frame the receiver still has (recoverFrom) instead of
// being a keyframe.
class HevcVaapiEncoder {
public:
    struct Config {
        int width = 1920, height = 1080, fps = 60;
        int bitrateKbps = 12000;
        Colour colour = Colour::Bt709Limited;
    };
    using Output = std::function<void(const uint8_t* data, size_t size, bool keyframe)>;

    HevcVaapiEncoder();
    ~HevcVaapiEncoder();
    HevcVaapiEncoder(const HevcVaapiEncoder&) = delete;
    HevcVaapiEncoder& operator=(const HevcVaapiEncoder&) = delete;

    // |display|: the VADisplay the input surfaces live on.
    bool init(void* display, const Config& cfg, QString* why);
    // Encodes NV12 surface |surface| (the configured size) as frame |id|
    // (the caller's frame numbering, consecutive); |out| gets the access
    // unit. The surface may be reused once this returns.
    bool encode(unsigned surface, uint32_t id, bool forceKeyframe, const Output& out);
    // Frames back from the last encoded frame to its reference (0: keyframe).
    int lastRefBack() const;
    // The receiver has decoded frame |goodId| and lost what came after: the
    // next frame predicts from it, or from the newest frame before it in
    // its chain that is still kept. False if none is (send a keyframe).
    bool recoverFrom(uint32_t goodId);
    // Spacing of the anchors, in frames (>= 1).
    void setAnchorInterval(int frames);
    // New CBR target from the next frame on, without a keyframe.
    void setBitrateKbps(int kbps);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace p2p::video
