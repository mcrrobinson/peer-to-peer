#include "net/cc/congestion_controller.h"

#include <algorithm>
#include <limits>

namespace p2p::net::cc {

namespace {
constexpr int64_t kBaseBucketUs = 1'000'000;
constexpr size_t kBaseBuckets = 30;
constexpr int64_t kQueueWindowUs = 50'000;
constexpr int64_t kDeliveredWindowUs = 300'000;
constexpr int64_t kDeliveredMinSpanUs = 100'000;
constexpr int64_t kLossWindowUs = 250'000;
constexpr int64_t kRateWindowUs = 500'000;
constexpr int64_t kPacingHoldUs = 10'000'000;
constexpr int64_t kNearCongestionUs = 20'000'000;
constexpr double kCeGain = 1.0 / 16.0;
constexpr int kEcnCe = 3;
} // namespace

CongestionController::CongestionController(Config cfg) : cfg_(cfg), log_(kLog)
{
    cfg_.minBps = std::max<int64_t>(cfg_.minBps, 50'000);
    cfg_.maxBps = std::max(cfg_.maxBps, cfg_.minBps);
    target_ = double(std::clamp(cfg_.startBps > 0 ? cfg_.startBps : cfg_.maxBps, cfg_.minBps, cfg_.maxBps));
}

CongestionController::Sent* CongestionController::lookup(uint64_t pn)
{
    auto& s = log_[pn % kLog];
    return s.pn == pn ? &s : nullptr;
}

void CongestionController::onPacketSent(uint64_t pn, int64_t nowUs, size_t bytes, uint8_t stream)
{
    auto& s = log_[pn % kLog];
    s = {pn, nowUs, uint32_t(bytes), false, false};
    highestSent_ = std::max(highestSent_, pn);

    if (stream < kStreams) {
        auto& r = streamRate_[stream];
        if (r.startUs == 0) r.startUs = nowUs;
        r.bytes += bytes;
        const int64_t dt = nowUs - r.startUs;
        if (dt >= kRateWindowUs) {
            r.bps = int64_t(double(r.bytes) * 8e6 / double(dt));
            r.bytes = 0;
            r.startUs = nowUs;
        }
    }
}

int64_t CongestionController::streamBps(uint8_t stream) const
{
    return stream < kStreams ? streamRate_[stream].bps : 0;
}

void CongestionController::updateBase(int64_t nowUs, int64_t owd)
{
    if (baseBuckets_.empty() || nowUs - baseBuckets_.back().first >= kBaseBucketUs) {
        baseBuckets_.emplace_back(nowUs, owd);
        while (baseBuckets_.size() > kBaseBuckets) baseBuckets_.pop_front();
    } else {
        baseBuckets_.back().second = std::min(baseBuckets_.back().second, owd);
    }
    baseOwd_ = std::numeric_limits<int64_t>::max();
    for (const auto& b : baseBuckets_) baseOwd_ = std::min(baseOwd_, b.second);
    haveBase_ = true;
}

void CongestionController::onFeedback(const TransportFeedback& fb, int64_t nowUs)
{
    ++st_.reports;
    uint64_t newAcked = 0, newCe = 0, newLost = 0;
    int64_t reportMinOwd = std::numeric_limits<int64_t>::max();
    const Sent* newest = nullptr;
    int64_t newestArrival = 0;

    for (const auto& e : fb.entries) {
        if (!e.received) continue;
        Sent* s = lookup(e.pn);
        if (!s || s->acked) continue;
        if (s->lost) { // arrived after we gave up on it: reordering, not loss
            s->lost = false;
            ++st_.spuriousLost;
            if (st_.lost) --st_.lost;
            if (windowLost_) --windowLost_;
        }
        s->acked = true;
        ++newAcked;
        ++windowAcked_;
        if (e.ecn == kEcnCe) ++newCe;
        const int64_t owd = e.arrivalUs - s->sendUs;
        reportMinOwd = std::min(reportMinOwd, owd);
        updateBase(nowUs, owd);
        delivered_.emplace_back(e.arrivalUs, int64_t(s->bytes));
        deliveredAt_.push_back(nowUs);
        if (!newest || s->pn > newest->pn) {
            newest = s;
            newestArrival = e.arrivalUs;
        }
        highestAcked_ = std::max(highestAcked_, s->pn);
    }
    st_.acked += newAcked;
    st_.ceMarked += newCe;
    if (newAcked == 0) return;

    // RTT from the newest packet: total time minus how long the receiver held it.
    if (newest) {
        const int64_t rtt = std::max<int64_t>(100, nowUs - newest->sendUs - (fb.reportUs - newestArrival));
        srtt_ = srtt_ == 0 ? rtt : (7 * srtt_ + rtt) / 8;
    }

    // Loss: packets a report says are missing while newer ones made it.
    // Packets no report covered (the report itself was lost) stay unknown,
    // so reverse-path loss doesn't look like forward congestion.
    for (const auto& e : fb.entries) {
        if (e.received || e.pn + kReorderPackets >= highestAcked_) continue;
        Sent* s = lookup(e.pn);
        if (s && !s->acked && !s->lost) {
            s->lost = true;
            ++newLost;
        }
    }
    st_.lost += newLost;
    windowLost_ += newLost;

    // Queue delay: minimum of the per-report minima over the last 50 ms.
    if (haveBase_ && reportMinOwd != std::numeric_limits<int64_t>::max()) {
        qSamples_.emplace_back(nowUs, std::max<int64_t>(0, reportMinOwd - baseOwd_));
    }
    while (!qSamples_.empty() && nowUs - qSamples_.front().first > kQueueWindowUs) qSamples_.pop_front();
    if (!qSamples_.empty()) {
        qdelay_ = std::numeric_limits<int64_t>::max();
        for (const auto& q : qSamples_) qdelay_ = std::min(qdelay_, q.second);
    }

    // Delivery rate over the arrival-time span of the last 300 ms of acks.
    while (!deliveredAt_.empty() && nowUs - deliveredAt_.front() > kDeliveredWindowUs) {
        deliveredAt_.pop_front();
        delivered_.pop_front();
    }
    if (delivered_.size() >= 2) {
        int64_t lo = delivered_.front().first, hi = lo, bytes = 0;
        for (size_t i = 0; i < delivered_.size(); ++i) {
            lo = std::min(lo, delivered_[i].first);
            hi = std::max(hi, delivered_[i].first);
            if (i) bytes += delivered_[i].second;
        }
        if (hi - lo >= kDeliveredMinSpanUs) deliveredBps_ = int64_t(double(bytes) * 8e6 / double(hi - lo));
    }

    // Loss rate per 250 ms window; the smoothed value attacks fast, decays slowly.
    if (lossWindowStart_ == 0) lossWindowStart_ = nowUs;
    if (nowUs - lossWindowStart_ >= kLossWindowUs) {
        const uint64_t total = windowAcked_ + windowLost_;
        windowLoss_ = total ? double(windowLost_) / double(total) : 0.0;
        lossRate_ = windowLoss_ > lossRate_ ? windowLoss_ : 0.8 * lossRate_ + 0.2 * windowLoss_;
        windowAcked_ = windowLost_ = 0;
        lossWindowStart_ = nowUs;
    }

    ceAlpha_ = (1.0 - kCeGain) * ceAlpha_ + kCeGain * double(newCe) / double(newAcked);
    control(nowUs, newLost, newCe);
}

void CongestionController::congestion(int64_t nowUs, int64_t before)
{
    lastCongestionUs_ = nowUs;
    lastReduceUs_ = nowUs;
    lastCongestionBps_ = double(before);
}

void CongestionController::control(int64_t nowUs, uint64_t newLost, uint64_t newCe)
{
    const int64_t dt = lastUpdateUs_ ? std::clamp<int64_t>(nowUs - lastUpdateUs_, 0, 100'000) : 0;
    lastUpdateUs_ = nowUs;
    const int64_t tgt = cfg_.queueDelayTargetUs;
    const int64_t q = qdelay_;
    // One reaction per RTT plus the time the queue signal needs to show it.
    const bool canReduce = nowUs - lastReduceUs_ >= srtt_ + kQueueWindowUs + reduceHoldUs_;
    const int64_t before = int64_t(target_);

    if (newCe > 0 && canReduce) {
        target_ *= 1.0 - std::clamp(ceAlpha_, 0.02, 1.0) / 2.0;
        ++st_.ceEvents;
        congestion(nowUs, before);
        reduceHoldUs_ = 0;
    } else if (q > tgt && canReduce) {
        target_ *= 1.0 - std::clamp(0.5 * double(q - tgt) / double(q), 0.02, 0.3);
        if (q > 2 * tgt && deliveredBps_ > 0)
            target_ = std::min(target_, double(deliveredBps_) * std::max(0.5, 1.0 - double(q) / 400'000.0));
        ++st_.delayEvents;
        congestion(nowUs, before);
        reduceHoldUs_ = std::min<int64_t>(q, 200'000);
    } else if (newLost > 0 && canReduce && (q > tgt / 2 || windowLoss_ > 0.1)) {
        target_ *= windowLoss_ > 0.1 ? 1.0 - 0.5 * std::min(windowLoss_, 0.5) : 0.85;
        ++st_.lossEvents;
        congestion(nowUs, before);
        reduceHoldUs_ = 0;
    } else if (q < tgt / 2) {
        const bool appLimited = deliveredBps_ > 0 && target_ > 1.6 * double(deliveredBps_);
        if (!appLimited) {
            const bool near = lastCongestionUs_ != 0 && nowUs - lastCongestionUs_ < kNearCongestionUs &&
                              target_ > 0.85 * lastCongestionBps_ && target_ < 1.15 * lastCongestionBps_;
            const double perSecond = near ? 0.12 : 0.5;
            target_ *= 1.0 + perSecond * double(dt) / 1e6;
        }
    }
    target_ = std::clamp(target_, double(cfg_.minBps), double(cfg_.maxBps));
}

int64_t CongestionController::pacingBps(int64_t nowUs) const
{
    if (lastCongestionUs_ == 0 || nowUs - lastCongestionUs_ > kPacingHoldUs) return 0;
    return int64_t(target_ * 1.5);
}

CongestionController::Stats CongestionController::stats(int64_t nowUs) const
{
    Stats s = st_;
    s.targetBps = int64_t(target_);
    s.pacingBps = pacingBps(nowUs);
    s.deliveredBps = deliveredBps_;
    s.queueDelayUs = qdelay_;
    s.srttUs = srtt_;
    s.lossRate = lossRate_;
    s.ceAlpha = ceAlpha_;
    return s;
}

} // namespace p2p::net::cc
