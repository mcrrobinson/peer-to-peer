#pragma once

#include "net/crypto.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QTcpSocket;

namespace p2p::net {

// Reliable, encrypted signalling channel between two peers over TCP.
//
// Wire format: [u32 big-endian length][payload]. Handshake frames carry
// plaintext JSON (see crypto.h for the commit/reveal scheme); every frame
// after the handshake is ChaCha20-Poly1305 sealed JSON.
//
// The channel also runs a ping/pong loop to estimate RTT and the offset
// between the two peers' monotonic clocks (NTP-style, min-RTT filtered).
class ControlChannel : public QObject {
    Q_OBJECT
public:
    enum class Role { Initiator, Responder };
    static constexpr int kProtocolVersion = 1;

    // Takes ownership of |socket|, which must already be connected.
    ControlChannel(QTcpSocket* socket, Role role, QString localName, QObject* parent = nullptr);
    ~ControlChannel() override;

    void start();
    void send(const QJsonObject& msg);
    // Flushes pending writes, then disconnects. Emits closed(reason) once.
    void close(const QString& reason);

    Role role() const { return role_; }
    bool isEstablished() const { return hs_ == Handshake::Done; }
    QString remoteName() const { return remoteName_; }
    QString sas() const { return keys_.sas; }
    const crypto::SessionKeys& keys() const { return keys_; }
    QHostAddress peerAddress() const;

    // Latest estimates; -1 rtt means "no sample yet".
    double rttMs() const { return rttUs_ < 0 ? -1.0 : rttUs_ / 1000.0; }
    // remote_clock ~= local_clock + offset
    double clockOffsetMs() const { return offsetUs_ / 1000.0; }

signals:
    void established();
    void message(const QJsonObject& msg);
    void statsUpdated();
    void closed(const QString& reason);

private:
    enum class Handshake { WaitCommit, WaitHello, WaitReveal, Done };

    void onReadyRead();
    void handleFrame(const QByteArray& frame);
    void handleHandshake(const QJsonObject& msg);
    void handleSecure(const QJsonObject& msg);
    void finishHandshake(const QByteArray& remotePk);
    void sendFrame(const QByteArray& payload);
    void sendPlain(const QJsonObject& msg);
    void sendPing();
    void fail(const QString& reason);

    QTcpSocket* sock_;
    Role role_;
    QString localName_;
    QString remoteName_;
    Handshake hs_;
    crypto::KeyPair localKeys_;
    QByteArray remoteCommit_;
    QByteArray remotePk_;
    crypto::SessionKeys keys_;
    crypto::AeadStream aead_;
    QByteArray rxBuf_;
    bool closed_ = false;

    QTimer handshakeTimer_;
    QTimer pingTimer_;
    int pingsSent_ = 0;
    int64_t rttUs_ = -1;
    int64_t offsetUs_ = 0;
    int64_t bestRttUs_ = -1;      // min-RTT sample used for the offset
    int64_t bestRttAtUs_ = 0;
};

} // namespace p2p::net
