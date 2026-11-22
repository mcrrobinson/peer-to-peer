#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace p2p::audio {

// Picks the Opus bitrate from the congestion controller's target rate.
//
// Audio comes first: it may use whatever the target leaves after the
// minimum rates of the video streams that are sending, less its own
// per-packet overhead (headers cost ~58 kbps at 100 packets/s, more than
// the speech itself). The bitrate moves between minBps and maxBps in fixed
// steps: down at once, up one step after the allowance has stayed higher
// for upHoldUs, so a noisy target doesn't make the encoder flap.
//
// With video at its floor rung this is what keeps audio from overflowing a
// slow link; on any normal link the allowance is far above maxBps and the
// bitrate stays at the maximum. Not thread-safe.
class AudioRateController {
public:
    struct Config {
        int minBps = 12'000;
        int maxBps = 48'000;
        int64_t overheadBps = 0;        // per-packet headers on the wire, in bps
        int64_t upHoldUs = 2'000'000;
    };

    explicit AudioRateController(Config cfg) : cfg_(cfg)
    {
        cfg_.minBps = std::min(cfg_.minBps, cfg_.maxBps);
        current_ = cfg_.maxBps;
    }

    int bitrate() const { return current_; }

    // |targetBps|: congestion controller target (all streams, with headers).
    // |reservedBps|: what the sending video streams need at minimum.
    // Returns true when bitrate() changed.
    bool update(int64_t targetBps, int64_t reservedBps, int64_t nowUs)
    {
        const int64_t allowance = targetBps - reservedBps - cfg_.overheadBps;
        const int desired = stepAtMost(allowance);
        if (desired < current_) {
            current_ = desired;
            upSinceUs_ = -1;
            return true;
        }
        if (desired == current_) {
            upSinceUs_ = -1;
            return false;
        }
        if (upSinceUs_ < 0) upSinceUs_ = nowUs;
        if (nowUs - upSinceUs_ < cfg_.upHoldUs) return false;
        current_ = stepAbove(current_);
        upSinceUs_ = nowUs; // the next step up waits again
        return true;
    }

private:
    static constexpr std::array<int, 10> kSteps{12'000, 16'000, 20'000, 24'000, 32'000,
                                                40'000, 48'000, 64'000, 96'000, 128'000};

    // Largest step (or maxBps) within |bps|, never below minBps.
    int stepAtMost(int64_t bps) const
    {
        if (bps >= cfg_.maxBps) return cfg_.maxBps;
        int best = cfg_.minBps;
        for (int s : kSteps)
            if (s >= cfg_.minBps && s < cfg_.maxBps && s <= bps) best = s;
        return best;
    }

    int stepAbove(int bps) const
    {
        for (int s : kSteps)
            if (s > bps && s < cfg_.maxBps) return s;
        return cfg_.maxBps;
    }

    Config cfg_;
    int current_ = 0;
    int64_t upSinceUs_ = -1;
};

} // namespace p2p::audio
