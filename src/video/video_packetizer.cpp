#include "video/video_packetizer.h"

#include <algorithm>
#include <cstring>

namespace p2p::video {

namespace {
void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void putU32(uint8_t* p, uint32_t v) { putU16(p, uint16_t(v >> 16)); putU16(p + 2, uint16_t(v)); }
void putU64(uint8_t* p, uint64_t v) { putU32(p, uint32_t(v >> 32)); putU32(p + 4, uint32_t(v)); }
uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t getU32(const uint8_t* p) { return uint32_t(getU16(p)) << 16 | getU16(p + 2); }
uint64_t getU64(const uint8_t* p) { return uint64_t(getU32(p)) << 32 | getU32(p + 4); }
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
    p[22] = p[23] = 0;
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
    if (h.count == 0 || h.index >= h.count) return std::nullopt;
    return h;
}

size_t packetizeFrame(uint32_t frameId, int64_t captureUs, bool keyframe, uint8_t codec, const uint8_t* data,
                      size_t size, size_t maxPayload,
                      const std::function<void(const uint8_t* payload, size_t len)>& emit, uint8_t extraFlags, uint16_t width, uint16_t height)
{
    if (maxPayload <= FragmentHeader::kSize || size == 0) return 0;
    const size_t chunk = maxPayload - FragmentHeader::kSize;
    const size_t count = (size + chunk - 1) / chunk;
    if (count > 0xffff) return 0;

    // Spread bytes evenly so the last fragment isn't a tiny runt.
    const size_t per = (size + count - 1) / count;
    uint8_t buf[2048];
    FragmentHeader h;
    h.frameId = frameId;
    h.count = uint16_t(count);
    h.captureUs = captureUs;
    h.flags = uint8_t((keyframe ? FragmentHeader::kKeyframe : 0) | (extraFlags & ~FragmentHeader::kKeyframe));
    h.codec = codec;
    h.width = width;
    h.height = height;
    size_t off = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t n = std::min(per, size - off);
        h.index = uint16_t(i);
        h.write(buf);
        std::memcpy(buf + FragmentHeader::kSize, data + off, n);
        emit(buf, FragmentHeader::kSize + n);
        off += n;
    }
    return count;
}

// ------------------------------------------------------------ FrameAssembler

void FrameAssembler::insert(const FragmentHeader& h, const uint8_t* data, size_t len, int64_t arrivalUs)
{
    ++st_.fragments;
    if (haveNext_ && int32_t(h.frameId - nextId_) < 0) {
        ++st_.lateFragments;
        return;
    }
    if (ready_.count(h.frameId)) return; // duplicate of a completed frame

    auto it = partial_.find(h.frameId);
    if (it == partial_.end()) {
        if (partial_.size() >= cfg_.maxPartialFrames) partial_.erase(partial_.begin());
        Partial p;
        p.count = h.count;
        p.keyframe = h.flags & FragmentHeader::kKeyframe;
        p.flags = h.flags;
        p.width = h.width;
        p.height = h.height;
        p.codec = h.codec;
        p.captureUs = h.captureUs;
        p.firstArrivalUs = arrivalUs;
        p.chunks.resize(h.count);
        it = partial_.emplace(h.frameId, std::move(p)).first;
    }
    Partial& p = it->second;
    if (p.count != h.count) {
        ++st_.malformed;
        return;
    }
    if (!p.chunks[h.index].empty() || len == 0) return; // duplicate fragment
    p.chunks[h.index].assign(data, data + len);
    if (++p.received == p.count) {
        Partial done = std::move(p);
        partial_.erase(it);
        complete(h.frameId, std::move(done), arrivalUs);
    }
}

void FrameAssembler::complete(uint32_t id, Partial&& p, int64_t nowUs)
{
    AssembledFrame f;
    f.id = id;
    f.keyframe = p.keyframe;
    f.flags = p.flags;
    f.width = p.width;
    f.height = p.height;
    f.codec = p.codec;
    f.captureUs = p.captureUs;
    f.firstArrivalUs = p.firstArrivalUs;
    f.completeUs = nowUs;
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
    for (auto it = ready_.begin(); it != ready_.end();) {
        if (int32_t(it->first - id) < 0) {
            ++st_.framesSkipped;
            it = ready_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<AssembledFrame> FrameAssembler::poll(int64_t nowUs)
{
    std::vector<AssembledFrame> out;
    for (;;) {
        if (!haveNext_) {
            // Resynchronise on the oldest complete keyframe.
            auto key = std::find_if(ready_.begin(), ready_.end(), [](const auto& kv) { return kv.second.keyframe; });
            if (key == ready_.end()) break;
            nextId_ = key->first;
            dropBefore(nextId_);
            haveNext_ = true;
            gapSinceUs_ = -1;
        }

        if (auto it = ready_.find(nextId_); it != ready_.end()) {
            out.push_back(std::move(it->second));
            ready_.erase(it);
            ++nextId_;
            ++st_.framesReleased;
            gapSinceUs_ = -1;
            continue;
        }

        // nextId_ is missing. A complete keyframe further on lets us jump.
        auto key = std::find_if(ready_.begin(), ready_.end(), [&](const auto& kv) {
            return kv.second.keyframe && int32_t(kv.first - nextId_) > 0;
        });
        if (key != ready_.end()) {
            st_.framesLost += key->first - nextId_;
            nextId_ = key->first;
            dropBefore(nextId_);
            gapSinceUs_ = -1;
            continue;
        }

        // Anything newer than nextId_ means a gap; give reordering a moment.
        const bool newer = std::any_of(ready_.begin(), ready_.end(), [&](const auto& kv) {
                               return int32_t(kv.first - nextId_) > 0;
                           }) ||
                           std::any_of(partial_.begin(), partial_.end(), [&](const auto& kv) {
                               return int32_t(kv.first - nextId_) > 0;
                           });
        if (newer) {
            if (gapSinceUs_ < 0) gapSinceUs_ = nowUs;
            if (nowUs - gapSinceUs_ >= cfg_.reorderGraceUs) {
                ++st_.framesLost;
                haveNext_ = false; // need a keyframe
                gapSinceUs_ = -1;
                dropBefore(nextId_ + 1);
                continue;
            }
        }
        break;
    }
    return out;
}

} // namespace p2p::video
