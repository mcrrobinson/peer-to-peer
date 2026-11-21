#include "audio/audio_io.h"

namespace p2p::audio {

namespace {
class NoDeviceIO : public AudioIO {
public:
    bool start(Callback, QString* error) override
    {
        if (error) *error = QStringLiteral("built without miniaudio: no audio devices (use --audio-io null)");
        return false;
    }
    void stop() override {}
    bool hasCapture() const override { return false; }
    QString description() const override { return QStringLiteral("no audio backend"); }
    int deviceLatencyMs() const override { return 0; }
};
} // namespace

std::unique_ptr<AudioIO> createDeviceAudioIO(int) { return std::make_unique<NoDeviceIO>(); }

} // namespace p2p::audio
