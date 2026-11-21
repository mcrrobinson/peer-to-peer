#include "audio/audio_io.h"

#include <QLoggingCategory>

#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

Q_LOGGING_CATEGORY(lcDev, "p2p.audiodev")

namespace p2p::audio {

namespace {

class MiniaudioIO : public AudioIO {
public:
    explicit MiniaudioIO(int periodMs) : periodMs_(periodMs) {}
    ~MiniaudioIO() override { stop(); }

    bool start(Callback cb, QString* error) override
    {
        cb_ = std::move(cb);
        ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
        cfg.sampleRate = 48000;
        cfg.capture.format = ma_format_s16;
        cfg.capture.channels = 1;
        cfg.playback.format = ma_format_s16;
        cfg.playback.channels = 1;
        cfg.periodSizeInMilliseconds = ma_uint32(periodMs_);
        cfg.periods = 2;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.noPreSilencedOutputBuffer = MA_TRUE;
        cfg.dataCallback = &MiniaudioIO::dataCallback;
        cfg.pUserData = this;

        if (ma_device_init(nullptr, &cfg, &dev_) != MA_SUCCESS) {
            if (error) *error = QStringLiteral("could not open audio devices");
            return false;
        }
        inited_ = true;
        if (ma_device_start(&dev_) != MA_SUCCESS) {
            if (error) *error = QStringLiteral("could not start audio devices");
            stop();
            return false;
        }
        qCInfo(lcDev).noquote() << description();
        return true;
    }

    void stop() override
    {
        if (inited_) {
            ma_device_uninit(&dev_);
            inited_ = false;
        }
    }

    bool hasCapture() const override { return true; }

    QString description() const override
    {
        if (!inited_) return QStringLiteral("audio device (closed)");
        return QStringLiteral("%1: in \"%2\" / out \"%3\", %4 Hz, period %5 frames x %6")
            .arg(QString::fromUtf8(ma_get_backend_name(dev_.pContext->backend)),
                 QString::fromUtf8(dev_.capture.name), QString::fromUtf8(dev_.playback.name))
            .arg(dev_.sampleRate)
            .arg(dev_.playback.internalPeriodSizeInFrames)
            .arg(dev_.playback.internalPeriods);
    }

    int deviceLatencyMs() const override
    {
        if (!inited_) return 0;
        const auto frames = dev_.playback.internalPeriodSizeInFrames * dev_.playback.internalPeriods +
                            dev_.capture.internalPeriodSizeInFrames;
        const auto rate = dev_.playback.internalSampleRate ? dev_.playback.internalSampleRate : 48000;
        return int(frames * 1000 / rate);
    }

private:
    static void dataCallback(ma_device* d, void* out, const void* in, ma_uint32 frames)
    {
        auto* self = static_cast<MiniaudioIO*>(d->pUserData);
        self->cb_(static_cast<const int16_t*>(in), static_cast<int16_t*>(out), int(frames));
    }

    int periodMs_;
    ma_device dev_{};
    bool inited_ = false;
    Callback cb_;
};

} // namespace

std::unique_ptr<AudioIO> createDeviceAudioIO(int periodMs)
{
    return std::make_unique<MiniaudioIO>(periodMs);
}

} // namespace p2p::audio
