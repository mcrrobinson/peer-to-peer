#pragma once

#include <cstdint>
#include <vector>

namespace p2p::video {

// Encoder operating points for congestion control. FFmpeg's hevc_vaapi and
// libx265 only apply rate-control settings when the encoder is opened, so
// every bitrate change re-opens the encoder and costs a keyframe; the ladder
// therefore has discrete rungs (each ~0.7x the one above) and the selector
// switches with hysteresis. Resolution steps down when the bits per pixel
// would get too thin.
struct Rung {
    int kbps = 0;
    int width = 0, height = 0;
};

// Rungs from |maxKbps| down to |minKbps|, best first. Sizes never exceed
// the source and stay even (NV12).
std::vector<Rung> buildLadder(int maxKbps, int minKbps, int width, int height, int fps);

class RungSelector {
public:
    explicit RungSelector(std::vector<Rung> ladder) : ladder_(std::move(ladder)) {}

    // |budgetKbps|: what congestion control leaves for the encoder. Returns
    // true when the rung changed (re-open the encoder).
    //  * down: right away when the budget is < 80% of the current rung,
    //    otherwise once it has been short for 300 ms;
    //  * up: one rung, once the budget covers it with 10% headroom for 2 s,
    //    and at least 2 s after the last switch.
    bool update(double budgetKbps, int64_t nowUs);

    int index() const { return cur_; }
    const Rung& rung() const { return ladder_[size_t(cur_)]; }
    const std::vector<Rung>& ladder() const { return ladder_; }
    uint64_t switches() const { return switches_; }

private:
    int fits(double kbps) const; // best rung with rate <= kbps (or the last)

    std::vector<Rung> ladder_;
    int cur_ = 0;
    int64_t lastSwitchUs_ = 0;
    int64_t shortSinceUs_ = 0, roomSinceUs_ = 0;
    uint64_t switches_ = 0;
};

} // namespace p2p::video
