#include "net/discovery.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkDatagram>

Q_LOGGING_CATEGORY(lcDisc, "p2p.discovery")

namespace p2p::net {

namespace {
const QHostAddress kGroup(QStringLiteral("239.255.77.77"));
constexpr quint16 kPort = 47999;
constexpr int kBeaconMs = 1000;
constexpr qint64 kExpireMs = 3500;
} // namespace

Discovery::Discovery(QString instanceId, QString name, quint16 controlPort, QObject* parent)
    : QObject(parent), id_(std::move(instanceId)), name_(std::move(name)), port_(controlPort)
{
    connect(&sock_, &QUdpSocket::readyRead, this, &Discovery::onReadyRead);
    connect(&beaconTimer_, &QTimer::timeout, this, [this] {
        sendBeacon();
        expire();
    });
}

bool Discovery::start()
{
    if (!sock_.bind(QHostAddress::AnyIPv4, kPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qCWarning(lcDisc) << "bind failed:" << sock_.errorString();
        return false;
    }
    if (!sock_.joinMulticastGroup(kGroup))
        qCWarning(lcDisc) << "joinMulticastGroup failed:" << sock_.errorString();
    sock_.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    sock_.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    sendBeacon();
    beaconTimer_.start(kBeaconMs);
    return true;
}

void Discovery::sendBeacon()
{
    const QJsonObject b{{"app", "p2pcall"}, {"v", 1}, {"id", id_}, {"name", name_}, {"port", port_}};
    sock_.writeDatagram(QJsonDocument(b).toJson(QJsonDocument::Compact), kGroup, kPort);
}

void Discovery::onReadyRead()
{
    bool changed = false;
    while (sock_.hasPendingDatagrams()) {
        const QNetworkDatagram dg = sock_.receiveDatagram(2048);
        const QJsonObject o = QJsonDocument::fromJson(dg.data()).object();
        if (o.value("app").toString() != "p2pcall" || o.value("v").toInt() != 1)
            continue;
        const QString id = o.value("id").toString();
        const int port = o.value("port").toInt();
        if (id.isEmpty() || id == id_ || port <= 0 || port > 65535)
            continue;

        QHostAddress host = dg.senderAddress();
        if (host.protocol() == QAbstractSocket::IPv6Protocol) {
            bool ok = false;
            const quint32 v4 = host.toIPv4Address(&ok);
            if (ok) host = QHostAddress(v4);
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto it = std::find_if(peers_.begin(), peers_.end(), [&](const Peer& p) { return p.id == id; });
        if (it == peers_.end()) {
            peers_.append({id, o.value("name").toString().left(64), host, quint16(port), now});
            changed = true;
        } else {
            if (it->host != host || it->port != port) changed = true;
            it->host = host;
            it->port = quint16(port);
            it->lastSeenMs = now;
        }
    }
    if (changed)
        emit peersChanged();
}

void Discovery::expire()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto n = peers_.removeIf([&](const Peer& p) { return now - p.lastSeenMs > kExpireMs; });
    if (n > 0)
        emit peersChanged();
}

} // namespace p2p::net
