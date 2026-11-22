#pragma once

#include "app/media_session.h"
#include "media/codec_caps.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTimer>
#include <QVariantList>

#include <memory>
#include <optional>

class QTcpSocket;

namespace p2p::net {
class ControlChannel;
class Discovery;
}

namespace p2p::app {

// Owns the call lifecycle for a single 1:1 call:
//   Idle -> Connecting -> Handshaking -> Outgoing/Incoming -> InCall -> Idle
//
// Signalling messages (all inside the encrypted control channel):
//   caps    {caps}                     both sides, right after the handshake
//   invite  {send:{camera,screen}}     initiator, once remote caps are known
//   accept  {send:{camera,screen}}     responder
//   reject  {reason}
//   screen  {on, mode}                 sender started / stopped sharing its screen
//   hangup  {}
// Each side chooses what *it* sends, from its encoders and the peer's
// decoders; the receiver validates that it can decode the choice.
class CallController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ stateName NOTIFY stateChanged)
    Q_PROPERTY(QString localName READ localName CONSTANT)
    Q_PROPERTY(int listenPort READ listenPort CONSTANT)
    Q_PROPERTY(QString localCapsSummary READ localCapsSummary CONSTANT)
    Q_PROPERTY(QString remoteName READ remoteName NOTIFY callInfoChanged)
    Q_PROPERTY(QString sas READ sas NOTIFY callInfoChanged)
    Q_PROPERTY(QString sendSummary READ sendSummary NOTIFY callInfoChanged)
    Q_PROPERTY(QString recvSummary READ recvSummary NOTIFY callInfoChanged)
    Q_PROPERTY(double rttMs READ rttMs NOTIFY statsChanged)
    Q_PROPERTY(double clockOffsetMs READ clockOffsetMs NOTIFY statsChanged)
    Q_PROPERTY(QVariantList peers READ peers NOTIFY peersChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QStringList logLines READ logLines NOTIFY logChanged)
    Q_PROPERTY(bool preferAv1Camera READ preferAv1Camera WRITE setPreferAv1Camera NOTIFY policyChanged)
    Q_PROPERTY(bool preferAv1Screen READ preferAv1Screen WRITE setPreferAv1Screen NOTIFY policyChanged)
    Q_PROPERTY(QString mediaStats READ mediaStats NOTIFY mediaStatsChanged)
    Q_PROPERTY(bool micMuted READ micMuted WRITE setMicMuted NOTIFY micMutedChanged)
    Q_PROPERTY(bool echoCancel READ echoCancel WRITE setEchoCancel NOTIFY audioProcessingChanged)
    Q_PROPERTY(bool noiseSuppression READ noiseSuppression WRITE setNoiseSuppression NOTIFY audioProcessingChanged)
    Q_PROPERTY(int noiseLevel READ noiseLevel WRITE setNoiseLevel NOTIFY audioProcessingChanged)
    Q_PROPERTY(bool autoGain READ autoGain WRITE setAutoGain NOTIFY audioProcessingChanged)
    Q_PROPERTY(bool canShareScreen READ canShareScreen NOTIFY screenShareChanged)
    Q_PROPERTY(bool sharingScreen READ sharingScreen NOTIFY screenShareChanged)
    Q_PROPERTY(QString screenMode READ screenMode NOTIFY screenShareChanged)
    Q_PROPERTY(bool remoteSharingScreen READ remoteSharingScreen NOTIFY screenShareChanged)

