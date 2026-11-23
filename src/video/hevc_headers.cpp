#include "video/hevc_headers.h"

#include <algorithm>

namespace p2p::video::hevc {

namespace {

// RBSP bit writer (Exp-Golomb as in H.265 9.2).
struct Bits {
    std::vector<uint8_t> b;
    uint32_t acc = 0;
    int n = 0;

    void u(int bits, uint32_t v)
    {
        for (int i = bits - 1; i >= 0; --i) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++n == 8) {
                b.push_back(uint8_t(acc));
                acc = 0;
                n = 0;
            }
        }
    }
    void flag(bool v) { u(1, v ? 1 : 0); }
    void ue(uint32_t v)
    {
        const uint64_t x = uint64_t(v) + 1;
        int len = 0;
        while ((x >> (len + 1)) != 0) ++len;
        u(len, 0);
        u(len + 1, uint32_t(x));
    }
    void se(int32_t v) { ue(v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * int64_t(v))); }
    // rbsp_trailing_bits() and the slice header's byte_alignment() are the same.
    void trailing()
    {
        u(1, 1);
        while (n) u(1, 0);
    }
};

std::vector<uint8_t> nal(int type, const Bits& rbsp)
{
    std::vector<uint8_t> o = {0, 0, 0, 1, uint8_t(type << 1), 1}; // layer 0, temporal id 0
    int zeros = 0;
    for (uint8_t c : rbsp.b) {
        if (zeros >= 2 && c <= 3) {
            o.push_back(3);
            zeros = 0;
        }
        o.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return o;
}

enum Nal { TrailR = 1, IdrWRadl = 19, Vps = 32, Sps = 33, Pps = 34 };

void profileTierLevel(Bits& w, const StreamParams& p)
{
    w.u(2, 0);   // profile space
    w.flag(0);   // main tier
    w.u(5, 1);   // Main
    for (int j = 0; j < 32; ++j) w.flag(j == 1 || j == 2);
    w.flag(1);   // progressive source
    w.flag(0);   // interlaced
    w.flag(0);   // non packed constraint
    w.flag(1);   // frame only
    w.u(32, 0);  // 43 reserved bits + inbld
    w.u(12, 0);
    w.u(8, uint32_t(p.level()));
}

void subLayerOrdering(Bits& w, const StreamParams& p)
{
    w.flag(0); // ordering info only for the highest sub-layer
    w.ue(uint32_t(p.maxDpb - 1));
    w.ue(0); // no reordering
    w.ue(0);
}

} // namespace

