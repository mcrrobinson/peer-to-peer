#pragma once

#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;
struct OpusDREDDecoder;
struct OpusDRED;

namespace p2p::audio {

// DRED bitstream revision this binary can send and parse, or 0 when libopus
// was built without DRED. libopus versions DRED separately from its own
// version (DRED_EXPERIMENTAL_VERSION: 10 in 1.5.x, 12 in 1.6.x); a decoder
// silently ignores DRED of another revision, so peers only use DRED when
// both report the same value.
int dredRevision();

// Loss protection carried inside the Opus stream. One or the other: with
// both on, libopus gives DRED no bits at 10 ms frames, but still runs the
// DRED encoder network on every frame.
enum class LossProtection {
    Lbrr, // in-band FEC: rebuilds one lost frame at full quality, no added cost when nothing is lost
    Dred, // deep redundancy: recovers bursts (up to ~1 s of history), takes a large share of the bitrate
};

// Opus encoder tuned for interactive speech:
//  * 10 ms frames (5 ms optional), complexity 9
//  * VOIP mode (SILK/hybrid -> LBRR in-band FEC available) or
//    restricted-low-delay (CELT only, 2.5 ms lookahead, no LBRR)
//  * LBRR or DRED for loss recovery (see LossProtection)
class OpusEncoderWrapper {
public:
    struct Config {
        int sampleRate = 48000;
        int channels = 1;
        int bitrate = 48000;
        bool restrictedLowDelay = false;
        int expectedLossPct = 10;   // LBRR strength; with DRED, also DRED's share of the bitrate
        LossProtection protection = LossProtection::Lbrr;
        int dredFrames = 100;       // DRED duration request in 10 ms units (DRED mode only)
    };
    ~OpusEncoderWrapper();
    bool init(const Config& cfg);
    // Returns encoded size, or <0 on error.
    int encode(const int16_t* pcm, int frameSamples, uint8_t* out, int maxBytes);
    void setBitrate(int bps);
    void setExpectedLoss(int pct);
    bool dredEnabled() const { return dredEnabled_; }
    bool lbrrEnabled() const { return lbrrEnabled_; }
    int lookaheadSamples() const { return lookahead_; }

private:
    OpusEncoder* enc_ = nullptr;
    bool dredEnabled_ = false, lbrrEnabled_ = false;
    int lookahead_ = 0;
};

class OpusDecoderWrapper {
public:
    // Complexity 5 enables the neural (FARGAN) concealment for losses that
    // DRED and LBRR can't cover. Higher levels only select LACE/NoLACE,
    // which don't run on 10 ms frames.
    static constexpr int kComplexity = 5;

    ~OpusDecoderWrapper();
    bool init(int sampleRate, int channels);

    int decode(const uint8_t* data, int len, int16_t* pcm, int frameSamples);
    // Reconstruct the frame |distance| frames before |next| using DRED,
    // falling back to LBRR (distance == 1) and then PLC. Reports which
    // method was used.
    enum class Method { Dred, Fec, Plc };
    int conceal(const uint8_t* next, int nextLen, int distance, int16_t* pcm, int frameSamples, Method* used);
    int plc(int16_t* pcm, int frameSamples);

private:
    OpusDecoder* dec_ = nullptr;
    OpusDREDDecoder* dredDec_ = nullptr;
    OpusDRED* dred_ = nullptr;
    int sampleRate_ = 48000;
};

} // namespace p2p::audio
