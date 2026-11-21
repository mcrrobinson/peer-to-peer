#include "audio/audio_processor.h"

#ifdef P2P_HAVE_WEBRTC_APM
#include <modules/audio_processing/include/audio_processing.h>
#endif

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcApm, "p2p.apm")

namespace p2p::audio {

#ifdef P2P_HAVE_WEBRTC_APM

struct AudioProcessor::Impl {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig cfg;
    int delayMs = 0;
};

AudioProcessor::AudioProcessor() = default;
AudioProcessor::~AudioProcessor() = default;

bool AudioProcessor::available() { return true; }

namespace {
webrtc::AudioProcessing::Config makeConfig(bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel)
{
    webrtc::AudioProcessing::Config c;
    c.echo_canceller.enabled = echoCancel;
    c.echo_canceller.mobile_mode = false;
    c.noise_suppression.enabled = noiseSuppress;
    using NS = webrtc::AudioProcessing::Config::NoiseSuppression;
    c.noise_suppression.level = noiseLevel <= 0 ? NS::kLow : noiseLevel >= 2 ? NS::kHigh : NS::kModerate;
    c.high_pass_filter.enabled = echoCancel || noiseSuppress || autoGain;
    c.gain_controller2.enabled = autoGain;
    c.gain_controller2.adaptive_digital.enabled = autoGain;
    return c;
}
} // namespace

bool AudioProcessor::init(int sampleRate, bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel)
{
    auto impl = std::make_unique<Impl>();
    impl->apm = rtc::scoped_refptr<webrtc::AudioProcessing>(webrtc::AudioProcessingBuilder().Create());
    if (!impl->apm) return false;
    impl->apm->ApplyConfig(makeConfig(echoCancel, noiseSuppress, autoGain, noiseLevel));
    impl->apm->Initialize();
    impl->cfg = webrtc::StreamConfig(sampleRate, 1);
    impl_ = std::move(impl);
    echoCancel_ = echoCancel;
    noiseSuppress_ = noiseSuppress;
    autoGain_ = autoGain;
    noiseLevel_ = noiseLevel;
    qCInfo(lcApm) << "WebRTC APM: AEC3" << echoCancel << "NS" << noiseSuppress << "(level" << noiseLevel << ") AGC2"
                  << autoGain;
    return true;
}

void AudioProcessor::setOptions(bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel)
{
    if (!impl_) return;
    if (echoCancel == echoCancel_ && noiseSuppress == noiseSuppress_ && autoGain == autoGain_ &&
        noiseLevel == noiseLevel_)
        return;
    impl_->apm->ApplyConfig(makeConfig(echoCancel, noiseSuppress, autoGain, noiseLevel));
    echoCancel_ = echoCancel;
    noiseSuppress_ = noiseSuppress;
    autoGain_ = autoGain;
    noiseLevel_ = noiseLevel;
    qCInfo(lcApm) << "WebRTC APM now: AEC3" << echoCancel << "NS" << noiseSuppress << "(level" << noiseLevel
                  << ") AGC2" << autoGain;
}

void AudioProcessor::processRender(const int16_t* frame)
{
    if (!impl_) return;
    // ProcessReverseStream requires an output buffer; we discard it.
    int16_t scratch[960];
    impl_->apm->ProcessReverseStream(frame, impl_->cfg, impl_->cfg, scratch);
}

void AudioProcessor::processCapture(int16_t* frame)
{
    if (!impl_) return;
    impl_->apm->set_stream_delay_ms(impl_->delayMs);
    impl_->apm->ProcessStream(frame, impl_->cfg, impl_->cfg, frame);
}

void AudioProcessor::setStreamDelayMs(int ms)
{
    if (impl_) impl_->delayMs = ms;
}

#else // !P2P_HAVE_WEBRTC_APM

struct AudioProcessor::Impl {};
AudioProcessor::AudioProcessor() = default;
AudioProcessor::~AudioProcessor() = default;
bool AudioProcessor::available() { return false; }
bool AudioProcessor::init(int, bool, bool, bool, int)
{
    qCInfo(lcApm) << "WebRTC APM not compiled in; audio processing disabled";
    return false;
}
void AudioProcessor::setOptions(bool, bool, bool, int) {}
void AudioProcessor::processRender(const int16_t*) {}
void AudioProcessor::processCapture(int16_t*) {}
void AudioProcessor::setStreamDelayMs(int) {}

#endif

} // namespace p2p::audio
