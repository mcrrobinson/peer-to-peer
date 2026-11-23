#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

// Sliding-window FEC across frames (in the spirit of Tambur's streaming
// codes, without its learned overhead model): each parity packet is a
// random linear combination over GF(2^8) of every data packet -- whole
// payloads, fragment header included -- of the last few frames. Unlike
// per-frame Reed-Solomon, the overhead can be a fraction of a packet per
// frame (small frames on slow links), and a burst that wipes out a frame
// is repaired from the parity of the frames after it, without a round trip.
//
// Parity packet payload: a FragmentHeader (kWindowParity, frame id = the
// newest frame covered, index 0, count 1, capture time of that frame,
// encode delay field = capture span of the frames covered), then
//   u8 frames covered n | u16 data packet count per frame (oldest first) |
//   u32 seed | u16 symbol length | symbol
// Frames covered are the n consecutive ids ending at the header's id. A
// symbol is [u16 packet length | packet, zero-padded]; the coefficient of
// packet (frame, index) comes from (seed, frame, index), never zero.
namespace p2p::video {

class WindowFecEncoder {
public:
    // Window: frames each parity packet covers (the current one included).
    explicit WindowFecEncoder(int windowFrames = 4) : window_(windowFrames) {}
    void setWindow(int frames) { window_ = frames < 1 ? 1 : frames; }
    int window() const { return window_; }

    // A data packet of frame |frameId| as sent (full payload). Frame ids are
    // consecutive; a new id starts a new frame.
    void add(uint32_t frameId, const uint8_t* payload, size_t len);
    // Frame |frameId| is complete: parity packets for it, about |rate| x
    // its data packets (fractions carry over to the next frames).
    std::vector<std::vector<uint8_t>> endFrame(uint32_t frameId, double rate, size_t maxPayload);
    // Bytes the parity framing adds on top of a data packet: data packets
    // must leave this much room under the payload limit.
    static size_t overhead(int windowFrames);

private:
    struct Frame {
        uint32_t id = 0;
        int64_t captureUs = 0;
        std::vector<std::vector<uint8_t>> packets;
    };
    int window_;
    std::deque<Frame> frames_;
    double credit_ = 0;
    uint32_t seed_ = 0x9e3779b9u;
};

class WindowFecDecoder {
public:
    // A data packet arrived (full payload; a resend's kRetransmit flag is
    // ignored). Returns packets this let us rebuild.
    std::vector<std::vector<uint8_t>> onData(const uint8_t* payload, size_t len);
    // A parity packet arrived. Returns packets rebuilt.
    std::vector<std::vector<uint8_t>> onParity(const uint8_t* payload, size_t len);

    uint64_t recovered() const { return recovered_; }
    // Capture span of the frames the last parity covered: a frame's repair
    // can come this much later than the frame itself.
    int64_t spanUs() const { return spanUs_; }

private:
    struct Eq {
        uint32_t newest = 0;
        std::map<uint64_t, uint8_t> unknown; // packet key -> coefficient
        std::vector<uint8_t> rhs;            // parity minus the known packets
    };
    void learn(uint64_t key, std::vector<uint8_t> symbol, std::vector<std::vector<uint8_t>>& out);
    void solve(std::vector<std::vector<uint8_t>>& out);
    void prune(uint32_t newest);

    std::map<uint64_t, std::vector<uint8_t>> known_; // key -> symbol, recent frames
    std::vector<Eq> eqs_;
    bool haveNewest_ = false;
    uint32_t newest_ = 0;
    uint64_t recovered_ = 0;
    int64_t spanUs_ = 0;
};

} // namespace p2p::video
