#include "video/video_packetizer.h"

#include "net/fec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace p2p::video {

namespace {
void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void putU32(uint8_t* p, uint32_t v) { putU16(p, uint16_t(v >> 16)); putU16(p + 2, uint16_t(v)); }
void putU64(uint8_t* p, uint64_t v) { putU32(p, uint32_t(v >> 32)); putU32(p + 4, uint32_t(v)); }
uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t getU32(const uint8_t* p) { return uint32_t(getU16(p)) << 16 | getU16(p + 2); }
uint64_t getU64(const uint8_t* p) { return uint64_t(getU32(p)) << 32 | getU32(p + 4); }

constexpr size_t kLenPrefix = 2; // FEC symbols start with the fragment length

// Data fragment index range of |block|.
std::pair<int, int> blockRange(const FragmentHeader& h, int block)
{
    const int lo = block * h.fecBlock;
    return {lo, std::min<int>(h.count, lo + h.fecBlock)};
}
} // namespace

void FragmentHeader::write(uint8_t* p) const
{
    putU32(p, frameId);
    putU16(p + 4, index);
    putU16(p + 6, count);
    putU64(p + 8, uint64_t(captureUs));
    p[16] = flags;
    p[17] = codec;
    putU16(p + 18, width);
    putU16(p + 20, height);
    p[22] = fecParity;
    p[23] = fecBlock;
    putU16(p + 24, encodeDelay10Us);
    p[26] = refBack;
}

std::optional<FragmentHeader> FragmentHeader::read(const uint8_t* p, size_t len)
{
    if (len < kSize) return std::nullopt;
    FragmentHeader h;
    h.frameId = getU32(p);
    h.index = getU16(p + 4);
    h.count = getU16(p + 6);
    h.captureUs = int64_t(getU64(p + 8));
    h.flags = p[16];
    h.codec = p[17];
    h.width = getU16(p + 18);
    h.height = getU16(p + 20);
    h.fecParity = p[22];
    h.fecBlock = p[23];
    h.encodeDelay10Us = getU16(p + 24);
    h.refBack = p[26];
    if (h.count == 0) return std::nullopt;
    if (h.fecParity == 0) {
        if (h.index >= h.count) return std::nullopt;
    } else {
        if (h.fecBlock == 0 || int(h.fecBlock) + int(h.fecParity) > net::fec::kMaxShards) return std::nullopt;
        if (int(h.index) >= int(h.count) + h.blocks() * int(h.fecParity)) return std::nullopt;
    }
    return h;
}

size_t packetizeFrame(uint32_t frameId, int64_t captureUs, bool keyframe, uint8_t codec, const uint8_t* data,
                      size_t size, size_t maxPayload,
                      const std::function<void(const uint8_t* payload, size_t len)>& sendFragment,
                      const PacketizeOptions& opt)
{
    if (maxPayload <= FragmentHeader::kSize + kLenPrefix || size == 0) return 0;
    // Room for the FEC length prefix so parity fragments fit too.
    const size_t chunk = maxPayload - FragmentHeader::kSize - kLenPrefix;
    const size_t count = (size + chunk - 1) / chunk;
    if (count > 0xfff0) return 0;

    // Spread bytes evenly so the last fragment isn't a tiny runt.
    const size_t per = (size + count - 1) / count;
    uint8_t buf[2048];
    FragmentHeader h;
    h.frameId = frameId;
    h.count = uint16_t(count);
    h.captureUs = captureUs;
    h.flags = uint8_t((keyframe ? FragmentHeader::kKeyframe : 0) |
                      (opt.extraFlags & ~(FragmentHeader::kKeyframe | FragmentHeader::kRetransmit)));
    h.codec = codec;
    h.refBack = keyframe ? 0 : opt.refBack;
    if (opt.encodedUs > 0)
        h.encodeDelay10Us = uint16_t(std::clamp<int64_t>((opt.encodedUs - captureUs) / 10, 0, 0xffff));
    h.width = opt.width;
    h.height = opt.height;
    if (opt.fecRate > 0) {
        const int block = std::clamp(opt.fecBlockSize, 1, 200);
        const int blockLen = std::min<int>(block, int(count));
        h.fecBlock = uint8_t(block);
        h.fecParity = uint8_t(std::clamp(int(std::ceil(blockLen * opt.fecRate)), 1, 255 - block));
    }

    size_t off = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t n = std::min(per, size - off);
        h.index = uint16_t(i);
        h.write(buf);
        std::memcpy(buf + FragmentHeader::kSize, data + off, n);
        sendFragment(buf, FragmentHeader::kSize + n);
        off += n;
    }

    if (h.fecParity) {
        const size_t symLen = kLenPrefix + per;
        std::vector<uint8_t> symbols;
        std::vector<std::vector<uint8_t>> parity;
        for (int b = 0; b < h.blocks(); ++b) {
            const auto [lo, hi] = blockRange(h, b);
            symbols.assign(size_t(hi - lo) * symLen, 0);
            std::vector<const uint8_t*> ptrs;
            for (int i = lo; i < hi; ++i) {
                uint8_t* s = symbols.data() + size_t(i - lo) * symLen;
                const size_t start = size_t(i) * per;
                const size_t n = std::min(per, size - start);
                putU16(s, uint16_t(n));
                std::memcpy(s + kLenPrefix, data + start, n);
                ptrs.push_back(s);
            }
            net::fec::encode(ptrs, symLen, h.fecParity, parity);
            for (int j = 0; j < h.fecParity; ++j) {
                h.index = uint16_t(int(count) + b * h.fecParity + j);
                h.write(buf);
                std::memcpy(buf + FragmentHeader::kSize, parity[size_t(j)].data(), symLen);
                sendFragment(buf, FragmentHeader::kSize + symLen);
            }
        }
    }
    return count;
}