public:
    enum class State { Idle, Connecting, Handshaking, Outgoing, Incoming, InCall };
    Q_ENUM(State)

    struct Options {
        QString name;
        quint16 port = 0;          // 0 = ephemeral
        bool autoAccept = false;
        bool discovery = true;
        media::MediaCaps caps;
        bool media = true;              // run the UDP media plane during calls
        MediaSession::Config mediaConfig;
    };

    struct Selections {
        media::StreamSelection camera;
        std::optional<media::StreamSelection> screen;
        QJsonObject toJson() const;
        static std::optional<Selections> fromJson(const QJsonObject& o);
        QString summary() const;
    };

    explicit CallController(Options opts, QObject* parent = nullptr);
    ~CallController() override;

    bool start();

    State state() const { return state_; }
    QString stateName() const;
    QString localName() const { return opts_.name; }
    int listenPort() const { return server_.serverPort(); }
    QString localCapsSummary() const { return opts_.caps.summary(); }
    QString remoteName() const;
    QString sas() const;
    QString sendSummary() const { return sendSel_ ? sendSel_->summary() : QString(); }
    QString recvSummary() const { return recvSel_ ? recvSel_->summary() : QString(); }
    std::optional<Selections> sendSelections() const { return sendSel_; }
    std::optional<Selections> recvSelections() const { return recvSel_; }
    double rttMs() const;
    double clockOffsetMs() const;
    QVariantList peers() const;
    QString lastError() const { return lastError_; }
    QStringList logLines() const { return log_; }
    bool preferAv1Camera() const { return cameraPolicy_.preferAv1; }
    bool preferAv1Screen() const { return screenPolicy_.preferAv1; }
    void setPreferAv1Camera(bool v);
    void setPreferAv1Screen(bool v);
    QString mediaStats() const { return mediaStats_; }
    bool micMuted() const { return micMuted_; }
    void setMicMuted(bool m);
    // Mic processing; applied live to an active call and kept for later calls.
    bool echoCancel() const { return opts_.mediaConfig.audio.echoCancel; }
    bool noiseSuppression() const { return opts_.mediaConfig.audio.noiseSuppress; }
    int noiseLevel() const { return opts_.mediaConfig.audio.noiseLevel; }
    bool autoGain() const { return opts_.mediaConfig.audio.autoGain; }
    void setEchoCancel(bool v);
    void setNoiseSuppression(bool v);
    void setNoiseLevel(int level);
    void setAutoGain(bool v);
    const MediaSession* mediaSession() const { return media_.get(); }
    bool canShareScreen() const { return media_ && media_->canShareScreen(); }
    bool sharingScreen() const { return sharingScreen_; }
    QString screenMode() const { return screenMode_; }
    bool remoteSharingScreen() const { return remoteSharing_; }

    Q_INVOKABLE void callAddress(const QString& hostPort);
    Q_INVOKABLE void callPeer(const QString& peerId);
    Q_INVOKABLE void accept();
    Q_INVOKABLE void reject();
    Q_INVOKABLE void hangup();
    // |mode|: "text" (full resolution, up to 30 fps) or "motion" (1080p60).
    Q_INVOKABLE void startScreenShare(const QString& mode);
    Q_INVOKABLE void stopScreenShare();

signals:
    void stateChanged();
    void callInfoChanged();
    void statsChanged();
    void peersChanged();
    void lastErrorChanged();
    void logChanged();
    void policyChanged();
    void callEstablished();
    void callEnded(const QString& reason);
    void mediaStatsChanged();
    void micMutedChanged();
    void audioProcessingChanged();
    void screenShareChanged();

private:
    void onNewConnection();
    void attachChannel(QTcpSocket* sock, bool initiator);
    void onEstablished();
    void onMessage(const QJsonObject& msg);
    void onChannelClosed(const QString& reason);
    std::optional<Selections> computeSendSelections() const;
    bool acceptRemoteSelections(const QJsonObject& o);
    void setState(State s);
    void setError(const QString& e);
    void endCall(const QString& reason);
    void appendLog(const QString& line);
    void startMedia();
    void applyAudioProcessing();
    void enterCall();

    Options opts_;
    QString instanceId_;
    QTcpServer server_;
    std::unique_ptr<net::Discovery> discovery_;
    QPointer<net::ControlChannel> channel_;
    QPointer<QTcpSocket> pendingSocket_;
    State state_ = State::Idle;
    std::optional<media::MediaCaps> remoteCaps_;
    std::optional<Selections> sendSel_;
    std::optional<Selections> recvSel_;
    media::NegotiationPolicy cameraPolicy_;
    media::NegotiationPolicy screenPolicy_;
    QString lastError_;
    QStringList log_;
    std::unique_ptr<MediaSession> media_;
    uint16_t remoteMediaPort_ = 0;
    QTimer mediaStatsTimer_;
    QString mediaStats_;
    bool micMuted_ = false;
    bool sharingScreen_ = false;
    bool remoteSharing_ = false;
    QString screenMode_;
};

} // namespace p2p::app
