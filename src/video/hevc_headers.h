#pragma once

#include "video/video_frame.h"

#include <cstdint>
#include <vector>

// HEVC parameter sets and slice headers for an encoder that takes them from
// the application (VA-API packed headers). One P-only, low-delay stream:
// one slice per picture, explicit RPS in every slice header (so the encoder
// decides which earlier pictures stay referenceable), no TMVP.
// Each function returns one NAL unit: 4-byte start code, header, RBSP with
// emulation prevention.
namespace p2p::video::hevc {

struct StreamParams {
    int width = 0, height = 0; // the picture; coded at multiples of alignW x alignH, the rest cropped
    int alignW = 16, alignH = 16; // the encoder's coded-size alignment (multiples of 8)
    int fps = 60;
    Colour colour = Colour::Bt709Limited;
    int maxDpb = 5; // pictures the decoder must hold, current one included
    int log2MaxPocLsb = 16;
    bool constrainedIntraPred = false;

    int codedWidth() const { return (width + alignW - 1) / alignW * alignW; }
    int codedHeight() const { return (height + alignH - 1) / alignH * alignH; }
    int level() const; // general_level_idc (30 x level)
};

// A picture kept in the RPS; |used| = a reference of the current picture.
struct RefPic {
    int poc = 0;
    bool used = false;
};

std::vector<uint8_t> vps(const StreamParams& p);
std::vector<uint8_t> sps(const StreamParams& p);
std::vector<uint8_t> pps(const StreamParams& p);
// IDR (I slice, POC 0) or P slice at |poc| keeping |refs| (any order).
std::vector<uint8_t> sliceHeader(const StreamParams& p, bool idr, int poc, std::vector<RefPic> refs);

} // namespace p2p::video::hevc
