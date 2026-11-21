#pragma once

#include "audio/audio_io.h"
#include "audio/audio_processor.h"
#include "audio/drift_resampler.h"
#include "audio/jitter_buffer.h"
#include "audio/opus_codec.h"

#include <QString>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace p2p::net {
class MediaTransport;
}

namespace p2p::audio {

// Capture -> (AEC/NS/AGC) -> Opus -> transport   and
// transport -> jitter buffer -> Opus (+FEC/DRED/PLC) -> playout.
//
// Everything audio-side runs on the device callback thread; the transport
// receive thread only inserts into the jitter buffer.
//
// Audio payload (inside the encrypted transport packet):
//   u32 seq | u32 timestamp (samples) | u64 capture time (sender us) | opus
class AudioPipeline {
public:
    enum class Source { Mic, Tone, Silence, Echo };
    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSamples = 480; // 10 ms
    static constexpr int kToneHz = 500;       // 5 whole cycles per 10 ms frame

    struct Config {
        Source source = Source::Mic;
        bool useDevice = true;     // false: simulated clock, playback discarded
        // WebRTC APM on the mic path (each stage independently).
        bool echoCancel = false;      // off by default: buggy in testing, revisit later
        bool noiseSuppress = false;   // off by default: it chopped speech in testing
        bool autoGain = false;        // off by default: made speech sound uneven in testing
        int noiseLevel = 1;           // 0 low, 1 moderate, 2 high
        int periodMs = 10;
        OpusEncoderWrapper::Config opus;
    };

    struct Stats {
        uint64_t framesSent = 0, bytesSent = 0;
        JitterBuffer::Stats jb;
        int targetMs = 0, levelMs = 0;
        double jitterP95Ms = 0;
        uint64_t concealDred = 0, concealFec = 0, concealPlc = 0;
        double latencyAvgMs = -1, latencyP95Ms = -1; // capture -> playout hand-off (excl. device buffers)
        int deviceLatencyMs = 0;
        double outputRmsDb = -120;
        double toneFraction = 0;   // share of output energy at kToneHz (tone-source verification)
        // Device callback timing (diagnoses bursty audio servers).
        uint64_t callbacks = 0;
        int minCallbackFrames = 0, maxCallbackFrames = 0;
        double maxCallbackGapMs = 0;
        uint64_t lateCallbacks = 0;  // gap > 1.5x the nominal period
        double driftPpm = 0;          // current playout rate correction
        bool processingActive = false, dred = false;
        bool echoCancel = false, noiseSuppress = false, autoGain = false;
        QString device;
    };

    AudioPipeline(Config cfg, net::MediaTransport& transport);
    ~AudioPipeline();

    // Registers the transport handler; call before transport.start().
    void attach();
    bool start(QString* error);
    void stop();

    void setMuted(bool m) { muted_ = m; }
    void setClockOffsetUs(int64_t us) { clockOffsetUs_ = us; }
    // Live AEC / NS / AGC switching (no-op without a real microphone).
    void setProcessing(bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel)
    {
        apm_.setOptions(echoCancel, noiseSuppress, autoGain, noiseLevel);
    }

    Stats stats() const;
    static QString format(const Stats& s);

private:
    void onPacket(const uint8_t* data, size_t len, int64_t arrivalUs);
    void process(const int16_t* in, int16_t* out, int frames);
    void decodeOne(int64_t nowUs);
    void encodeAndSend();
    void analyse(const int16_t* pcm, int n);
    double playoutRatio();

    Config cfg_;
    net::MediaTransport& transport_;
    std::unique_ptr<AudioIO> io_;
    OpusEncoderWrapper enc_;
    OpusDecoderWrapper dec_;
    AudioProcessor apm_;

    // receive side
    mutable std::mutex jbMutex_;
    JitterBuffer jb_;

    // audio-thread state
    DriftResampler playout_;          // decoded audio, drift-compensated on the way out
    double smoothedBufferedSamples_ = 0;
    int16_t capFrame_[kFrameSamples]{};
    int capCount_ = 0;
    int64_t capStartUs_ = 0;
    uint32_t seq_ = 0, ts_ = 0;
    int tonePhase_ = 0;
    double sumSq_ = 0, toneSumSq_ = 0;
    int analysedSamples_ = 0;

    std::atomic<uint64_t> callbacks_{0}, lateCallbacks_{0};
    std::atomic<int> minCbFrames_{1 << 30}, maxCbFrames_{0};
    std::atomic<int64_t> maxGapUs_{0};
    std::atomic<double> ratio_{1.0};
    int64_t lastCallbackUs_ = 0;

    std::atomic<bool> muted_{false};
    std::atomic<int64_t> clockOffsetUs_{0};
    std::atomic<uint64_t> framesSent_{0}, bytesSent_{0};
    std::atomic<uint64_t> concealDred_{0}, concealFec_{0}, concealPlc_{0};

    mutable std::mutex statsMutex_;
    std::deque<double> latencies_;
    double outputRmsDb_ = -120, toneFraction_ = 0;
};

} // namespace p2p::audio
