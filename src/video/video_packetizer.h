#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace p2p::video {

// Encoded video frames are split into fragments that each fit one media
// datagram. Every fragment carries a small header (inside the encrypted
// payload):
//
//   u32 frame id | u16 fragment index | u16 fragment count |
//   u64 capture time (sender us) | u8 flags | u8 codec |
//   u16 display width | u16 display height | u16 reserved
//
// The display size is carried because some encoders (Mesa VA-API HEVC)
// pad the coded size, e.g. 1080 -> 1088, without a conformance window.
//
// The whole frame is sent as one burst (no pacing on a LAN).
struct FragmentHeader {
    static constexpr size_t kSize = 24;
    // kTestPattern: the sender's source is the synthetic pattern, so the
    // receiver can verify the frame number embedded in the pixels.
    enum Flags : uint8_t { kKeyframe = 1, kTestPattern = 2 };

    uint32_t frameId = 0;
    uint16_t index = 0;
    uint16_t count = 0;
    int64_t captureUs = 0;
    uint8_t flags = 0;
    uint8_t codec = 0;
    uint16_t width = 0, height = 0; // 0 = unknown

    void write(uint8_t* p) const;
    static std::optional<FragmentHeader> read(const uint8_t* p, size_t len);
};

// Calls |emit| once per fragment with a complete payload (header + data).
// Returns the number of fragments, or 0 if the frame is too large.
size_t packetizeFrame(uint32_t frameId, int64_t captureUs, bool keyframe, uint8_t codec, const uint8_t* data,
                      size_t size, size_t maxPayload,
                      const std::function<void(const uint8_t* payload, size_t len)>& emit,
                      uint8_t extraFlags = 0, uint16_t width = 0, uint16_t height = 0);

struct AssembledFrame {
    uint32_t id = 0;
    bool keyframe = false;
    uint8_t flags = 0;
    uint8_t codec = 0;
    uint16_t width = 0, height = 0;
    int64_t captureUs = 0;
    int64_t firstArrivalUs = 0;
    int64_t completeUs = 0;
    std::vector<uint8_t> data;
};

// Rebuilds frames from fragments and releases them strictly in decode
// order. Video is rendered as soon as it is decodable, so there is no
// playout delay — only a short reorder grace period before a missing frame
// is declared lost. Until NACK/LTR recovery lands (phase 5), a lost frame
// means waiting for a keyframe: needKeyframe() turns true and the caller
// sends a picture-loss indication.
//
// Not thread-safe; the owner serialises calls.
class FrameAssembler {
public:
    struct Config {
        int64_t reorderGraceUs = 20'000; // how long a gap may persist before the frame is lost
        size_t maxPartialFrames = 64;
        size_t maxFrameBytes = 16 << 20;
    };
    struct Stats {
        uint64_t fragments = 0, framesComplete = 0, framesReleased = 0;
        uint64_t framesLost = 0, framesSkipped = 0, lateFragments = 0, malformed = 0;
    };

    FrameAssembler() : FrameAssembler(Config{}) {}
    explicit FrameAssembler(Config cfg) : cfg_(cfg) {}

    void insert(const FragmentHeader& h, const uint8_t* data, size_t len, int64_t arrivalUs);
    // Frames that can be decoded now, in order.
    std::vector<AssembledFrame> poll(int64_t nowUs);

    bool needKeyframe() const { return !haveNext_; }
    const Stats& stats() const { return st_; }

private:
    struct Partial {
        uint16_t count = 0;
        uint16_t received = 0;
        bool keyframe = false;
        uint8_t flags = 0;
        uint8_t codec = 0;
        uint16_t width = 0, height = 0;
        int64_t captureUs = 0;
        int64_t firstArrivalUs = 0;
        std::vector<std::vector<uint8_t>> chunks;
    };
    void complete(uint32_t id, Partial&& p, int64_t nowUs);
    void dropBefore(uint32_t id);

    Config cfg_;
    std::map<uint32_t, Partial> partial_;
    std::map<uint32_t, AssembledFrame> ready_;
    bool haveNext_ = false;       // false: waiting for a keyframe
    uint32_t nextId_ = 0;
    int64_t gapSinceUs_ = -1;     // when a frame newer than nextId_ first showed up
    Stats st_;
};

} // namespace p2p::video
