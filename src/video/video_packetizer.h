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
//   u32 frame id | u16 fragment index | u16 data fragment count |
//   u64 capture time (sender us) | u8 flags | u8 codec |
//   u16 display width | u16 display height |
//   u8 FEC parity per block | u8 FEC block size
//
// The display size is carried because some encoders (Mesa VA-API HEVC)
// pad the coded size, e.g. 1080 -> 1088, without a conformance window.
//
// FEC: data fragments are grouped into blocks of |fecBlock| consecutive
// fragments; each block gets |fecParity| Reed-Solomon parity fragments,
// with indices count + block * fecParity + j. A parity fragment protects
// the symbols [u16 data length | data zero-padded], so a rebuilt fragment
// knows its own length.
//
// The whole frame is sent as one burst (no pacing on a LAN).
struct FragmentHeader {
    static constexpr size_t kSize = 26;
    // kTestPattern: the sender's source is the synthetic pattern, so the
    // receiver can verify the frame number embedded in the pixels.
    // kRetransmit: a resend in answer to a NACK.
    enum Flags : uint8_t { kKeyframe = 1, kTestPattern = 2, kRetransmit = 4 };

    uint32_t frameId = 0;
    uint16_t index = 0;
    uint16_t count = 0;
    int64_t captureUs = 0;
    uint8_t flags = 0;
    uint8_t codec = 0;
    uint16_t width = 0, height = 0; // 0 = unknown
    uint8_t fecParity = 0;          // parity fragments per block, 0 = no FEC
    uint8_t fecBlock = 0;
    // Capture -> encoder output on the sender, in 10 us units (latency
    // breakdown); saturates at 0xffff.
    uint16_t encodeDelay10Us = 0;

    bool isParity() const { return index >= count; }
    int blocks() const { return fecParity ? (count + fecBlock - 1) / fecBlock : 0; }
    void write(uint8_t* p) const;
    static std::optional<FragmentHeader> read(const uint8_t* p, size_t len);
};

struct PacketizeOptions {
    uint8_t extraFlags = 0;
    uint16_t width = 0, height = 0;
    // Parity fragments as a fraction of data fragments (0 = off). Each
    // block gets ceil(blockSize * fecRate) parity fragments.
    double fecRate = 0.0;
    int fecBlockSize = 32;
    int64_t encodedUs = 0; // when the encoder produced the frame (0 = unknown), sender clock
};

// Calls |emit| once per fragment with a complete payload (header + data):
// data fragments first, then parity. Returns the number of data
// fragments, or 0 if the frame is too large.
size_t packetizeFrame(uint32_t frameId, int64_t captureUs, bool keyframe, uint8_t codec, const uint8_t* data,
                      size_t size, size_t maxPayload,
                      const std::function<void(const uint8_t* payload, size_t len)>& emit,
                      const PacketizeOptions& opt = {});

struct AssembledFrame {
    uint32_t id = 0;
    bool keyframe = false;
    uint8_t flags = 0;
    uint8_t codec = 0;
    uint16_t width = 0, height = 0;
    int64_t captureUs = 0;
    int64_t encodedUs = 0; // sender clock, 0 = unknown
    int64_t firstArrivalUs = 0;
    int64_t completeUs = 0;
    bool recovered = false; // needed FEC or retransmission
    std::vector<uint8_t> data;
};

// Retransmission request: one data fragment, or kWholeFrame for a frame
// of which nothing arrived.
struct NackItem {
    static constexpr uint16_t kWholeFrame = 0xffff;
    uint32_t frameId = 0;
    uint16_t index = 0;
};

// Rebuilds frames from fragments and releases them strictly in decode
// order. Video is rendered as soon as it is decodable, so there is no
// playout delay. Missing fragments are first rebuilt from FEC, then
// requested again (collectNacks); only once the grace period runs out is
// a frame declared lost, needKeyframe() turns true, and the caller sends a
// picture-loss indication.
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
        uint64_t parityReceived = 0, fecRecoveredFragments = 0, fecRecoveredFrames = 0;
        uint64_t retransmitsReceived = 0, retransmitsUseful = 0, nacked = 0;
        uint64_t highestPacketCount = 0; // data + parity fragments expected, for loss stats
    };
    struct NackPolicy {
        int64_t reorderUs = 2'000; // quiet time before a hole counts as lost
        int64_t retryUs = 10'000;  // gap between repeat requests for one fragment
        int maxTries = 3;
    };

    FrameAssembler() : FrameAssembler(Config{}) {}
    explicit FrameAssembler(Config cfg) : cfg_(cfg) {}

    void setReorderGraceUs(int64_t us) { cfg_.reorderGraceUs = us; }
    int64_t reorderGraceUs() const { return cfg_.reorderGraceUs; }

    void insert(const FragmentHeader& h, const uint8_t* data, size_t len, int64_t arrivalUs);
    // Frames that can be decoded now, in order.
    std::vector<AssembledFrame> poll(int64_t nowUs);
    // Fragments worth requesting again now (FEC can't cover them).
    std::vector<NackItem> collectNacks(int64_t nowUs, const NackPolicy& policy);

    bool needKeyframe() const { return !haveNext_; }
    const Stats& stats() const { return st_; }

private:
    struct Retry {
        int tries = 0;
        int64_t lastUs = 0;
    };
    struct Partial {
        FragmentHeader first; // header of the first fragment seen
        uint16_t received = 0;
        int64_t firstArrivalUs = 0;
        int64_t lastArrivalUs = 0;
        bool recovered = false;
        size_t symbolLen = 0; // parity payload size, 0 = no parity seen yet
        std::vector<std::vector<uint8_t>> chunks;
        std::vector<std::vector<uint8_t>> parity;
        std::vector<Retry> retries;
    };
    struct MissingFrame {
        int64_t sinceUs = 0;
        Retry retry;
    };
    void tryRecover(Partial& p, int block);
    void complete(uint32_t id, Partial&& p, int64_t nowUs);
    void dropBefore(uint32_t id);
    bool known(uint32_t id) const { return partial_.count(id) || ready_.count(id); }

    Config cfg_;
    std::map<uint32_t, Partial> partial_;
    std::map<uint32_t, AssembledFrame> ready_;
    std::map<uint32_t, MissingFrame> missing_; // frames with no fragment at all, inside the gap
    bool haveNext_ = false;       // false: waiting for a keyframe
    bool haveFloor_ = false;      // nextId_ is a floor while resyncing: older frames are history
    uint32_t nextId_ = 0;
    bool haveHighest_ = false;
    uint32_t highestId_ = 0;      // newest frame id seen
    int64_t gapSinceUs_ = -1;     // when a frame newer than nextId_ first showed up
    bool haveLastFragment_ = false;
    uint32_t lastFragmentFrame_ = 0;
    int64_t lastFragmentUs_ = 0;
    int64_t fragmentGapUs_ = 0;   // EWMA of the gap between fragments of one frame
    Stats st_;
};

} // namespace p2p::video
