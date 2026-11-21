#pragma once

#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;
struct OpusDREDDecoder;
struct OpusDRED;

namespace p2p::audio {

// Opus encoder tuned for interactive speech:
//  * 10 ms frames (5 ms optional), complexity 9
//  * VOIP mode (SILK/hybrid -> LBRR in-band FEC available) or
//    restricted-low-delay (CELT only, 2.5 ms lookahead, no LBRR)
//  * DRED (Opus 1.5 deep redundancy) for burst-loss recovery
class OpusEncoderWrapper {
public:
    struct Config {
        int sampleRate = 48000;
        int channels = 1;
        int bitrate = 48000;
        bool restrictedLowDelay = false;
        int expectedLossPct = 10;   // drives LBRR/DRED usage
        int dredFrames = 10;        // 10 ms units; 0 disables DRED
    };
    ~OpusEncoderWrapper();
    bool init(const Config& cfg);
    // Returns encoded size, or <0 on error.
    int encode(const int16_t* pcm, int frameSamples, uint8_t* out, int maxBytes);
    void setBitrate(int bps);
    void setExpectedLoss(int pct);
    bool dredEnabled() const { return dredEnabled_; }
    int lookaheadSamples() const { return lookahead_; }

private:
    OpusEncoder* enc_ = nullptr;
    bool dredEnabled_ = false;
    int lookahead_ = 0;
};

class OpusDecoderWrapper {
public:
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
