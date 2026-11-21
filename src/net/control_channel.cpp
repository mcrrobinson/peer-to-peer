#include "net/control_channel.h"

#include "core/clock.h"

#include <QJsonDocument>
#include <QLoggingCategory>
#include <QTcpSocket>
#include <QtEndian>

#include <sodium.h>

Q_LOGGING_CATEGORY(lcCtrl, "p2p.ctrl")

namespace p2p::net {

namespace {
constexpr quint32 kMaxFrame = 1 << 20;
constexpr int kHandshakeTimeoutMs = 5000;
constexpr int kPingIntervalMs = 1000;
// A quick burst right after the handshake gets a low-RTT clock-offset
// sample early, even if the first ones are delayed by call start-up work.
constexpr int kPingBurstCount = 5;
constexpr int kPingBurstIntervalMs = 100;
// Re-pick the clock-offset reference sample if the best one is this old.
constexpr int64_t kOffsetSampleMaxAgeUs = 10'000'000;

QString b64(const QByteArray& b) { return QString::fromLatin1(b.toBase64()); }
QByteArray unb64(const QJsonValue& v) { return QByteArray::fromBase64(v.toString().toLatin1()); }
} // namespace

ControlChannel::ControlChannel(QTcpSocket* socket, Role role, QString localName, QObject* parent)
    : QObject(parent)
    , sock_(socket)
    , role_(role)
    , localName_(std::move(localName))
    , hs_(role == Role::Initiator ? Handshake::WaitHello : Handshake::WaitCommit)
    , localKeys_(crypto::KeyPair::generate())
{
    sock_->setParent(this);
    sock_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(sock_, &QTcpSocket::readyRead, this, &ControlChannel::onReadyRead);
    connect(sock_, &QTcpSocket::disconnected, this, [this] { fail(QStringLiteral("peer disconnected")); });
    connect(sock_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        if (err != QAbstractSocket::RemoteHostClosedError)
            fail(sock_->errorString());
    });

    handshakeTimer_.setSingleShot(true);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this] { fail(QStringLiteral("handshake timeout")); });
    connect(&pingTimer_, &QTimer::timeout, this, [this] {
        sendPing();
        if (++pingsSent_ == kPingBurstCount) pingTimer_.setInterval(kPingIntervalMs);
    });
}

ControlChannel::~ControlChannel()
{
    sodium_memzero(localKeys_.sk.data(), localKeys_.sk.size());
    sodium_memzero(keys_.rx.data(), keys_.rx.size());
    sodium_memzero(keys_.tx.data(), keys_.tx.size());
}

QHostAddress ControlChannel::peerAddress() const
{
    return sock_->peerAddress();
}

void ControlChannel::start()
{
    handshakeTimer_.start(kHandshakeTimeoutMs);
    if (role_ == Role::Initiator) {
        sendPlain({{"t", "commit"},
                   {"v", kProtocolVersion},
                   {"name", localName_},
                   {"h", b64(crypto::commitment(localKeys_.publicKey()))}});
    }
    // Data may already be buffered if the peer was fast.
    if (sock_->bytesAvailable() > 0)
        onReadyRead();
}

