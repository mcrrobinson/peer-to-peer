#include "video/window_fec.h"

#include "net/fec.h"
#include "video/video_packetizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace p2p::video {

namespace {
using net::fec::gfInv;
using net::fec::gfMul;
using net::fec::gfMulAdd;

constexpr size_t kLenPrefix = 2;
constexpr int kMaxWindow = 16;
constexpr int32_t kKeepPacketsFrames = 32; // received packets kept, in frames
constexpr int32_t kKeepEqFrames = 16;      // parity kept, in frames (past any grace period)

void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void putU32(uint8_t* p, uint32_t v) { putU16(p, uint16_t(v >> 16)); putU16(p + 2, uint16_t(v)); }
uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t getU32(const uint8_t* p) { return uint32_t(getU16(p)) << 16 | getU16(p + 2); }

uint64_t key(uint32_t frame, uint16_t index) { return uint64_t(frame) << 16 | index; }
uint32_t keyFrame(uint64_t k) { return uint32_t(k >> 16); }

uint8_t coefficient(uint32_t seed, uint32_t frame, uint16_t index)
{
    uint64_t z = (uint64_t(seed) << 32 | frame) * 0x9e3779b97f4a7c15ull + index + 1;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return uint8_t(1 + z % 255);
}

// [u16 length | packet]
std::vector<uint8_t> symbol(const uint8_t* packet, size_t len)
{
    std::vector<uint8_t> s(kLenPrefix + len);
    putU16(s.data(), uint16_t(len));
    std::memcpy(s.data() + kLenPrefix, packet, len);
    return s;
}

void addScaled(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, uint8_t c)
{
    if (dst.size() < src.size()) dst.resize(src.size(), 0);
    gfMulAdd(dst.data(), src.data(), c, src.size());
}
} // namespace

// ------------------------------------------------------------ encoder

size_t WindowFecEncoder::overhead(int windowFrames)
{
    // Parity: header + descriptor + length prefix, around the packet.
    return FragmentHeader::kSize + 1 + 2 * size_t(windowFrames) + 4 + 2 + kLenPrefix;
}

void WindowFecEncoder::add(uint32_t frameId, const uint8_t* payload, size_t len)
{
    if (frames_.empty() || frames_.back().id != frameId) {
        if (!frames_.empty() && frames_.back().id + 1 != frameId) frames_.clear(); // not consecutive
        auto h = FragmentHeader::read(payload, len);
        frames_.push_back({frameId, h ? h->captureUs : 0, {}});
        while (int(frames_.size()) > std::min(window_, kMaxWindow)) frames_.pop_front();
    }
    frames_.back().packets.emplace_back(payload, payload + len);
}

std::vector<std::vector<uint8_t>> WindowFecEncoder::endFrame(uint32_t frameId, double rate, size_t maxPayload)
{
    std::vector<std::vector<uint8_t>> out;
    if (frames_.empty() || frames_.back().id != frameId) return out;
    if (rate <= 0) {
        credit_ = 0;
        return out;
    }
    size_t total = 0;
    for (const auto& f : frames_) total += f.packets.size();
    credit_ += rate * double(frames_.back().packets.size());
    int n = int(std::floor(credit_));
    credit_ -= n;
    n = std::min<int>(n, int(total));

    for (int j = 0; j < n; ++j) {
        // Cover as many of the newest frames as fit in one packet.
        size_t first = 0;
        size_t symLen = 0;
        for (;; ++first) {
            if (first == frames_.size()) return out;
            symLen = 0;
            for (size_t f = first; f < frames_.size(); ++f)
                for (const auto& p : frames_[f].packets) symLen = std::max(symLen, kLenPrefix + p.size());
            const size_t nf = frames_.size() - first;
            if (FragmentHeader::kSize + 1 + 2 * nf + 4 + 2 + symLen <= maxPayload) break;
        }
        const size_t nf = frames_.size() - first;
        const uint32_t seed = seed_ = seed_ * 1664525u + 1013904223u;
        std::vector<uint8_t> pkt(FragmentHeader::kSize + 1 + 2 * nf + 4 + 2 + symLen, 0);
        FragmentHeader h;
        h.frameId = frameId;
        h.index = 0;
        h.count = 1;
        h.flags = FragmentHeader::kWindowParity;
        h.captureUs = frames_.back().captureUs;
        h.encodeDelay10Us = uint16_t(std::clamp<int64_t>((frames_.back().captureUs - frames_[first].captureUs) / 10, 0, 0xffff));
        h.write(pkt.data());
        uint8_t* p = pkt.data() + FragmentHeader::kSize;
        *p++ = uint8_t(nf);
        for (size_t f = first; f < frames_.size(); ++f, p += 2) putU16(p, uint16_t(frames_[f].packets.size()));
        putU32(p, seed);
        putU16(p + 4, uint16_t(symLen));
        uint8_t* sym = p + 6;
        for (size_t f = first; f < frames_.size(); ++f)
            for (size_t i = 0; i < frames_[f].packets.size(); ++i) {
                const auto& src = frames_[f].packets[i];
                const uint8_t c = coefficient(seed, frames_[f].id, uint16_t(i));
                uint8_t len[2];
                putU16(len, uint16_t(src.size()));
                gfMulAdd(sym, len, c, kLenPrefix);
                gfMulAdd(sym + kLenPrefix, src.data(), c, src.size());
            }
        out.push_back(std::move(pkt));
    }
    return out;
}

