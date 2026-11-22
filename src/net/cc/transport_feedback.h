#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace p2p::net::cc {

// Transport-wide congestion feedback (in the spirit of RFC 8888), sent by
// the receiver of media as its own datagram type. One report covers a
// contiguous range of the peer's packet numbers:
//
//   u64 report time (receiver clock, us) | u64 base packet number | u16 count |
//   count x u16 { bit 15 received | bits 14-13 ECN | bits 12-0 arrival time
//                 offset before the report time, 64 us units, saturating }
//
// Consecutive reports overlap, so losing one report loses no information;
// the sender treats repeats idempotently. "Not received" entries are only
// informative (the packet may still arrive late); the sender declares loss
// itself from what has been acknowledged.
struct FeedbackEntry {
    uint64_t pn = 0;
    bool received = false;
    uint8_t ecn = 0;          // IP ECN codepoint (0 not-ECT, 1 ECT(1), 2 ECT(0), 3 CE)
    int64_t arrivalUs = 0;    // receiver clock; only meaningful when received
};

struct TransportFeedback {
    static constexpr size_t kHeaderSize = 18;
    static constexpr int64_t kAtoUnitUs = 64;
    static constexpr uint16_t kAtoMax = 0x1fff;
    int64_t reportUs = 0;
    uint64_t basePn = 0;
    std::vector<FeedbackEntry> entries; // entries[i].pn == basePn + i

    // Arrival offsets are quantised to 64 us; saturated ones read back as
    // (report time - kAtoMax units).
    size_t serialize(uint8_t* out, size_t cap) const;
    static std::optional<TransportFeedback> parse(const uint8_t* p, size_t len);
    static size_t maxEntries(size_t payload) { return payload > kHeaderSize ? (payload - kHeaderSize) / 2 : 0; }
};

// Receiver side: remembers the recent arrivals and builds reports. Not
// thread-safe (runs on the transport's receive thread).
class FeedbackBuilder {
public:
    static constexpr size_t kRing = 4096;
    static constexpr uint64_t kOverlap = 32; // packets repeated from the previous report

    void onPacket(uint64_t pn, int64_t arrivalUs, uint8_t ecn);
    bool hasNew() const { return newCount_ > 0; }
    size_t newCount() const { return newCount_; }
    // Builds a report covering the new arrivals (plus overlap), at most
    // |maxEntries| packets, the newest ones.
    std::optional<TransportFeedback> build(int64_t nowUs, size_t maxEntries);

private:
    struct Slot {
        uint64_t pn = 0;
        int64_t arrivalUs = 0;
        uint8_t ecn = 0;
    };
    Slot ring_[kRing];
    uint64_t first_ = 0;   // first packet number ever seen
    uint64_t highest_ = 0;
    uint64_t lowestNew_ = 0; // lowest packet number received since the last report (0 = none)
    uint64_t lastReportedHigh_ = 0;
    size_t newCount_ = 0;
};

} // namespace p2p::net::cc
