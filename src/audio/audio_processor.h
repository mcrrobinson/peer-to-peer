#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace p2p::audio {

// WebRTC AudioProcessing (AEC3 echo cancellation, noise suppression,
// high-pass, adaptive digital gain). Works on 10 ms frames. Capture and
// render calls may come from different threads. Compiled as a pass-through
// when webrtc-audio-processing is unavailable (e.g. macOS, which will use
// the system Voice-Processing I/O unit instead).
class AudioProcessor {
public:
    AudioProcessor();
    ~AudioProcessor();

    static bool available();
    // noiseLevel: 0 low, 1 moderate, 2 high
    bool init(int sampleRate, bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel = 1);
    // Re-configure while audio is running (APM locks internally).
    void setOptions(bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel);
    // True when APM exists and at least one stage is enabled.
    bool active() const { return impl_ != nullptr && (echoCancel_ || noiseSuppress_ || autoGain_); }
    bool echoCancel() const { return echoCancel_; }
    bool noiseSuppress() const { return noiseSuppress_; }
    bool autoGain() const { return autoGain_; }

    void processRender(const int16_t* frame);   // far-end audio about to be played
    void processCapture(int16_t* frame);        // near-end mic audio, in place
    void setStreamDelayMs(int ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> echoCancel_{false}, noiseSuppress_{false}, autoGain_{false};
    std::atomic<int> noiseLevel_{1};
};

} // namespace p2p::audio