// ------------------------------------------------------------ decoder

void WindowFecDecoder::prune(uint32_t newest)
{
    if (!haveNewest_ || int32_t(newest - newest_) > 0) {
        newest_ = newest;
        haveNewest_ = true;
    }
    for (auto it = known_.begin(); it != known_.end();)
        it = int32_t(newest_ - keyFrame(it->first)) > kKeepPacketsFrames ? known_.erase(it) : std::next(it);
    eqs_.erase(std::remove_if(eqs_.begin(), eqs_.end(),
                              [&](const Eq& e) { return e.unknown.empty() || int32_t(newest_ - e.newest) > kKeepEqFrames; }),
               eqs_.end());
}

void WindowFecDecoder::learn(uint64_t k, std::vector<uint8_t> sym, std::vector<std::vector<uint8_t>>& out)
{
    (void)out;
    auto [it, fresh] = known_.emplace(k, std::move(sym));
    if (!fresh) return;
    for (Eq& e : eqs_) {
        auto u = e.unknown.find(k);
        if (u == e.unknown.end()) continue;
        addScaled(e.rhs, it->second, u->second);
        e.unknown.erase(u);
    }
}

std::vector<std::vector<uint8_t>> WindowFecDecoder::onData(const uint8_t* payload, size_t len)
{
    std::vector<std::vector<uint8_t>> out;
    auto h = FragmentHeader::read(payload, len);
    if (!h || (h->flags & FragmentHeader::kWindowParity) || h->isParity()) return out;
    // Exactly as first sent (a resend sets kRetransmit).
    std::vector<uint8_t> sym = symbol(payload, len);
    sym[kLenPrefix + 16] &= uint8_t(~FragmentHeader::kRetransmit);
    const uint64_t k = key(h->frameId, h->index);
    if (known_.count(k)) return out;
    bool touches = false;
    for (const Eq& e : eqs_) touches |= e.unknown.count(k) != 0;
    learn(k, std::move(sym), out);
    if (touches) solve(out);
    prune(h->frameId);
    return out;
}

