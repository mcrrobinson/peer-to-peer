#include "app/call_controller.h"

#include "net/control_channel.h"
#include "net/discovery.h"
#include "net/port_check.h"

#include <QDateTime>
#include <algorithm>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantMap>

#include <sodium.h>

Q_LOGGING_CATEGORY(lcCall, "p2p.call")
Q_LOGGING_CATEGORY(lcStats, "p2p.stats")

namespace p2p::app {

using net::ControlChannel;

namespace {
constexpr int kConnectTimeoutMs = 4000;
constexpr int kMaxLogLines = 200;

QString randomId()
{
    unsigned char b[8];
    randombytes_buf(b, sizeof(b));
    return QString::fromLatin1(QByteArray(reinterpret_cast<char*>(b), sizeof(b)).toHex());
}
} // namespace

// ---------------------------------------------------------------- Selections

QJsonObject CallController::Selections::toJson() const
{
    QJsonObject o{{"camera", camera.toJson()}};
    if (screen) o.insert("screen", screen->toJson());
    return o;
}

std::optional<CallController::Selections> CallController::Selections::fromJson(const QJsonObject& o)
{
    auto cam = media::StreamSelection::fromJson(o.value("camera").toObject());
    if (!cam) return std::nullopt;
    Selections s{*cam, std::nullopt};
    if (o.contains("screen")) {
        s.screen = media::StreamSelection::fromJson(o.value("screen").toObject());
        if (!s.screen) return std::nullopt;
    }
    return s;
}

QString CallController::Selections::summary() const
{
    return QStringLiteral("camera: %1; screen: %2")
        .arg(camera.summary(), screen ? screen->summary() : QStringLiteral("n/a"));
}

// ------------------------------------------------------------ CallController

CallController::CallController(Options opts, QObject* parent)
    : QObject(parent), opts_(std::move(opts)), instanceId_(randomId())
{
    connect(&server_, &QTcpServer::newConnection, this, &CallController::onNewConnection);
    mediaStatsTimer_.setInterval(500);
    connect(&mediaStatsTimer_, &QTimer::timeout, this, [this] {
        if (!media_) return;
        if (channel_) {
            media_->setClockOffsetUs(int64_t(channel_->clockOffsetMs() * 1000.0));
            if (channel_->rttMs() >= 0) media_->setRttUs(int64_t(channel_->rttMs() * 1000.0));
        }
        mediaStats_ = media_->statsText();
        emit mediaStatsChanged();
        static int tick = 0;
        if (++tick % 4 == 0) qCInfo(lcStats).noquote() << "\n" + mediaStats_; // every 2 s, any mode
    });
}

CallController::~CallController() = default;

bool CallController::start()
{
    // A port another program holds on 127.0.0.1 would take our loopback
    // connections (macOS allows the bind): pick another one, or fail like a
    // port in use.
    for (int attempt = 0;; ++attempt) {
        if (!server_.listen(QHostAddress::Any, opts_.port)) {
            setError(QStringLiteral("cannot listen on port %1: %2").arg(opts_.port).arg(server_.errorString()));
            return false;
        }
        if (!net::loopbackPortShadowed(server_.serverPort(), false)) break;
        const quint16 taken = server_.serverPort();
        server_.close();
        if (opts_.port != 0 || attempt == 7) {
            setError(QStringLiteral("cannot listen on port %1: another program holds it on 127.0.0.1").arg(taken));
            return false;
        }
    }
    appendLog(QStringLiteral("listening on port %1").arg(server_.serverPort()));
    appendLog(QStringLiteral("local codecs: %1").arg(opts_.caps.summary()));
    if (opts_.discovery) {
        discovery_ = std::make_unique<net::Discovery>(instanceId_, opts_.name, server_.serverPort());
        connect(discovery_.get(), &net::Discovery::peersChanged, this, &CallController::peersChanged);
        if (!discovery_->start())
            appendLog(QStringLiteral("LAN discovery unavailable (manual address still works)"));
    }
    return true;
}

QString CallController::stateName() const
{
    switch (state_) {
    case State::Idle: return QStringLiteral("idle");
    case State::Connecting: return QStringLiteral("connecting");
    case State::Handshaking: return QStringLiteral("handshaking");
    case State::Outgoing: return QStringLiteral("outgoing");
    case State::Incoming: return QStringLiteral("incoming");
    case State::InCall: return QStringLiteral("in-call");
    }
    return {};
}

QString CallController::remoteName() const { return channel_ ? channel_->remoteName() : QString(); }
QString CallController::sas() const
{
    return channel_ && channel_->isEstablished() ? channel_->sas() : QString();
}
double CallController::rttMs() const { return channel_ ? channel_->rttMs() : -1.0; }
double CallController::clockOffsetMs() const { return channel_ ? channel_->clockOffsetMs() : 0.0; }

QVariantList CallController::peers() const
{
    QVariantList out;
    if (!discovery_) return out;
    for (const auto& p : discovery_->peers()) {
        out.append(QVariantMap{{"id", p.id},
                               {"name", p.name},
                               {"host", p.host.toString()},
                               {"port", p.port}});
    }
    return out;
}

void CallController::setPreferAv1Camera(bool v)
{
    if (cameraPolicy_.preferAv1 == v) return;
    cameraPolicy_.preferAv1 = v;
    emit policyChanged();
}

void CallController::setPreferAv1Screen(bool v)
{
    if (screenPolicy_.preferAv1 == v) return;
    screenPolicy_.preferAv1 = v;
    emit policyChanged();
}

// ------------------------------------------------------------ call actions

void CallController::callAddress(const QString& hostPort)
{
    if (state_ != State::Idle) {
        setError(QStringLiteral("already in a call"));
        return;
    }
    const int colon = hostPort.lastIndexOf(':');
    bool ok = false;
    const QString host = colon > 0 ? hostPort.left(colon).trimmed() : QString();
    const int port = colon > 0 ? hostPort.mid(colon + 1).toInt(&ok) : 0;
    if (host.isEmpty() || !ok || port <= 0 || port > 65535) {
        setError(QStringLiteral("address must be host:port"));
        return;
    }

    setError({});
    auto* sock = new QTcpSocket(this);
    pendingSocket_ = sock;
    setState(State::Connecting);
    appendLog(QStringLiteral("connecting to %1:%2").arg(host).arg(port));

    connect(sock, &QTcpSocket::connected, this, [this, sock] {
        if (pendingSocket_ != sock) return;
        pendingSocket_ = nullptr;
        sock->disconnect(this);
        attachChannel(sock, /*initiator=*/true);
    });
    connect(sock, &QTcpSocket::errorOccurred, this, [this, sock] {
        if (pendingSocket_ != sock) return;
        pendingSocket_ = nullptr;
        setError(QStringLiteral("connect failed: %1").arg(sock->errorString()));
        sock->deleteLater();
        endCall(QStringLiteral("connect failed"));
    });
    QTimer::singleShot(kConnectTimeoutMs, sock, [this, sock] {
        if (pendingSocket_ != sock) return;
        pendingSocket_ = nullptr;
        setError(QStringLiteral("connect timed out"));
        sock->abort();
        sock->deleteLater();
        endCall(QStringLiteral("connect timed out"));
    });
    sock->connectToHost(host, quint16(port));
}

void CallController::callPeer(const QString& peerId)
{
    if (!discovery_) return;
    for (const auto& p : discovery_->peers()) {
        if (p.id == peerId) {
            callAddress(QStringLiteral("%1:%2").arg(p.host.toString()).arg(p.port));
            return;
        }
    }
    setError(QStringLiteral("peer no longer available"));
}

void CallController::accept()
{
    if (state_ != State::Incoming || !channel_) return;
    sendSel_ = computeSendSelections();
    if (!sendSel_) {
        channel_->send({{"t", "reject"}, {"reason", "no common codec"}});
        setError(QStringLiteral("no codec in common with peer"));
        channel_->close(QStringLiteral("no common codec"));
        return;
    }
    channel_->send({{"t", "accept"}, {"send", sendSel_->toJson()}});
    emit callInfoChanged();
    appendLog(QStringLiteral("call accepted; sending %1").arg(sendSel_->summary()));
    enterCall();
}

void CallController::reject()
{
    if (state_ != State::Incoming || !channel_) return;
    channel_->send({{"t", "reject"}, {"reason", "declined"}});
    channel_->close(QStringLiteral("rejected"));
}

void CallController::hangup()
{
    if (pendingSocket_) {
        pendingSocket_->abort();
        pendingSocket_->deleteLater();
        pendingSocket_ = nullptr;
        endCall(QStringLiteral("cancelled"));
        return;
    }
    if (!channel_) return;
    if (channel_->isEstablished())
        channel_->send({{"t", "hangup"}});
    channel_->close(QStringLiteral("local hangup"));
}

// ------------------------------------------------------------ channel events

void CallController::onNewConnection()
{
    while (QTcpSocket* sock = server_.nextPendingConnection()) {
        if (state_ != State::Idle) {
            appendLog(QStringLiteral("rejecting connection from %1: busy").arg(sock->peerAddress().toString()));
            sock->abort();
            sock->deleteLater();
            continue;
        }
        appendLog(QStringLiteral("incoming connection from %1").arg(sock->peerAddress().toString()));
        attachChannel(sock, /*initiator=*/false);
    }
}

void CallController::attachChannel(QTcpSocket* sock, bool initiator)
{
    auto* ch = new ControlChannel(sock, initiator ? ControlChannel::Role::Initiator : ControlChannel::Role::Responder,
                                  opts_.name, this);
    channel_ = ch;
    connect(ch, &ControlChannel::established, this, &CallController::onEstablished);
    connect(ch, &ControlChannel::message, this, &CallController::onMessage);
    connect(ch, &ControlChannel::statsUpdated, this, &CallController::statsChanged);
    connect(ch, &ControlChannel::closed, this, &CallController::onChannelClosed);
    setState(State::Handshaking);
    ch->start();
}

void CallController::onEstablished()
{
    appendLog(QStringLiteral("secure channel with %1, SAS %2").arg(channel_->remoteName(), channel_->sas()));
    emit callInfoChanged();
    QJsonObject caps{{"t", "caps"}, {"caps", opts_.caps.toJson()}};
    if (opts_.media) {
        media_ = std::make_unique<MediaSession>(opts_.mediaConfig);
        QString err;
        if (media_->open(&err)) {
            caps.insert("mediaPort", int(media_->localPort()));
        } else {
            appendLog(QStringLiteral("media socket unavailable: %1").arg(err));
            media_.reset();
        }
    }
    channel_->send(caps);
}

void CallController::onMessage(const QJsonObject& msg)
{
    if (!channel_) return;
    const QString t = msg.value("t").toString();
    const bool initiator = channel_->role() == ControlChannel::Role::Initiator;

    if (t == "caps") {
        if (remoteCaps_) return; // ignore duplicates
        remoteCaps_ = media::MediaCaps::fromJson(msg.value("caps").toObject());
        const int mp = msg.value("mediaPort").toInt();
        remoteMediaPort_ = (mp > 0 && mp <= 65535) ? uint16_t(mp) : 0;
        appendLog(QStringLiteral("remote codecs: %1").arg(remoteCaps_->summary()));
        if (initiator && state_ == State::Handshaking) {
            sendSel_ = computeSendSelections();
            if (!sendSel_) {
                setError(QStringLiteral("no codec in common with peer"));
                channel_->send({{"t", "hangup"}});
                channel_->close(QStringLiteral("no common codec"));
                return;
            }
            channel_->send({{"t", "invite"}, {"send", sendSel_->toJson()}});
            emit callInfoChanged();
            setState(State::Outgoing);
        }
        return;
    }

    if (t == "invite" && !initiator && state_ == State::Handshaking) {
        if (!remoteCaps_ || !acceptRemoteSelections(msg.value("send").toObject())) {
            channel_->send({{"t", "reject"}, {"reason", "unsupported stream selection"}});
            channel_->close(QStringLiteral("bad invite"));
            return;
        }
        setState(State::Incoming);
        appendLog(QStringLiteral("incoming call from %1").arg(channel_->remoteName()));
        if (opts_.autoAccept)
            accept();
        return;
    }

    if (t == "accept" && initiator && state_ == State::Outgoing) {
        if (!acceptRemoteSelections(msg.value("send").toObject())) {
            channel_->send({{"t", "hangup"}});
            channel_->close(QStringLiteral("bad accept"));
            return;
        }
        appendLog(QStringLiteral("call established; sending %1").arg(sendSel_->summary()));
        enterCall();
        return;
    }

    if (t == "reject") {
        setError(QStringLiteral("call rejected: %1").arg(msg.value("reason").toString()));
        channel_->close(QStringLiteral("rejected by peer"));
        return;
    }

    if (t == "hangup") {
        channel_->close(QStringLiteral("remote hangup"));
        return;
    }

    if (t == "screen" && state_ == State::InCall) {
        const bool on = msg.value("on").toBool();
        if (on == remoteSharing_) return;
        remoteSharing_ = on;
        if (!on && media_) media_->clearRemoteScreen();
        appendLog(on ? QStringLiteral("%1 is sharing their screen (%2)").arg(channel_->remoteName(),
                                                                             msg.value("mode").toString())
                     : QStringLiteral("%1 stopped sharing").arg(channel_->remoteName()));
        emit screenShareChanged();
        return;
    }

    qCDebug(lcCall) << "ignoring message" << t << "in state" << stateName();
}

bool CallController::acceptRemoteSelections(const QJsonObject& o)
{
    auto sel = Selections::fromJson(o);
    if (!sel) return false;
    if (!opts_.caps.canDecode(sel->camera.codec)) return false;
    if (sel->screen && !opts_.caps.canDecode(sel->screen->codec)) return false;
    recvSel_ = sel;
    appendLog(QStringLiteral("receiving %1").arg(recvSel_->summary()));
    emit callInfoChanged();
    return true;
}

std::optional<CallController::Selections> CallController::computeSendSelections() const
{
    if (!remoteCaps_) return std::nullopt;
    auto cam = media::negotiate(opts_.caps.encoders, remoteCaps_->decoders, cameraPolicy_);
    if (!cam) return std::nullopt;
    return Selections{*cam, media::negotiate(opts_.caps.encoders, remoteCaps_->decoders, screenPolicy_)};
}

void CallController::enterCall()
{
    setState(State::InCall);
    startMedia();
    emit callEstablished();
}

void CallController::startMedia()
{
    if (!media_ || !channel_) return;
    if (remoteMediaPort_ == 0) {
        appendLog(QStringLiteral("peer offered no media port; signalling only"));
        return;
    }
    QString err;
    std::optional<media::StreamSelection> sendCam, recvCam, sendScreen, recvScreen;
    if (sendSel_) {
        sendCam = sendSel_->camera;
        sendScreen = sendSel_->screen;
    }
    if (recvSel_) {
        recvCam = recvSel_->camera;
        recvScreen = recvSel_->screen;
    }
    if (!media_->start(channel_->keys(), channel_->peerAddress(), remoteMediaPort_, sendCam, recvCam, sendScreen,
                       recvScreen, &err)) {
        setError(QStringLiteral("media failed to start: %1").arg(err));
        media_.reset();
        return;
    }
    media_->setMuted(micMuted_);
    media_->setClockOffsetUs(int64_t(channel_->clockOffsetMs() * 1000.0));
    if (channel_->rttMs() >= 0) media_->setRttUs(int64_t(channel_->rttMs() * 1000.0));
    appendLog(QStringLiteral("media flowing: udp %1 -> %2:%3")
                  .arg(media_->localPort())
                  .arg(channel_->peerAddress().toString())
                  .arg(remoteMediaPort_));
    mediaStatsTimer_.start();
    emit screenShareChanged(); // canShareScreen
}

void CallController::startScreenShare(const QString& mode)
{
    if (state_ != State::InCall || !media_ || !channel_ || sharingScreen_) return;
    const bool motion = mode == QLatin1String("motion");
    QString err;
    // The source reports its own end (portal cancelled, stopped from the
    // desktop) from another thread: hop back here.
    QPointer<CallController> self(this);
    auto onEnded = [self](QString why) {
        QMetaObject::invokeMethod(self, [self, why] {
            if (!self || !self->sharingScreen_) return;
            self->appendLog(QStringLiteral("screen sharing ended: %1").arg(why));
            self->stopScreenShare();
        }, Qt::QueuedConnection);
    };
    if (!media_->startScreen(motion ? video::ScreenConfig::Mode::Motion : video::ScreenConfig::Mode::Text, onEnded,
                             &err)) {
        setError(QStringLiteral("screen sharing failed: %1").arg(err));
        return;
    }
    sharingScreen_ = true;
    screenMode_ = motion ? QStringLiteral("motion") : QStringLiteral("text");
    channel_->send({{"t", "screen"}, {"on", true}, {"mode", screenMode_}});
    appendLog(QStringLiteral("sharing screen (%1 mode)").arg(screenMode_));
    emit screenShareChanged();
}

void CallController::stopScreenShare()
{
    if (!sharingScreen_) return;
    sharingScreen_ = false;
    screenMode_.clear();
    if (media_) media_->stopScreen();
    if (channel_ && channel_->isEstablished()) channel_->send({{"t", "screen"}, {"on", false}});
    appendLog(QStringLiteral("stopped sharing screen"));
    emit screenShareChanged();
}

void CallController::setMicMuted(bool m)
{
    if (micMuted_ == m) return;
    micMuted_ = m;
    if (media_) media_->setMuted(m);
    emit micMutedChanged();
}

void CallController::applyAudioProcessing()
{
    const auto& a = opts_.mediaConfig.audio;
    if (media_) media_->setAudioProcessing(a.echoCancel, a.noiseSuppress, a.autoGain, a.noiseLevel);
    static const char* kLevels[] = {"low", "moderate", "high"};
    appendLog(QStringLiteral("mic processing: echo cancellation %1, noise suppression %2, auto gain %3")
                  .arg(a.echoCancel ? "on" : "off",
                       a.noiseSuppress ? QString::fromLatin1(kLevels[std::clamp(a.noiseLevel, 0, 2)]) : "off",
                       a.autoGain ? "on" : "off"));
    emit audioProcessingChanged();
}

void CallController::setEchoCancel(bool v)
{
    if (opts_.mediaConfig.audio.echoCancel == v) return;
    opts_.mediaConfig.audio.echoCancel = v;
    applyAudioProcessing();
}

void CallController::setNoiseSuppression(bool v)
{
    if (opts_.mediaConfig.audio.noiseSuppress == v) return;
    opts_.mediaConfig.audio.noiseSuppress = v;
    applyAudioProcessing();
}

void CallController::setNoiseLevel(int level)
{
    level = std::clamp(level, 0, 2);
    if (opts_.mediaConfig.audio.noiseLevel == level) return;
    opts_.mediaConfig.audio.noiseLevel = level;
    applyAudioProcessing();
}

void CallController::setAutoGain(bool v)
{
    if (opts_.mediaConfig.audio.autoGain == v) return;
    opts_.mediaConfig.audio.autoGain = v;
    applyAudioProcessing();
}

void CallController::onChannelClosed(const QString& reason)
{
    if (sender() != channel_) return;
    channel_->deleteLater();
    channel_ = nullptr;
    endCall(reason);
}

void CallController::endCall(const QString& reason)
{
    const bool wasActive = state_ != State::Idle;
    mediaStatsTimer_.stop();
    const bool hadScreen = sharingScreen_ || remoteSharing_;
    sharingScreen_ = remoteSharing_ = false;
    screenMode_.clear();
    media_.reset();
    if (hadScreen) emit screenShareChanged();
    remoteMediaPort_ = 0;
    mediaStats_.clear();
    emit mediaStatsChanged();
    remoteCaps_.reset();
    sendSel_.reset();
    recvSel_.reset();
    setState(State::Idle);
    emit callInfoChanged();
    emit statsChanged();
    if (wasActive) {
        appendLog(QStringLiteral("call ended: %1").arg(reason));
        emit callEnded(reason);
    }
}

void CallController::setState(State s)
{
    if (state_ == s) return;
    state_ = s;
    qCInfo(lcCall) << "state ->" << stateName();
    emit stateChanged();
}

void CallController::setError(const QString& e)
{
    if (lastError_ == e) return;
    lastError_ = e;
    if (!e.isEmpty()) qCWarning(lcCall).noquote() << e;
    emit lastErrorChanged();
}

void CallController::appendLog(const QString& line)
{
    qCInfo(lcCall).noquote() << line;
    log_.append(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), line));
    while (log_.size() > kMaxLogLines) log_.removeFirst();
    emit logChanged();
}

} // namespace p2p::app
