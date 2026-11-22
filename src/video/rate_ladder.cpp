#include "video/rate_ladder.h"

#include <algorithm>

namespace p2p::video {

namespace {
constexpr double kStep = 0.7;
constexpr double kMinBitsPerPixel = 0.04; // per frame; below this, drop resolution
constexpr double kScales[] = {1.0, 0.75, 0.5, 0.375, 0.25};
constexpr int64_t kDownAfterUs = 300'000;
constexpr int64_t kUpAfterUs = 2'000'000;
constexpr int64_t kMinUpIntervalUs = 2'000'000;
constexpr int64_t kMinDownIntervalUs = 200'000;
} // namespace

std::vector<Rung> buildLadder(int maxKbps, int minKbps, int width, int height, int fps)
{
    std::vector<Rung> out;
    minKbps = std::max(1, std::min(minKbps, maxKbps));
    fps = std::max(1, fps);
    for (double kbps = maxKbps;; kbps *= kStep) {
        const int k = std::max(minKbps, int(kbps + 0.5));
        double scale = kScales[std::size(kScales) - 1];
        for (double s : kScales) {
            const double pixels = double(width) * s * double(height) * s * fps;
            if (double(k) * 1000.0 / pixels >= kMinBitsPerPixel) {
                scale = s;
                break;
            }
        }
        Rung r;
        r.kbps = k;
        r.width = std::max(16, int(width * scale) & ~1);
        r.height = std::max(16, int(height * scale) & ~1);
        out.push_back(r);
        if (k <= minKbps) break;
    }
    return out;
}

int RungSelector::fits(double kbps) const
{
    for (size_t i = 0; i < ladder_.size(); ++i)
        if (ladder_[i].kbps <= kbps) return int(i);
    return int(ladder_.size()) - 1;
}

bool RungSelector::update(double budgetKbps, int64_t nowUs)
{
    const double cur = ladder_[size_t(cur_)].kbps;
    auto switchTo = [&](int i) {
        cur_ = i;
        lastSwitchUs_ = nowUs;
        shortSinceUs_ = roomSinceUs_ = 0;
        ++switches_;
        return true;
    };

    if (budgetKbps < cur) {
        roomSinceUs_ = 0;
        if (shortSinceUs_ == 0) shortSinceUs_ = nowUs;
        const int target = fits(budgetKbps);
        if (target == cur_ || nowUs - lastSwitchUs_ < kMinDownIntervalUs) return false;
        if (budgetKbps < 0.8 * cur || nowUs - shortSinceUs_ >= kDownAfterUs) return switchTo(target);
        return false;
    }
    shortSinceUs_ = 0;
    const int up = fits(budgetKbps / 1.1);
    if (up >= cur_) {
        roomSinceUs_ = 0;
        return false;
    }
    if (roomSinceUs_ == 0) roomSinceUs_ = nowUs;
    // One rung at a time: each step up is a probe of the link.
    if (nowUs - roomSinceUs_ >= kUpAfterUs && nowUs - lastSwitchUs_ >= kMinUpIntervalUs) return switchTo(cur_ - 1);
    return false;
}

} // namespace p2p::video
