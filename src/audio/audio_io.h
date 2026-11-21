#pragma once

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace p2p::audio {

// Full-duplex audio I/O at 48 kHz mono s16. A single callback receives the
// captured block and fills the playback block, which keeps the echo
// canceller's render and capture streams aligned.
class AudioIO {
public:
    // |in| may be null when there is no capture device.
    using Callback = std::function<void(const int16_t* in, int16_t* out, int frames)>;
    virtual ~AudioIO() = default;
    virtual bool start(Callback cb, QString* error) = 0;
    virtual void stop() = 0;
    virtual bool hasCapture() const = 0;
    virtual QString description() const = 0;
    // Best estimate of device buffering (capture + playback), for AEC and stats.
    virtual int deviceLatencyMs() const = 0;
};

// Real devices via miniaudio (PipeWire/Pulse/ALSA on Linux, CoreAudio on macOS).
std::unique_ptr<AudioIO> createDeviceAudioIO(int periodMs);

// Clocked by a thread instead of hardware: no capture, playback discarded.
// Used for headless tests and as a "null sink".
class SimulatedAudioIO : public AudioIO {
public:
    explicit SimulatedAudioIO(int periodMs = 10) : periodMs_(periodMs) {}
    ~SimulatedAudioIO() override { stop(); }
    bool start(Callback cb, QString* error) override;
    void stop() override;
    bool hasCapture() const override { return false; }
    QString description() const override { return QStringLiteral("simulated (null) device"); }
    int deviceLatencyMs() const override { return 0; }

private:
    int periodMs_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace p2p::audio
