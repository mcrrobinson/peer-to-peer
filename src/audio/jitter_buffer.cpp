#include "audio/jitter_buffer.h"

#include <algorithm>

namespace p2p::audio {

namespace {
constexpr uint32_t kResetGapFrames = 3000; // ~30 s jump => treat as a new stream
}

JitterBuffer::JitterBuffer(Config cfg) : cfg_(cfg), baseTarget_(cfg.minTargetFrames) {}

int JitterBuffer::targetFrames() const
{
    return std::clamp(baseTarget_ + boost_, cfg_.minTargetFrames, cfg_.maxTargetFrames);
}

int JitterBuffer::levelFrames() const
{
    if (q_.empty() || maxSeq_ < playhead_) return 0;
    return int(maxSeq_ - playhead_ + 1);
}

void JitterBuffer::insert(EncodedFrame f)
{
    if (!havePlayhead_) {
        havePlayhead_ = true;
        playhead_ = f.seq;
        maxSeq_ = f.seq;
    } else if (f.seq < playhead_) {
        if (!started_ && playhead_ - f.seq < uint32_t(cfg_.maxTargetFrames)) {
            playhead_ = f.seq; // reordered start of stream
        } else {
            ++st_.late;
            return;
        }
    } else if (f.seq - playhead_ > kResetGapFrames) {
        q_.clear();
        playhead_ = f.seq;
        maxSeq_ = f.seq;
        started_ = false;
        transits_.clear();
        ++st_.resets;
    }

    if (q_.count(f.seq)) {
        ++st_.duplicates;
        return;
    }
    if (underrunPending_ && f.seq == underrunSeq_) {
        // We stalled waiting for this one: genuine lateness => more depth.
        underrunPending_ = false;
        ++st_.lateAfterWait;
        boost_ = std::min(boost_ + 1, cfg_.maxBoost);
        pullsSinceUnderrun_ = 0;
    }
    updateJitter(f);
    maxSeq_ = std::max(maxSeq_, f.seq);
    ++st_.received;
    q_.emplace(f.seq, std::move(f));
}

void JitterBuffer::updateJitter(const EncodedFrame& f)
{
    // Transit time up to an unknown constant (clock offset); only its
    // variation matters.
    const int64_t transit = f.arrivalUs - int64_t(f.timestamp) * 1'000'000 / cfg_.sampleRate;
    transits_.push_back(transit);
    while (transits_.size() > cfg_.windowPackets) transits_.pop_front();

    const int64_t minTransit = *std::min_element(transits_.begin(), transits_.end());
    std::vector<int64_t> dv;
    dv.reserve(transits_.size());
    for (int64_t t : transits_) dv.push_back(t - minTransit);
    const size_t k = (dv.size() * 95) / 100;
    std::nth_element(dv.begin(), dv.begin() + long(std::min(k, dv.size() - 1)), dv.end());
    jitterP95Us_ = dv[std::min(k, dv.size() - 1)];

    const int64_t frameUs = int64_t(cfg_.frameSamples) * 1'000'000 / cfg_.sampleRate;
    const int frames = int((jitterP95Us_ + cfg_.marginUs + frameUs - 1) / frameUs);
    baseTarget_ = std::clamp(frames, cfg_.minTargetFrames, cfg_.maxTargetFrames);
}

void JitterBuffer::discardTo(uint32_t seq)
{
    while (playhead_ < seq) {
        st_.dropped += q_.erase(playhead_);
        ++playhead_;
    }
    if (underrunPending_ && underrunSeq_ < playhead_) underrunPending_ = false;
    excessPulls_ = 0;
}

JitterBuffer::Result JitterBuffer::pull()
{
    Result r;
    if (!havePlayhead_) return r;
    if (!started_) {
        if (levelFrames() < targetFrames()) return r;
        started_ = true;
    }

    // Gross backlog (audio that queued up before playout started, or a
    // stall's worth arriving at once): playing it would put the call
    // seconds behind, so keep only the newest target's worth.
    if (levelFrames() > targetFrames() + cfg_.flushExcessFrames) {
        discardTo(maxSeq_ - uint32_t(targetFrames()) + 1);
    } else if (levelFrames() > targetFrames() + cfg_.hardTrimExcessFrames) {
        // Last-resort trim if we've been sitting far above target for a while.
        if (++excessPulls_ >= cfg_.excessPullsBeforeDrop) {
            q_.erase(playhead_);
            ++playhead_;
            ++st_.dropped;
            excessPulls_ = 0;
        }
    } else {
        excessPulls_ = 0;
    }

    if (++pullsSinceUnderrun_ >= cfg_.pullsPerBoostDecay && boost_ > 0) {
        --boost_;
        pullsSinceUnderrun_ = 0;
    }

    if (auto it = q_.find(playhead_); it != q_.end()) {
        r.action = Action::Decode;
        r.frame = std::move(it->second);
        q_.erase(it);
        ++playhead_;
        ++st_.decoded;
        return r;
    }

    if (auto next = q_.upper_bound(playhead_); next != q_.end()) {
        r.action = Action::Conceal;
        r.frame = next->second; // copy: still needed for its own playout
        r.distance = int(next->first - playhead_);
        if (underrunPending_ && underrunSeq_ == playhead_) underrunPending_ = false; // lost, not late
        ++playhead_;
        ++st_.concealed;
        return r;
    }

    // Nothing newer has arrived: hold the playhead and let the buffer grow.
    r.action = Action::Underrun;
    ++st_.underruns;
    underrunPending_ = true;
    underrunSeq_ = playhead_;
    return r;
}

} // namespace p2p::audio
