#include "net/cc/transport_feedback.h"

#include <algorithm>

namespace p2p::net::cc {

namespace {
void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void putU64(uint8_t* p, uint64_t v)
{
    for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v); v >>= 8; }
}
uint64_t getU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
} // namespace

size_t TransportFeedback::serialize(uint8_t* out, size_t cap) const
{
    const size_t n = entries.size();
    if (n > 0xffff || kHeaderSize + 2 * n > cap) return 0;
    putU64(out, uint64_t(reportUs));
    putU64(out + 8, basePn);
    putU16(out + 16, uint16_t(n));
    for (size_t i = 0; i < n; ++i) {
        const auto& e = entries[i];
        uint16_t v = 0;
        if (e.received) {
            const int64_t ato = std::clamp<int64_t>((reportUs - e.arrivalUs + kAtoUnitUs / 2) / kAtoUnitUs, 0, kAtoMax);
            v = uint16_t(0x8000 | (uint16_t(e.ecn & 3) << 13) | uint16_t(ato));
        }
        putU16(out + kHeaderSize + 2 * i, v);
    }
    return kHeaderSize + 2 * n;
}

std::optional<TransportFeedback> TransportFeedback::parse(const uint8_t* p, size_t len)
{
    if (len < kHeaderSize) return std::nullopt;
    TransportFeedback fb;
    fb.reportUs = int64_t(getU64(p));
    fb.basePn = getU64(p + 8);
    const size_t n = getU16(p + 16);
    if (len < kHeaderSize + 2 * n || fb.basePn == 0) return std::nullopt;
    fb.entries.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t v = getU16(p + kHeaderSize + 2 * i);
        auto& e = fb.entries[i];
        e.pn = fb.basePn + i;
        e.received = v & 0x8000;
        e.ecn = uint8_t((v >> 13) & 3);
        e.arrivalUs = fb.reportUs - int64_t(v & kAtoMax) * kAtoUnitUs;
    }
    return fb;
}

void FeedbackBuilder::onPacket(uint64_t pn, int64_t arrivalUs, uint8_t ecn)
{
    if (pn == 0) return;
    if (highest_ != 0 && pn + kRing <= highest_) return; // far too old to report
    auto& s = ring_[pn % kRing];
    if (s.pn == pn) return; // duplicate
    s = {pn, arrivalUs, ecn};
    first_ = first_ == 0 ? pn : std::min(first_, pn);
    highest_ = std::max(highest_, pn);
    lowestNew_ = lowestNew_ == 0 ? pn : std::min(lowestNew_, pn);
    ++newCount_;
}

std::optional<TransportFeedback> FeedbackBuilder::build(int64_t nowUs, size_t maxEntries)
{
    if (newCount_ == 0 || maxEntries == 0) return std::nullopt;
    uint64_t from = lowestNew_;
    if (lastReportedHigh_ != 0) {
        const uint64_t overlapFrom = lastReportedHigh_ + 1 > kOverlap ? lastReportedHigh_ + 1 - kOverlap : 1;
        from = std::min(from, std::max(overlapFrom, first_));
    }
    if (highest_ - from + 1 > maxEntries) from = highest_ - maxEntries + 1;
    if (highest_ - from + 1 > kRing) from = highest_ - kRing + 1;

    TransportFeedback fb;
    fb.reportUs = nowUs;
    fb.basePn = from;
    fb.entries.resize(size_t(highest_ - from + 1));
    for (uint64_t pn = from; pn <= highest_; ++pn) {
        auto& e = fb.entries[size_t(pn - from)];
        e.pn = pn;
        const auto& s = ring_[pn % kRing];
        if (s.pn == pn) {
            e.received = true;
            e.ecn = s.ecn;
            e.arrivalUs = s.arrivalUs;
        }
    }
    lastReportedHigh_ = highest_;
    lowestNew_ = 0;
    newCount_ = 0;
    return fb;
}

} // namespace p2p::net::cc