void ControlChannel::send(const QJsonObject& msg)
{
    if (closed_ || hs_ != Handshake::Done)
        return;
    sendFrame(aead_.seal(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void ControlChannel::close(const QString& reason)
{
    if (closed_)
        return;
    closed_ = true;
    pingTimer_.stop();
    handshakeTimer_.stop();
    sock_->disconnectFromHost(); // flushes pending writes
    emit closed(reason);
}

void ControlChannel::fail(const QString& reason)
{
    if (closed_)
        return;
    qCInfo(lcCtrl) << "channel closed:" << reason;
    closed_ = true;
    pingTimer_.stop();
    handshakeTimer_.stop();
    sock_->abort();
    emit closed(reason);
}

void ControlChannel::sendFrame(const QByteArray& payload)
{
    char hdr[4];
    qToBigEndian<quint32>(quint32(payload.size()), hdr);
    sock_->write(hdr, 4);
    sock_->write(payload);
}

void ControlChannel::sendPlain(const QJsonObject& msg)
{
    sendFrame(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void ControlChannel::onReadyRead()
{
    rxBuf_.append(sock_->readAll());
    while (!closed_) {
        if (rxBuf_.size() < 4)
            return;
        const quint32 len = qFromBigEndian<quint32>(rxBuf_.constData());
        if (len > kMaxFrame) {
            fail(QStringLiteral("frame too large"));
            return;
        }
        if (quint32(rxBuf_.size()) < 4 + len)
            return;
        const QByteArray frame = rxBuf_.mid(4, int(len));
        rxBuf_.remove(0, int(4 + len));
        handleFrame(frame);
    }
}

void ControlChannel::handleFrame(const QByteArray& frame)
{
    QByteArray plain;
    if (hs_ == Handshake::Done) {
        auto opened = aead_.open(frame);
        if (!opened) {
            fail(QStringLiteral("authentication failure"));
            return;
        }
        plain = *opened;
    } else {
        plain = frame;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(plain, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(QStringLiteral("protocol error: bad json"));
        return;
    }
    if (hs_ == Handshake::Done)
        handleSecure(doc.object());
    else
        handleHandshake(doc.object());
}

void ControlChannel::handleHandshake(const QJsonObject& msg)
{
    const QString t = msg.value("t").toString();
    switch (hs_) {
    case Handshake::WaitCommit: // responder
        if (t != "commit") return fail(QStringLiteral("protocol error: expected commit"));
        if (msg.value("v").toInt() != kProtocolVersion)
            return fail(QStringLiteral("protocol version mismatch"));
        remoteName_ = msg.value("name").toString().left(64);
        remoteCommit_ = unb64(msg.value("h"));
        if (remoteCommit_.size() != int(crypto_generichash_BYTES))
            return fail(QStringLiteral("protocol error: bad commitment"));
        sendPlain({{"t", "hello"},
                   {"v", kProtocolVersion},
                   {"name", localName_},
                   {"pk", b64(localKeys_.publicKey())}});
        hs_ = Handshake::WaitReveal;
        return;

    case Handshake::WaitHello: { // initiator
        if (t != "hello") return fail(QStringLiteral("protocol error: expected hello"));
        if (msg.value("v").toInt() != kProtocolVersion)
            return fail(QStringLiteral("protocol version mismatch"));
        remoteName_ = msg.value("name").toString().left(64);
        const QByteArray pk = unb64(msg.value("pk"));
        sendPlain({{"t", "reveal"}, {"pk", b64(localKeys_.publicKey())}});
        finishHandshake(pk);
        return;
    }

    case Handshake::WaitReveal: { // responder
        if (t != "reveal") return fail(QStringLiteral("protocol error: expected reveal"));
        const QByteArray pk = unb64(msg.value("pk"));
        if (sodium_memcmp(crypto::commitment(pk).constData(), remoteCommit_.constData(),
                          crypto_generichash_BYTES) != 0)
            return fail(QStringLiteral("commitment mismatch"));
        finishHandshake(pk);
        return;
    }

    case Handshake::Done:
        return;
    }
}

void ControlChannel::finishHandshake(const QByteArray& remotePk)
{
    auto keys = crypto::deriveSession(role_ == Role::Initiator, localKeys_, remotePk);
    if (!keys)
        return fail(QStringLiteral("key agreement failed"));
    remotePk_ = remotePk;
    keys_ = *keys;
    aead_ = crypto::AeadStream(keys_.tx, keys_.rx);
    sodium_memzero(localKeys_.sk.data(), localKeys_.sk.size());
    hs_ = Handshake::Done;
    handshakeTimer_.stop();
    qCInfo(lcCtrl) << "secure channel established with" << remoteName_ << "SAS" << keys_.sas;
    emit established();
    if (closed_)
        return;
    sendPing();
    pingsSent_ = 1;
    pingTimer_.start(kPingBurstIntervalMs);
}

void ControlChannel::sendPing()
{
    send({{"t", "ping"}, {"t0", QString::number(nowUs())}});
}

void ControlChannel::handleSecure(const QJsonObject& msg)
{
    const QString t = msg.value("t").toString();
    if (t == "ping") {
        const QString t1 = QString::number(nowUs());
        send({{"t", "pong"}, {"t0", msg.value("t0")}, {"t1", t1}, {"t2", QString::number(nowUs())}});
        return;
    }
    if (t == "pong") {
        const int64_t t3 = nowUs();
        const int64_t t0 = msg.value("t0").toString().toLongLong();
        const int64_t t1 = msg.value("t1").toString().toLongLong();
        const int64_t t2 = msg.value("t2").toString().toLongLong();
        const int64_t rtt = (t3 - t0) - (t2 - t1);
        if (t0 <= 0 || rtt < 0)
            return;
        rttUs_ = rtt;
        // Offset from the lowest-RTT recent sample is the least biased.
        if (bestRttUs_ < 0 || rtt <= bestRttUs_ || t3 - bestRttAtUs_ > kOffsetSampleMaxAgeUs) {
            bestRttUs_ = rtt;
            bestRttAtUs_ = t3;
            offsetUs_ = ((t1 - t0) + (t2 - t3)) / 2;
        }
        emit statsUpdated();
        return;
    }
    emit message(msg);
}

} // namespace p2p::net
