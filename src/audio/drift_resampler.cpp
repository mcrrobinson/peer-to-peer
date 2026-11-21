#include "audio/drift_resampler.h"

#include <algorithm>
#include <cmath>

namespace p2p::audio {

DriftResampler::DriftResampler()
{
    buf_.reserve(48000);
    buf_.push_back(0.0f); // history sample for the first interpolation
}

void DriftResampler::push(const int16_t* pcm, int n)
{
    buf_.insert(buf_.end(), pcm, pcm + n);
}

size_t DriftResampler::required(int n, double ratio) const
{
    if (n <= 0) return 0;
    const double last = pos_ + double(n - 1) * ratio;
    return size_t(std::floor(last)) + 3; // needs buf_[idx + 2]
}

void DriftResampler::pull(int16_t* out, int n, double ratio)
{
    for (int i = 0; i < n; ++i) {
        const size_t idx = size_t(pos_);
        const float t = float(pos_ - double(idx));
        const float x0 = buf_[idx - 1], x1 = buf_[idx], x2 = buf_[idx + 1], x3 = buf_[idx + 2];
        const float y = x1 + 0.5f * t * (x2 - x0 + t * (2.0f * x0 - 5.0f * x1 + 4.0f * x2 - x3 +
                                                        t * (3.0f * (x1 - x2) + x3 - x0)));
        out[i] = int16_t(std::clamp(std::lround(y), -32768L, 32767L));
        pos_ += ratio;
    }
    // Drop consumed input, keeping one history sample before pos_.
    const size_t drop = std::min(size_t(pos_) - 1, buf_.size());
    buf_.erase(buf_.begin(), buf_.begin() + long(drop));
    pos_ -= double(drop);
}

} // namespace p2p::audio
