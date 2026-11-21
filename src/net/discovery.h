#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

namespace p2p::net {

// Minimal LAN discovery: each instance multicasts a small JSON beacon once
// a second on 239.255.77.77:47999 and keeps a list of peers seen in the last
// few seconds. Multicast loopback is enabled so several instances on one
// machine can find each other. (DNS-SD/Bonjour interop can come later.)
class Discovery : public QObject {
    Q_OBJECT
public:
    struct Peer {
        QString id;
        QString name;
        QHostAddress host;
        quint16 port = 0;
        qint64 lastSeenMs = 0;
    };

    Discovery(QString instanceId, QString name, quint16 controlPort, QObject* parent = nullptr);
    bool start();
    QList<Peer> peers() const { return peers_; }

signals:
    void peersChanged();

private:
    void sendBeacon();
    void onReadyRead();
    void expire();

    QString id_;
    QString name_;
    quint16 port_;
    QUdpSocket sock_;
    QTimer beaconTimer_;
    QList<Peer> peers_;
};

} // namespace p2p::net
