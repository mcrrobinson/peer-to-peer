#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace p2p::audio {

struct EncodedFrame {
    uint32_t seq = 0;
    uint32_t timestamp = 0;   // in samples
    int64_t captureUs = 0;    // sender clock
    int64_t arrivalUs = 0;    // receiver clock
    std::vector<uint8_t> data;
};

// Adaptive audio jitter buffer, tuned for low delay.
//
// The playout side pulls one frame per frame interval. The target depth is
// derived from the p95 of packet delay variation over a ~2 s window (plus a
// small margin), so on a quiet LAN it settles at a single frame. A frame
// that was waited for and then arrived late temporarily raises the target
// (a lost one does not); sustained excess depth is trimmed by dropping a
// frame, and a gross backlog (at start, or after a stall) is discarded at
// once down to the newest target's worth. When a frame is missing but a later one has arrived, the
// caller is told to conceal it and is handed that later frame so it can use
// Opus in-band FEC (LBRR) or DRED before falling back to PLC.
//
// Not thread-safe; the owner serialises insert()/pull().
class JitterBuffer {
public:
    struct Config {
        int sampleRate = 48000;
        int frameSamples = 480;
        int minTargetFrames = 1;
        int maxTargetFrames = 25;
        size_t windowPackets = 200;
        int marginUs = 2000;
        // Normal drift/burst draining is done smoothly by the playout
        // resampler; hard frame drops only kick in for gross excess.
        int hardTrimExcessFrames = 6;
        int excessPullsBeforeDrop = 20;   // ~200 ms above that before trimming
        // Beyond this excess, skip straight to the newest target's worth
        // instead of draining (which takes ~10 s per second of backlog).
        int flushExcessFrames = 20;
        int pullsPerBoostDecay = 500;     // ~5 s without underrun lowers the boost
        int maxBoost = 6;
    };

    enum class Action {
        Silence,  // not started yet
        Decode,   // frame holds the packet to decode
        Conceal,  // frame (if non-empty) is the next available packet, |distance| frames ahead
        Underrun, // nothing to play; conceal without advancing
    };

    struct Result {
        Action action = Action::Silence;
        EncodedFrame frame;
        int distance = 0;
    };

    struct Stats {
        uint64_t received = 0, decoded = 0, concealed = 0, underruns = 0;
        uint64_t late = 0, duplicates = 0, dropped = 0, resets = 0, lateAfterWait = 0;
    };

    JitterBuffer() : JitterBuffer(Config{}) {}
    explicit JitterBuffer(Config cfg);

    void insert(EncodedFrame f);
    Result pull();

    int targetFrames() const;
    int levelFrames() const;
    double jitterP95Ms() const { return jitterP95Us_ / 1000.0; }
    const Stats& stats() const { return st_; }

private:
    void updateJitter(const EncodedFrame& f);
    void discardTo(uint32_t seq);

    Config cfg_;
    std::map<uint32_t, EncodedFrame> q_;
    bool havePlayhead_ = false;
    bool started_ = false;
    uint32_t playhead_ = 0;
    uint32_t maxSeq_ = 0;

    std::deque<int64_t> transits_;
    int64_t jitterP95Us_ = 0;
    int baseTarget_ = 1;
    int boost_ = 0;
    int pullsSinceUnderrun_ = 0;
    int excessPulls_ = 0;
    // An underrun holds the playhead. Only if the awaited frame then turns
    // up (i.e. it was late, not lost) do we raise the target.
    bool underrunPending_ = false;
    uint32_t underrunSeq_ = 0;
    Stats st_;
};

} // namespace p2p::audio
