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
constexpr int64_t kProbeWindowUs = 10'000'000;  // a step up that lasts this long succeeded
constexpr int64_t kFirstBackoffUs = 10'000'000; // wait before retrying a rung that failed
constexpr int64_t kMaxBackoffUs = 80'000'000;
constexpr int64_t kSettleUs = 3'000'000;        // after a failed probe, ride out the controller's dip
constexpr double kSevere = 0.6;                 // fallback: below this share of the old rung, go lower
constexpr double kSettleFloor = 0.7;            // while settling, only drop below this share
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
    if (holdUntilUs_.empty()) {
        holdUntilUs_.assign(ladder_.size(), 0);
        backoffUs_.assign(ladder_.size(), kFirstBackoffUs);
    }
    const double cur = ladder_[size_t(cur_)].kbps;
    auto switchTo = [&](int i) {
        cur_ = i;
        lastSwitchUs_ = nowUs;
        shortSinceUs_ = roomSinceUs_ = 0;
        ++switches_;
        return true;
    };
    // A probe that held long enough: this rung is good again.
    if (probing_ && nowUs - probeStartUs_ >= kProbeWindowUs) {
        probing_ = false;
        backoffUs_[size_t(cur_)] = kFirstBackoffUs;
    }

    if (budgetKbps < cur) {
        roomSinceUs_ = 0;
        if (shortSinceUs_ == 0) shortSinceUs_ = nowUs;
        if (nowUs - lastSwitchUs_ < kMinDownIntervalUs) return false;
        if (probing_) {
            // The step up failed: go back to the rung we came from, and
            // don't try this one again for a while (longer each time).
            probing_ = false;
            auto& backoff = backoffUs_[size_t(cur_)];
            holdUntilUs_[size_t(cur_)] = nowUs + backoff;
            backoff = std::min(2 * backoff, kMaxBackoffUs);
            ++failedProbes_;
            const int back = cur_ + 1 < int(ladder_.size()) ? cur_ + 1 : cur_;
            const int target = budgetKbps < kSevere * ladder_[size_t(back)].kbps ? fits(budgetKbps) : back;
            settleUntilUs_ = nowUs + kSettleUs;
            return target != cur_ && switchTo(target);
        }
        const int target = fits(budgetKbps);
        if (target == cur_) return false;
        // Right after a failed probe the controller's cut overshoots for a
        // moment; only a severe shortfall moves us further down then.
        if (nowUs < settleUntilUs_) return budgetKbps < kSettleFloor * cur && switchTo(target);
        if (budgetKbps < 0.8 * cur || nowUs - shortSinceUs_ >= kDownAfterUs) return switchTo(target);
        return false;
    }
    shortSinceUs_ = 0;
    const int up = fits(budgetKbps / 1.1);
    if (up >= cur_ || nowUs < holdUntilUs_[size_t(cur_ - 1)]) {
        roomSinceUs_ = 0;
        return false;
    }
    if (roomSinceUs_ == 0) roomSinceUs_ = nowUs;
    // One rung at a time: each step up is a probe of the link.
    if (nowUs - roomSinceUs_ >= kUpAfterUs && nowUs - lastSwitchUs_ >= kMinUpIntervalUs) {
        probing_ = true;
        probeStartUs_ = nowUs;
        return switchTo(cur_ - 1);
    }
    return false;
}

} // namespace p2p::video
