#include "app/media_session.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSession, "p2p.session")

namespace p2p::app {

namespace {
constexpr char kMediaKeyContext[8] = {'p', '2', 'p', 'm', 'e', 'd', 'i', 'a'};
}

MediaSession::MediaSession(Config cfg) : cfg_(std::move(cfg)) {}

MediaSession::~MediaSession()
{
    stop();
}

bool MediaSession::open(QString* error)
{
    return transport_.open(0, error);
}

bool MediaSession::start(const crypto::SessionKeys& keys, const QHostAddress& remote, uint16_t remotePort,
                         const std::optional<media::StreamSelection>& sendCamera,
                         const std::optional<media::StreamSelection>& recvCamera, QString* error)
{
    if (started_) return true;
    // Independent subkeys for the media plane; A.tx == B.rx so both ends agree.
    transport_.setKeys(crypto::deriveSubkey(keys.tx, 1, kMediaKeyContext),
                       crypto::deriveSubkey(keys.rx, 1, kMediaKeyContext));
    if (!transport_.setRemote(remote, remotePort)) {
        if (error) *error = QStringLiteral("bad remote media address");
        return false;
    }
    transport_.setSimulatedLoss(cfg_.simulatedLoss, cfg_.simulatedBurst);

    audio_ = std::make_unique<audio::AudioPipeline>(cfg_.audio, transport_);
    audio_->attach();
    if (sendCamera && cfg_.video.source != video::VideoStreamConfig::Source::None) {
        videoSend_ = std::make_unique<video::VideoSendStream>(cfg_.video, *sendCamera, transport_,
                                                              net::wire::kCamera, QStringLiteral("local"));
        videoSend_->attach();
    }
    if (recvCamera) {
        videoRecv_ = std::make_unique<video::VideoReceiveStream>(*recvCamera, transport_, net::wire::kCamera,
                                                                 QStringLiteral("remote"), cfg_.video.nack);
        videoRecv_->attach();
    }
    transport_.start();
    if (!audio_->start(error)) {
        stop();
        return false;
    }
    // Video problems are reported but don't take the call down.
    QString verr;
    if (videoRecv_ && !videoRecv_->start(&verr)) {
        qCWarning(lcSession).noquote() << "video receive disabled:" << verr;
        videoRecv_.reset();
    }
    if (videoSend_ && !videoSend_->start(&verr)) {
        qCWarning(lcSession).noquote() << "video send disabled:" << verr;
        videoSend_.reset();
    }
    started_ = true;
    return true;
}

std::optional<video::VideoSendStream::Stats> MediaSession::videoSendStats() const
{
    if (!videoSend_) return std::nullopt;
    return videoSend_->stats();
}

std::optional<video::VideoReceiveStream::Stats> MediaSession::videoRecvStats() const
{
    if (!videoRecv_) return std::nullopt;
    return videoRecv_->stats();
}

void MediaSession::stop()
{
    // Sources/decoders first (they call into the transport), then the transport.
    if (videoSend_) videoSend_->stop();
    if (videoRecv_) videoRecv_->stop();
    if (audio_) audio_->stop();
    transport_.stop();
    videoSend_.reset();
    videoRecv_.reset();
    audio_.reset();
    started_ = false;
}

void MediaSession::setClockOffsetUs(int64_t us)
{
    if (audio_) audio_->setClockOffsetUs(us);
    if (videoRecv_) videoRecv_->setClockOffsetUs(us);
}

void MediaSession::setRttUs(int64_t us)
{
    if (videoRecv_) videoRecv_->setRttUs(us);
}

void MediaSession::setAudioProcessing(bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel)
{
    cfg_.audio.echoCancel = echoCancel;
    cfg_.audio.noiseSuppress = noiseSuppress;
    cfg_.audio.autoGain = autoGain;
    cfg_.audio.noiseLevel = noiseLevel;
    if (audio_) audio_->setProcessing(echoCancel, noiseSuppress, autoGain, noiseLevel);
}

void MediaSession::setMuted(bool m)
{
    if (audio_) audio_->setMuted(m);
}

audio::AudioPipeline::Stats MediaSession::audioStats() const
{
    return audio_ ? audio_->stats() : audio::AudioPipeline::Stats{};
}

QString MediaSession::statsText() const
{
    if (!audio_) return {};
    const auto t = transport_.stats();
    QString s = audio::AudioPipeline::format(audio_->stats());
    if (videoSend_) {
        const auto v = videoSend_->stats();
        s += QStringLiteral("\nvideo tx %1 %2x%3 %4 fps %5 kbps | enc %6 avg %7 ms max %8 ms | key %9 pli-rx %10\n"
                            "     src %11\n     loss rep %12% | fec %13% (%14 parity) | nack items %15 resent %16 miss %17")
                 .arg(v.encoder)
                 .arg(v.width)
                 .arg(v.height)
                 .arg(v.fps, 0, 'f', 1)
                 .arg(v.kbps, 0, 'f', 0)
                 .arg(v.encoded)
                 .arg(v.encodeAvgMs, 0, 'f', 2)
                 .arg(v.encodeMaxMs, 0, 'f', 1)
                 .arg(v.keyframes)
                 .arg(v.pliReceived)
                 .arg(v.source)
                 .arg(v.reportedLoss * 100, 0, 'f', 2)
                 .arg(v.fecRate * 100, 0, 'f', 0)
                 .arg(v.parityFragments)
                 .arg(v.nackItems)
                 .arg(v.retransmitted)
                 .arg(v.retransmitMisses);
    }
    if (videoRecv_) {
        const auto v = videoRecv_->stats();
        s += QStringLiteral("\nvideo rx %1 %2x%3 %4 fps %5 kbps | dec %6 avg %7 ms err %8 | lost %9 skipped %10 "
                            "pli-tx %11\n     latency capture->decoded avg %12 ms p95 %13 ms | pattern ok %14 bad %15\n"
                            "     path loss %16% | recovered frames %17 (fec %18 frags, resend %19/%20 useful) nack %21")
                 .arg(v.decoder)
                 .arg(v.width)
                 .arg(v.height)
                 .arg(v.fps, 0, 'f', 1)
                 .arg(v.kbps, 0, 'f', 0)
                 .arg(v.decoded)
                 .arg(v.decodeAvgMs, 0, 'f', 2)
                 .arg(v.decodeErrors)
                 .arg(v.assembler.framesLost)
                 .arg(v.assembler.framesSkipped)
                 .arg(v.pliSent)
                 .arg(v.latencyAvgMs, 0, 'f', 1)
                 .arg(v.latencyP95Ms, 0, 'f', 1)
                 .arg(v.patternOk)
                 .arg(v.patternBad)
                 .arg(v.pathLoss * 100, 0, 'f', 2)
                 .arg(v.recoveredDecoded)
                 .arg(v.assembler.fecRecoveredFragments)
                 .arg(v.assembler.retransmitsUseful)
                 .arg(v.assembler.retransmitsReceived)
                 .arg(v.assembler.nacked);
    }
    s += QStringLiteral("\nudp tx %1 pkts rx %2 pkts | auth-fail %3 replay %4 sim-drop %5 send-err %6")
             .arg(t.packetsSent)
             .arg(t.packetsReceived)
             .arg(t.authFailures)
             .arg(t.replays)
             .arg(t.simDropped)
             .arg(t.sendErrors);
    return s;
}

} // namespace p2p::app