std::vector<std::vector<uint8_t>> WindowFecDecoder::onParity(const uint8_t* payload, size_t len)
{
    std::vector<std::vector<uint8_t>> out;
    auto h = FragmentHeader::read(payload, len);
    if (!h || !(h->flags & FragmentHeader::kWindowParity)) return out;
    size_t off = FragmentHeader::kSize;
    if (len < off + 1) return out;
    const size_t nf = payload[off++];
    if (nf == 0 || nf > size_t(kMaxWindow) || len < off + 2 * nf + 6) return out;
    std::vector<uint16_t> counts(nf);
    for (size_t f = 0; f < nf; ++f, off += 2) counts[f] = getU16(payload + off);
    const uint32_t seed = getU32(payload + off);
    const size_t symLen = getU16(payload + off + 4);
    off += 6;
    if (symLen < kLenPrefix || len < off + symLen) return out;

    spanUs_ = int64_t(h->encodeDelay10Us) * 10;
    Eq e;
    e.newest = h->frameId;
    e.rhs.assign(payload + off, payload + off + symLen);
    for (size_t f = 0; f < nf; ++f) {
        const uint32_t frame = h->frameId - uint32_t(nf - 1 - f);
        for (uint16_t i = 0; i < counts[f]; ++i) {
            const uint64_t k = key(frame, i);
            const uint8_t c = coefficient(seed, frame, i);
            if (auto it = known_.find(k); it != known_.end()) {
                if (it->second.size() > symLen) return out; // inconsistent with what we have
                addScaled(e.rhs, it->second, c);
            } else {
                e.unknown.emplace(k, c);
            }
        }
    }
    prune(h->frameId);
    if (e.unknown.empty()) return out;
    eqs_.push_back(std::move(e));
    solve(out);
    return out;
}

// Gaussian elimination on the coefficients only, tracking which equations
// each row combines; the (long) symbols are combined just for the rows that
// end up solving a packet.
void WindowFecDecoder::solve(std::vector<std::vector<uint8_t>>& out)
{
    for (;;) {
        std::vector<uint64_t> cols;
        for (const Eq& e : eqs_)
            for (const auto& [k, c] : e.unknown) cols.push_back(k);
        std::sort(cols.begin(), cols.end());
        cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
        const size_t rows = eqs_.size(), nc = cols.size();
        if (nc == 0 || rows == 0) return;

        std::vector<std::vector<uint8_t>> a(rows, std::vector<uint8_t>(nc, 0)), comb(rows, std::vector<uint8_t>(rows, 0));
        for (size_t r = 0; r < rows; ++r) {
            for (const auto& [k, c] : eqs_[r].unknown)
                a[r][size_t(std::lower_bound(cols.begin(), cols.end(), k) - cols.begin())] = c;
            comb[r][r] = 1;
        }
        size_t rank = 0;
        std::vector<size_t> pivotCol;
        for (size_t c = 0; c < nc && rank < rows; ++c) {
            size_t p = rank;
            while (p < rows && a[p][c] == 0) ++p;
            if (p == rows) continue;
            std::swap(a[p], a[rank]);
            std::swap(comb[p], comb[rank]);
            const uint8_t inv = gfInv(a[rank][c]);
            for (auto& x : a[rank]) x = gfMul(x, inv);
            for (auto& x : comb[rank]) x = gfMul(x, inv);
            for (size_t r = 0; r < rows; ++r) {
                if (r == rank || a[r][c] == 0) continue;
                const uint8_t f = a[r][c];
                for (size_t j = 0; j < nc; ++j) a[r][j] ^= gfMul(f, a[rank][j]);
                for (size_t j = 0; j < rows; ++j) comb[r][j] ^= gfMul(f, comb[rank][j]);
            }
            pivotCol.push_back(c);
            ++rank;
        }
        bool progress = false;
        for (size_t r = 0; r < rank; ++r) {
            const size_t nz = size_t(std::count_if(a[r].begin(), a[r].end(), [](uint8_t x) { return x != 0; }));
            if (nz != 1) continue;
            std::vector<uint8_t> sym;
            for (size_t j = 0; j < rows; ++j)
                if (comb[r][j]) addScaled(sym, eqs_[j].rhs, comb[r][j]);
            if (sym.size() < kLenPrefix) continue;
            const size_t plen = getU16(sym.data());
            if (plen == 0 || kLenPrefix + plen > sym.size()) continue; // not a packet: give up on it
            sym.resize(kLenPrefix + plen);
            out.emplace_back(sym.begin() + kLenPrefix, sym.end());
            ++recovered_;
            learn(cols[pivotCol[r]], std::move(sym), out);
            progress = true;
        }
        eqs_.erase(std::remove_if(eqs_.begin(), eqs_.end(), [](const Eq& e) { return e.unknown.empty(); }), eqs_.end());
        if (!progress) return;
    }
}

} // namespace p2p::video
