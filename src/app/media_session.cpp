#include "app/media_session.h"

#include <QLoggingCategory>

#include <cmath>

Q_LOGGING_CATEGORY(lcSession, "p2p.session")

namespace p2p::app {

namespace {
constexpr char kMediaKeyContext[8] = {'p', '2', 'p', 'm', 'e', 'd', 'i', 'a'};
constexpr double kScreenShare = 0.65; // of the video budget while camera and screen both send

QString formatSend(const char* label, const video::VideoSendStream::Stats& v)
{
    return QStringLiteral("\n%25 tx %1 %2x%3 %4 fps %5 kbps | enc %6 avg %7 ms max %8 ms | key %9 pli-rx %10\n"
                          "     src %11\n     loss est %12% | fec %13% (%14 parity) | nack items %15 resent %16 miss %17\n"
                          "     ladder rung %18/%19 %20 kbps (budget %21) switches %22 reopen max %23 ms | skipped %24")
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
        .arg(v.retransmitMisses)
        .arg(v.rungIndex + 1)
        .arg(v.rungCount)
        .arg(v.rungKbps)
        .arg(v.budgetKbps, 0, 'f', 0)
        .arg(v.rungSwitches)
        .arg(v.reopenMaxMs, 0, 'f', 1)
        .arg(v.framesSkipped)
        .arg(QLatin1String(label)) +
           (v.stages.isEmpty() ? QString() : QStringLiteral("\n     stages ms avg/p95: %1").arg(v.stages));
}

QString formatDisplay(const video::FrameHub::DisplayStats& d)
{
    if (d.shown == 0) return {};
    const QString wait = std::isnan(d.wait.avg)
                             ? QString()
                             : QStringLiteral("wait %1/%2 | ").arg(d.wait.avg, 0, 'f', 1).arg(d.wait.p95, 0, 'f', 1);
    return QStringLiteral("\n     display %1capture->screen %2/%3 ms | shown %4 (%6 fps) superseded %5")
        .arg(wait)
        .arg(d.total.avg, 0, 'f', 1)
        .arg(d.total.p95, 0, 'f', 1)
        .arg(d.shown)
        .arg(d.superseded)
        .arg(d.shownFps, 0, 'f', 1);
}

QString formatRecv(const char* label, const video::VideoReceiveStream::Stats& v)
{
    return QStringLiteral("\n%22 rx %1 %2x%3 %4 fps %5 kbps | dec %6 avg %7 ms err %8 | lost %9 skipped %10 "
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
        .arg(v.assembler.nacked)
        .arg(QLatin1String(label)) +
           (v.stages.isEmpty() ? QString() : QStringLiteral("\n     stages ms avg/p95: %1").arg(v.stages)) +
           formatDisplay(v.display);
}
} // namespace

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
                         const std::optional<media::StreamSelection>& recvCamera,
                         const std::optional<media::StreamSelection>& sendScreen,
                         const std::optional<media::StreamSelection>& recvScreen, QString* error)
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
    transport_.setLinkSim(cfg_.linkSim);
    // The controller's range covers video (max with FEC headroom, min) plus
    // audio; with screen sharing negotiated, the screen's maximum too.
    net::cc::CongestionController::Config cc;
    const bool screenPossible = sendScreen && cfg_.screen.source != video::VideoStreamConfig::Source::None;
    cc.maxBps = int64_t(cfg_.video.bitrateKbps + (screenPossible ? cfg_.screen.bitrateKbps : 0)) * 1250 + 150'000;
    cc.minBps = int64_t(cfg_.video.minBitrateKbps) * 1000 + 80'000;
    transport_.setCongestionControl(cc, cfg_.video.congestionControl);

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
    if (sendScreen && cfg_.screen.source != video::VideoStreamConfig::Source::None) {
        auto sc = cfg_.screen;
        sc.onSourceEnded = [this](QString why) {
            if (screenEnded_) screenEnded_(std::move(why));
        };
        screenSend_ = std::make_unique<video::VideoSendStream>(sc, *sendScreen, transport_, net::wire::kScreen,
                                                               QStringLiteral("local-screen"));
        screenSend_->attach();
    }
    if (recvScreen) {
        screenRecv_ = std::make_unique<video::VideoReceiveStream>(*recvScreen, transport_, net::wire::kScreen,
                                                                  QStringLiteral("remote-screen"), cfg_.screen.nack);
        screenRecv_->attach();
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
    if (screenRecv_ && !screenRecv_->start(&verr)) {
        qCWarning(lcSession).noquote() << "screen receive disabled:" << verr;
        screenRecv_.reset();
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

std::optional<video::VideoSendStream::Stats> MediaSession::screenSendStats() const
{
    if (!screenSend_) return std::nullopt;
    return screenSend_->stats();
}

std::optional<video::VideoReceiveStream::Stats> MediaSession::screenRecvStats() const
{
    if (!screenRecv_) return std::nullopt;
    return screenRecv_->stats();
}

bool MediaSession::startScreen(video::ScreenConfig::Mode mode, std::function<void(QString)> onEnded, QString* error)
{
    if (!screenSend_) {
        if (error) *error = QStringLiteral("screen sharing was not negotiated for this call");
        return false;
    }
    if (screenSend_->running()) return true;
    video::ScreenConfig sc = cfg_.screen.screen;
    sc.mode = mode;
    sc.maxFps = mode == video::ScreenConfig::Mode::Text ? 30 : 60;
    sc.maxWidth = mode == video::ScreenConfig::Mode::Text ? 3840 : 1920;
    sc.maxHeight = mode == video::ScreenConfig::Mode::Text ? 2160 : 1080;
    screenSend_->setScreenConfig(sc);
    screenEnded_ = std::move(onEnded);
    if (!screenSend_->start(error)) return false;
    updateBudgetShares();
    return true;
}

void MediaSession::stopScreen()
{
    if (!screenSend_) return;
    screenSend_->stop();
    updateBudgetShares();
}

void MediaSession::clearRemoteScreen()
{
    video::FrameHub::instance().clear(QStringLiteral("remote-screen"));
    if (screenRecv_) screenRecv_->resetPatternCheck();
}

void MediaSession::updateBudgetShares()
{
    const bool screen = sharingScreen();
    const bool camera = videoSend_ && videoSend_->running();
    if (videoSend_) videoSend_->setBudgetShare(screen ? 1.0 - kScreenShare : 1.0);
    if (screenSend_) screenSend_->setBudgetShare(camera ? kScreenShare : 1.0);
}

void MediaSession::stop()
{
    // Sources/decoders first (they call into the transport), then the transport.
    if (screenSend_) screenSend_->stop();
    if (screenRecv_) screenRecv_->stop();
    if (videoSend_) videoSend_->stop();
    if (videoRecv_) videoRecv_->stop();
    if (audio_) audio_->stop();
    transport_.stop();
    screenSend_.reset();
    screenRecv_.reset();
    screenEnded_ = nullptr;
    videoSend_.reset();
    videoRecv_.reset();
    audio_.reset();
    started_ = false;
}

void MediaSession::setClockOffsetUs(int64_t us)
{
    if (audio_) audio_->setClockOffsetUs(us);
    if (videoRecv_) videoRecv_->setClockOffsetUs(us);
    if (screenRecv_) screenRecv_->setClockOffsetUs(us);
}

void MediaSession::setRttUs(int64_t us)
{
    if (videoRecv_) videoRecv_->setRttUs(us);
    if (screenRecv_) screenRecv_->setRttUs(us);
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
        s += formatSend("video", videoSend_->stats());
        // Self view: local capture -> on screen.
        s += formatDisplay(video::FrameHub::instance().displayStats(QStringLiteral("local")));
    }
    if (videoRecv_) s += formatRecv("video", videoRecv_->stats());
    if (screenSend_ && screenSend_->running()) s += formatSend("screen", screenSend_->stats());
    if (screenRecv_ && screenRecv_->stats().decoded > 0) s += formatRecv("screen", screenRecv_->stats());
    {
        const auto c = transport_.ccStats();
        s += QStringLiteral("\ncc %1 target %2 kbps delivered %3 | qdelay %4 ms srtt %5 ms | loss %6% (lost %7 "
                            "reordered %8) ce %9 | events delay %10 loss %11 ce %12 | pacing %13 queue %14 ms")
                 .arg(transport_.congestionControlEnabled() ? QStringLiteral("on") : QStringLiteral("off"))
                 .arg(c.targetBps / 1000)
                 .arg(c.deliveredBps / 1000)
                 .arg(double(c.queueDelayUs) / 1000.0, 0, 'f', 1)
                 .arg(double(c.srttUs) / 1000.0, 0, 'f', 1)
                 .arg(c.lossRate * 100, 0, 'f', 2)
                 .arg(c.lost)
                 .arg(c.spuriousLost)
                 .arg(c.ceMarked)
                 .arg(c.delayEvents)
                 .arg(c.lossEvents)
                 .arg(c.ceEvents)
                 .arg(c.pacingBps > 0 ? QStringLiteral("%1 kbps").arg(c.pacingBps / 1000) : QStringLiteral("off"))
                 .arg(double(transport_.pacerQueueUs()) / 1000.0, 0, 'f', 0);
    }
    s += QStringLiteral("\nudp tx %1 pkts rx %2 pkts | auth-fail %3 replay %4 sim-drop %5 send-err %6 pacer-drop %7")
             .arg(t.packetsSent)
             .arg(t.packetsReceived)
             .arg(t.authFailures)
             .arg(t.replays)
             .arg(t.simDropped)
             .arg(t.sendErrors)
             .arg(t.pacerDropped);
    if (cfg_.linkSim.active()) s += QStringLiteral(" sim-queue-drop %1").arg(t.simQueueDrops);
    return s;
}

} // namespace p2p::app
