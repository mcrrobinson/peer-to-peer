#pragma once

#include "net/cc/transport_feedback.h"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace p2p::net::cc {

// Sender-side congestion controller in the spirit of SCReAM v2, rate based:
//
//  * Queue delay: one-way delay (receiver arrival - our send time, so the
//    clock offset cancels) minus a 30 s windowed minimum. The control signal
//    is the minimum over the last 50 ms: the standing queue, not the burst
//    of one frame's packets.
//  * L4S / ECN: the fraction of CE-marked packets feeds an EWMA alpha; on
//    marks the rate drops by alpha/2 at most once per RTT (Prague style).
//  * Delay: above the target the rate drops in proportion to the excess;
//    with a large queue, to below the measured delivery rate so the queue
//    drains in ~400 ms. No further delay cut until that queue had time to
//    drain (the signal lags the cut).
//  * Loss: congestion only when it comes with queueing, or above 10%;
//    random loss alone is left to NACK / FEC.
//  * Increase: fast (+50%/s) away from the last congestion point, slower
//    (+12%/s) within 15% of it; held while the queue is above half the target and
//    while the sender isn't using what it has (app limited).
//
// Pacing is requested only within 10 s of a congestion signal (the link is
// the bottleneck); otherwise packets go out as bursts.
//
// Not thread-safe; times are on the sender's monotonic clock except the
// arrival times inside the feedback.
class CongestionController {
public:
    struct Config {
        int64_t minBps = 300'000;
        int64_t maxBps = 12'500'000;
        int64_t startBps = 0;               // 0 = maxBps (LAN: start high)
        int64_t queueDelayTargetUs = 10'000;
    };
    struct Stats {
        int64_t targetBps = 0, pacingBps = 0, deliveredBps = 0;
        int64_t queueDelayUs = 0, srttUs = 0;
        double lossRate = 0;      // smoothed (fast attack, slow decay), for FEC
        double ceAlpha = 0;
        uint64_t acked = 0, lost = 0, spuriousLost = 0, ceMarked = 0, reports = 0;
        uint64_t delayEvents = 0, lossEvents = 0, ceEvents = 0;
    };
    static constexpr int kStreams = 4;

    CongestionController() : CongestionController(Config{}) {}
    explicit CongestionController(Config cfg);

    void onPacketSent(uint64_t pn, int64_t nowUs, size_t bytes, uint8_t stream);
    void onFeedback(const TransportFeedback& fb, int64_t nowUs);

    int64_t targetBps() const { return target_; }
    int64_t pacingBps(int64_t nowUs) const;   // 0 = send bursts
    int64_t streamBps(uint8_t stream) const;  // recent send rate of one stream (incl. headers)
    Stats stats(int64_t nowUs) const;

private:
    struct Sent {
        uint64_t pn = 0;
        int64_t sendUs = 0;
        uint32_t bytes = 0;
        bool acked = false, lost = false;
    };
    struct RateWindow {
        int64_t startUs = 0;
        uint64_t bytes = 0;
        int64_t bps = 0;
    };
    static constexpr size_t kLog = 16384;
    static constexpr uint64_t kReorderPackets = 3;

    Sent* lookup(uint64_t pn);
    void updateBase(int64_t nowUs, int64_t owd);
    void control(int64_t nowUs, uint64_t newLost, uint64_t newCe);
    void congestion(int64_t nowUs, int64_t before);

    Config cfg_;
    double target_ = 0;
    std::vector<Sent> log_;
    uint64_t highestSent_ = 0, highestAcked_ = 0;

    std::deque<std::pair<int64_t, int64_t>> baseBuckets_; // (bucket start, min owd)
    int64_t baseOwd_ = 0;
    bool haveBase_ = false;
    std::deque<std::pair<int64_t, int64_t>> qSamples_;    // (time, min queue delay of one report)
    int64_t qdelay_ = 0;
    int64_t srtt_ = 0;

    std::deque<std::pair<int64_t, int64_t>> delivered_;   // (arrival us, bytes)
    std::deque<int64_t> deliveredAt_;                     // when each was acknowledged
    int64_t deliveredBps_ = 0;

    int64_t lossWindowStart_ = 0;
    uint64_t windowAcked_ = 0, windowLost_ = 0;
    double windowLoss_ = 0, lossRate_ = 0;
    double ceAlpha_ = 0.25;

    int64_t lastUpdateUs_ = 0, lastReduceUs_ = 0, reduceHoldUs_ = 0;
    int64_t lastCongestionUs_ = 0;
    double lastCongestionBps_ = 0;

    std::array<RateWindow, kStreams> streamRate_{};
    Stats st_;
};

} // namespace p2p::net::cc