int StreamParams::level() const
{
    // Table A.8: max luma picture size and sample rate per level.
    struct L {
        int idc;
        int64_t ps, sps;
    };
    static const L levels[] = {{93, 983'040, 33'177'600},       {120, 2'228'224, 66'846'720},
                               {123, 2'228'224, 133'693'440},   {150, 8'912'896, 267'386'880},
                               {153, 8'912'896, 534'773'760},   {156, 8'912'896, 1'069'547'520},
                               {183, 35'651'584, 1'069'547'520}};
    const int64_t ps = int64_t(codedWidth()) * codedHeight();
    for (const L& l : levels) {
        // The DPB must fit too (A.4.2: 6 pictures at full size, more if smaller).
        const int maxDpbSize = ps <= l.ps / 4 ? 16 : ps <= l.ps / 2 ? 12 : ps <= l.ps * 3 / 4 ? 8 : 6;
        if (ps <= l.ps && ps * fps <= l.sps && maxDpb <= maxDpbSize) return l.idc;
    }
    return 186;
}

std::vector<uint8_t> vps(const StreamParams& p)
{
    Bits w;
    w.u(4, 0);      // vps id
    w.flag(1);      // base layer internal
    w.flag(1);      // base layer available
    w.u(6, 0);      // max layers - 1
    w.u(3, 0);      // max sub-layers - 1
    w.flag(1);      // temporal id nesting
    w.u(16, 0xffff);
    profileTierLevel(w, p);
    subLayerOrdering(w, p);
    w.u(6, 0);      // max layer id
    w.ue(0);        // layer sets - 1
    w.flag(0);      // timing info
    w.flag(0);      // extension
    w.trailing();
    return nal(Vps, w);
}

std::vector<uint8_t> sps(const StreamParams& p)
{
    Bits w;
    w.u(4, 0);  // vps id
    w.u(3, 0);  // max sub-layers - 1
    w.flag(1);  // temporal id nesting
    profileTierLevel(w, p);
    w.ue(0);    // sps id
    w.ue(1);    // 4:2:0
    w.ue(uint32_t(p.codedWidth()));
    w.ue(uint32_t(p.codedHeight()));
    const int cropR = p.codedWidth() - p.width, cropB = p.codedHeight() - p.height;
    w.flag(cropR || cropB); // conformance window, in chroma samples
    if (cropR || cropB) {
        w.ue(0);
        w.ue(uint32_t(cropR / 2));
        w.ue(0);
        w.ue(uint32_t(cropB / 2));
    }
    w.ue(0);    // luma bit depth - 8
    w.ue(0);    // chroma bit depth - 8
    w.ue(uint32_t(p.log2MaxPocLsb - 4));
    subLayerOrdering(w, p);
    w.ue(0);    // min CB 8
    w.ue(3);    // CTB 64
    w.ue(0);    // min TB 4
    w.ue(3);    // max TB 32
    w.ue(4);    // transform hierarchy depth, inter
    w.ue(4);    // and intra
    w.flag(0);  // scaling lists
    w.flag(1);  // AMP
    w.flag(1);  // SAO
    w.flag(0);  // PCM
    w.ue(0);    // no RPS in the SPS: every slice carries its own
    w.flag(0);  // long-term refs
    w.flag(0);  // TMVP (would tie a picture to its reference's motion)
    w.flag(0);  // strong intra smoothing

    w.flag(1);  // VUI
    w.flag(1);  // aspect ratio
    w.u(8, 1);  //   1:1
    w.flag(0);  // overscan
    w.flag(1);  // video signal type
    w.u(3, 5);  //   unspecified format
    const bool full = p.colour == Colour::Bt601Full;
    w.flag(full);
    w.flag(1);  //   colour description
    const uint32_t c = full ? 6 : 1; // SMPTE 170M (BT.601) or BT.709, primaries/transfer/matrix
    w.u(8, c);
    w.u(8, c);
    w.u(8, c);
    w.flag(0);  // chroma location
    w.flag(0);  // neutral chroma
    w.flag(0);  // field seq
    w.flag(0);  // frame field info
    w.flag(0);  // default display window
    w.flag(1);  // timing
    w.u(32, 1);
    w.u(32, uint32_t(p.fps));
    w.flag(0);  // POC proportional to timing
    w.flag(0);  // HRD parameters
    w.flag(0);  // bitstream restriction

    w.flag(0);  // extension
    w.trailing();
    return nal(Sps, w);
}

std::vector<uint8_t> pps(const StreamParams& p)
{
    Bits w;
    w.ue(0);    // pps id
    w.ue(0);    // sps id
    w.flag(0);  // dependent slice segments
    w.flag(0);  // output flag present
    w.u(3, 0);  // extra slice header bits
    w.flag(0);  // sign data hiding
    w.flag(1);  // cabac_init_present
    w.ue(0);    // L0 default active - 1
    w.ue(0);    // L1 default active - 1
    w.se(0);    // init QP - 26
    w.flag(p.constrainedIntraPred);
    w.flag(0);  // transform skip
    w.flag(1);  // cu_qp_delta (rate control adapts QP per CU)
    w.ue(0);    // diff_cu_qp_delta_depth
    w.se(0);    // cb offset
    w.se(0);    // cr offset
    w.flag(0);  // slice chroma QP offsets
    w.flag(0);  // weighted pred
    w.flag(0);  // weighted bipred
    w.flag(0);  // transquant bypass
    w.flag(0);  // tiles
    w.flag(0);  // entropy coding sync
    w.flag(1);  // loop filter across slices
    w.flag(1);  // deblocking control present
    w.flag(0);  //   override enabled
    w.flag(0);  //   deblocking disabled
    w.se(0);    //   beta offset / 2
    w.se(0);    //   tc offset / 2
    w.flag(0);  // scaling list data
    w.flag(0);  // lists modification
    w.ue(0);    // parallel merge level - 2
    w.flag(0);  // slice header extension
    w.flag(0);  // extension
    w.trailing();
    return nal(Pps, w);
}

std::vector<uint8_t> sliceHeader(const StreamParams& p, bool idr, int poc, std::vector<RefPic> refs)
{
    Bits w;
    w.flag(1); // first slice segment
    if (idr) w.flag(0); // no_output_of_prior_pics
    w.ue(0);   // pps id
    w.ue(idr ? 2 : 1); // I or P
    if (!idr) {
        w.u(p.log2MaxPocLsb, uint32_t(poc) & ((1u << p.log2MaxPocLsb) - 1));
        w.flag(0); // st_ref_pic_set in the header
        // st_ref_pic_set(num_short_term_ref_pic_sets = 0): explicit, all
        // before the current picture, nearest first.
        std::sort(refs.begin(), refs.end(), [](const RefPic& a, const RefPic& b) { return a.poc > b.poc; });
        w.ue(uint32_t(refs.size()));
        w.ue(0);
        int prev = poc;
        for (const RefPic& r : refs) {
            w.ue(uint32_t(prev - r.poc - 1));
            w.flag(r.used);
            prev = r.poc;
        }
    }
    w.flag(1); // SAO luma
    w.flag(1); // SAO chroma
    if (!idr) {
        w.flag(0); // num_ref_idx_active_override: one reference
        w.flag(0); // cabac_init_flag
        w.ue(0);   // five_minus_max_num_merge_cand
    }
    w.se(0);   // slice QP delta (rate control sets QP per CU)
    w.flag(1); // loop filter across slices
    w.trailing();
    return nal(idr ? IdrWRadl : TrailR, w);
}

} // namespace p2p::video::hevc