// ------------------------------------------------------------ FrameAssembler

void FrameAssembler::insert(const FragmentHeader& h, const uint8_t* data, size_t len, int64_t arrivalUs)
{
    ++st_.fragments;
    if (h.flags & FragmentHeader::kRetransmit) ++st_.retransmitsReceived;
    if (h.isParity()) ++st_.parityReceived;
    if ((haveNext_ || haveFloor_) && int32_t(h.frameId - nextId_) < 0) {
        ++st_.lateFragments;
        return;
    }
    if (!haveHighest_ || int32_t(h.frameId - highestId_) > 0) {
        highestId_ = h.frameId;
        haveHighest_ = true;
    }
    missing_.erase(h.frameId);
    // Typical spacing of one frame's fragments (wide when the sender paces).
    if (!(h.flags & FragmentHeader::kRetransmit)) {
        if (haveLastFragment_ && lastFragmentFrame_ == h.frameId) {
            const int64_t gap = std::clamp<int64_t>(arrivalUs - lastFragmentUs_, 0, 50'000);
            fragmentGapUs_ = fragmentGapUs_ ? (7 * fragmentGapUs_ + gap) / 8 : gap;
        }
        haveLastFragment_ = true;
        lastFragmentFrame_ = h.frameId;
        lastFragmentUs_ = arrivalUs;
    }
    if (ready_.count(h.frameId)) return; // duplicate of a completed frame

    auto it = partial_.find(h.frameId);
    if (it == partial_.end()) {
        if (partial_.size() >= cfg_.maxPartialFrames) partial_.erase(partial_.begin());
        Partial p;
        p.first = h;
        p.firstArrivalUs = arrivalUs;
        p.chunks.resize(h.count);
        p.retries.resize(h.count);
        p.parity.resize(size_t(h.blocks() * h.fecParity));
        it = partial_.emplace(h.frameId, std::move(p)).first;
    }
    Partial& p = it->second;
    if (p.first.count != h.count || p.first.fecParity != h.fecParity || p.first.fecBlock != h.fecBlock) {
        ++st_.malformed;
        return;
    }
    p.lastArrivalUs = arrivalUs;

    int block = -1;
    if (h.isParity()) {
        const size_t pi = size_t(h.index - h.count);
        if (!p.parity[pi].empty() || len <= kLenPrefix) return;
        if (p.symbolLen == 0) p.symbolLen = len;
        if (len != p.symbolLen) {
            ++st_.malformed;
            return;
        }
        p.parity[pi].assign(data, data + len);
        block = int(pi) / h.fecParity;
    } else {
        if (!p.chunks[h.index].empty() || len == 0) return; // duplicate fragment
        p.chunks[h.index].assign(data, data + len);
        ++p.received;
        if (h.flags & FragmentHeader::kRetransmit) {
            ++st_.retransmitsUseful;
            p.recovered = true;
        }
        if (h.fecParity) block = h.index / h.fecBlock;
    }
    if (block >= 0 && p.received < h.count) tryRecover(p, block);

    if (p.received == p.first.count) {
        Partial done = std::move(p);
        partial_.erase(it);
        complete(h.frameId, std::move(done), arrivalUs);
    }
}

void FrameAssembler::tryRecover(Partial& p, int block)
{
    const FragmentHeader& h = p.first;
    if (p.symbolLen == 0) return;
    const auto [lo, hi] = blockRange(h, block);
    const int k = hi - lo, m = h.fecParity;
    int haveData = 0, haveParity = 0;
    for (int i = lo; i < hi; ++i) haveData += !p.chunks[size_t(i)].empty();
    if (haveData == k) return;
    for (int j = 0; j < m; ++j) haveParity += !p.parity[size_t(block * m + j)].empty();
    if (haveData + haveParity < k) return;

    const size_t L = p.symbolLen;
    std::vector<std::vector<uint8_t>> shards(size_t(k + m));
    for (int i = lo; i < hi; ++i) {
        const auto& c = p.chunks[size_t(i)];
        if (c.empty()) continue;
        if (c.size() + kLenPrefix > L) return; // inconsistent sizes; leave it to NACK
        auto& s = shards[size_t(i - lo)];
        s.assign(L, 0);
        putU16(s.data(), uint16_t(c.size()));
        std::memcpy(s.data() + kLenPrefix, c.data(), c.size());
    }
    for (int j = 0; j < m; ++j) shards[size_t(k + j)] = p.parity[size_t(block * m + j)];
    if (!net::fec::decode(k, m, L, shards)) return;

    for (int i = lo; i < hi; ++i) {
        auto& c = p.chunks[size_t(i)];
        if (!c.empty()) continue;
        const auto& s = shards[size_t(i - lo)];
        const size_t n = getU16(s.data());
        if (n == 0 || n + kLenPrefix > L) {
            ++st_.malformed;
            return;
        }
        c.assign(s.begin() + kLenPrefix, s.begin() + long(kLenPrefix + n));
        ++p.received;
        ++st_.fecRecoveredFragments;
    }
    if (!p.recovered) ++st_.fecRecoveredFrames;
    p.recovered = true;
}

void FrameAssembler::complete(uint32_t id, Partial&& p, int64_t nowUs)
{
    AssembledFrame f;
    f.id = id;
    f.keyframe = p.first.flags & FragmentHeader::kKeyframe;
    f.flags = p.first.flags & ~FragmentHeader::kRetransmit;
    f.width = p.first.width;
    f.height = p.first.height;
    f.codec = p.first.codec;
    f.captureUs = p.first.captureUs;
    f.encodedUs = p.first.encodeDelay10Us ? p.first.captureUs + int64_t(p.first.encodeDelay10Us) * 10 : 0;
    f.firstArrivalUs = p.firstArrivalUs;
    f.completeUs = nowUs;
    f.recovered = p.recovered;
    f.refBack = f.keyframe ? 0 : p.first.refBack;
    size_t total = 0;
    for (const auto& c : p.chunks) total += c.size();
    if (total > cfg_.maxFrameBytes) {
        ++st_.malformed;
        return;
    }
    f.data.reserve(total);
    for (const auto& c : p.chunks) f.data.insert(f.data.end(), c.begin(), c.end());
    ++st_.framesComplete;
    ready_.emplace(id, std::move(f));
}

void FrameAssembler::dropBefore(uint32_t id)
{
    for (auto it = partial_.begin(); it != partial_.end();)
        it = int32_t(it->first - id) < 0 ? partial_.erase(it) : std::next(it);
    for (auto it = missing_.begin(); it != missing_.end();)
        it = int32_t(it->first - id) < 0 ? missing_.erase(it) : std::next(it);
    for (auto it = ready_.begin(); it != ready_.end();) {
        if (int32_t(it->first - id) < 0) {
            ++st_.framesSkipped;
            it = ready_.erase(it);
        } else {
            ++it;
        }
    }
}

bool FrameAssembler::released(uint32_t id) const
{
    return std::find(released_.begin(), released_.end(), id) != released_.end();
}

bool FrameAssembler::startsDecodable(uint32_t id, bool keyframe, uint8_t refBack) const
{
    return keyframe || (refBack && released(id - refBack));
}

std::optional<uint32_t> FrameAssembler::lastReleased() const
{
    if (released_.empty()) return std::nullopt;
    return released_.back();
}

std::vector<AssembledFrame> FrameAssembler::poll(int64_t nowUs)
{
    std::vector<AssembledFrame> out;
    auto decodable = [&](const auto& kv) { return startsDecodable(kv.first, kv.second.keyframe, kv.second.refBack); };
    for (;;) {
        if (!haveNext_) {
            // Resynchronise on the oldest complete frame we can decode: a
            // keyframe, or one that predicts from a frame we released (the
            // sender's answer to a recovery request).
            auto key = std::find_if(ready_.begin(), ready_.end(), decodable);
            if (key == ready_.end()) break;
            nextId_ = key->first;
            dropBefore(nextId_);
            haveNext_ = true;
            gapSinceUs_ = -1;
        }

        if (auto it = ready_.find(nextId_); it != ready_.end()) {
            released_.push_back(nextId_);
            if (released_.size() > kReleasedHistory) released_.pop_front();
            out.push_back(std::move(it->second));
            ready_.erase(it);
            ++nextId_;
            ++st_.framesReleased;
            gapSinceUs_ = -1;
            continue;
        }

        // nextId_ is missing. A complete frame further on that doesn't
        // need it (a keyframe or a recovery frame) lets us jump.
        auto key = std::find_if(ready_.begin(), ready_.end(), [&](const auto& kv) {
            return int32_t(kv.first - nextId_) > 0 && decodable(kv);
        });
        if (key != ready_.end()) {
            st_.framesLost += key->first - nextId_;
            nextId_ = key->first;
            dropBefore(nextId_);
            gapSinceUs_ = -1;
            continue;
        }

        // Anything newer than nextId_ means a gap; give FEC / NACK a moment.
        const bool newer = haveHighest_ && int32_t(highestId_ - nextId_) > 0;
        if (newer) {
            if (gapSinceUs_ < 0) gapSinceUs_ = nowUs;
            if (nowUs - gapSinceUs_ >= cfg_.reorderGraceUs) {
                ++st_.framesLost;
                haveNext_ = false; // need a keyframe
                gapSinceUs_ = -1;
                dropBefore(nextId_ + 1);
                ++nextId_; // never resync on (or accept) anything at or before the lost frame
                haveFloor_ = true;
                continue;
            }
        }
        break;
    }
    return out;
}

std::vector<NackItem> FrameAssembler::collectNacks(int64_t nowUs, const NackPolicy& policy)
{
    std::vector<NackItem> out;
    auto due = [&](Retry& r) {
        if (r.tries >= policy.maxTries) return false;
        if (r.tries > 0 && nowUs - r.lastUs < policy.retryUs) return false;
        ++r.tries;
        r.lastUs = nowUs;
        return true;
    };

    // Holes inside frames we have part of.
    for (auto& [id, p] : partial_) {
        if (haveNext_ && int32_t(id - nextId_) < 0) continue;
        // A paced frame arrives spread out: wait for a few fragment gaps,
        // not just the fixed reorder time.
        if (nowUs - p.lastArrivalUs < std::max(policy.reorderUs, 3 * fragmentGapUs_)) continue;
        const FragmentHeader& h = p.first;
        // While waiting to resync only a frame we could start from helps.
        if (!haveNext_ && !startsDecodable(id, h.flags & FragmentHeader::kKeyframe, h.refBack)) continue;
        const int blocks = h.fecParity ? h.blocks() : 1;
        for (int b = 0; b < blocks; ++b) {
            const auto [lo, hi] = h.fecParity ? blockRange(h, b) : std::pair<int, int>{0, int(h.count)};
            int have = 0;
            for (int i = lo; i < hi; ++i) have += !p.chunks[size_t(i)].empty();
            for (int j = 0; h.fecParity && j < h.fecParity; ++j) have += !p.parity[size_t(b * h.fecParity + j)].empty();
            int need = (hi - lo) - have; // fragments FEC can't make up for
            for (int i = lo; i < hi && need > 0; ++i) {
                if (!p.chunks[size_t(i)].empty()) continue;
                --need;
                if (due(p.retries[size_t(i)])) out.push_back({id, uint16_t(i)});
            }
        }
    }

    // Frames with nothing at all, between the decode point and the newest frame.
    if (haveNext_ && haveHighest_) {
        const int32_t span = std::min<int32_t>(int32_t(highestId_ - nextId_), 64);
        for (int32_t d = 0; d < span; ++d) {
            const uint32_t id = nextId_ + uint32_t(d);
            if (known(id)) continue;
            auto [it, fresh] = missing_.try_emplace(id);
            if (fresh) it->second.sinceUs = nowUs;
            if (nowUs - it->second.sinceUs < policy.reorderUs) continue;
            if (due(it->second.retry)) out.push_back({id, NackItem::kWholeFrame});
        }
    }
    st_.nacked += out.size();
    return out;
}

} // namespace p2p::video
