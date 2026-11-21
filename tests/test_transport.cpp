#include "net/media_transport.h"

#include <QTest>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>

using namespace p2p;
using namespace p2p::net;

namespace {
struct Received {
    std::mutex m;
    std::vector<std::vector<uint8_t>> packets;
    size_t size() { std::lock_guard l(m); return packets.size(); }
};

struct Pair {
    MediaTransport a, b;
    Received rxA, rxB;
    bool setup(bool mismatchedKeys = false)
    {
        crypto::Key k1{}, k2{}, k3{};
        k1.fill(1); k2.fill(2); k3.fill(3);
        if (!a.open(0, nullptr) || !b.open(0, nullptr)) return false;
        a.setKeys(k1, k2);
        b.setKeys(k2, mismatchedKeys ? k3 : k1);
        a.setRemote(QHostAddress::LocalHost, b.localPort());
        b.setRemote(QHostAddress::LocalHost, a.localPort());
        auto sink = [](Received& r) {
            return [&r](const uint8_t* d, size_t n, int64_t) {
                std::lock_guard l(r.m);
                r.packets.emplace_back(d, d + n);
            };
        };
        a.setHandler(wire::kAudio, sink(rxA));
        b.setHandler(wire::kAudio, sink(rxB));
        return a.start() && b.start();
    }
};
} // namespace

class TestTransport : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }

    void replayWindow()
    {
        ReplayWindow w;
        QVERIFY(!w.check(0));
        QVERIFY(w.check(5));
        w.mark(5);
        QVERIFY(!w.check(5));
        QVERIFY(w.check(3));  // reordered, inside window
        w.mark(3);
        QVERIFY(!w.check(3));
        w.mark(5000);
        QVERIFY(!w.check(5000 - ReplayWindow::kSize)); // too old
        QVERIFY(w.check(5000 - ReplayWindow::kSize + 1));
        QVERIFY(!w.check(4)); // far behind the window
    }

    void roundTripBothDirections()
    {
        Pair p;
        QVERIFY(p.setup());
        const uint8_t msg[] = {1, 2, 3, 4, 5};
        for (int i = 0; i < 100; ++i) {
            QVERIFY(p.a.send(wire::kMedia, wire::kAudio, msg, sizeof(msg)));
            QVERIFY(p.b.send(wire::kMedia, wire::kAudio, msg, sizeof(msg)));
        }
        QTRY_COMPARE(p.rxB.size(), size_t(100));
        QTRY_COMPARE(p.rxA.size(), size_t(100));
        QCOMPARE(p.rxB.packets.front(), std::vector<uint8_t>(msg, msg + sizeof(msg)));
        QCOMPARE(p.b.stats().authFailures, 0u);
    }

    void wrongKeyRejected()
    {
        Pair p;
        QVERIFY(p.setup(/*mismatchedKeys=*/true));
        const uint8_t msg[] = {9};
        p.a.send(wire::kMedia, wire::kAudio, msg, 1);
        QTRY_COMPARE(p.b.stats().authFailures, 1u);
        QCOMPARE(p.rxB.size(), size_t(0));
    }

    void replayAndTamperRejected()
    {
        Pair p;
        QVERIFY(p.setup());
        // Capture a genuine packet from A by pointing A at a raw socket.
        int raw = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        QCOMPARE(::bind(raw, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
        socklen_t len = sizeof(sa);
        ::getsockname(raw, reinterpret_cast<sockaddr*>(&sa), &len);

        MediaTransport sender;
        QVERIFY(sender.open(0, nullptr));
        crypto::Key bRx{}, unused{};
        bRx.fill(1); // Pair::setup gives B an rx key of all-ones
        sender.setKeys(bRx, unused);
        sender.setRemote(QHostAddress::LocalHost, ntohs(sa.sin_port));
        const uint8_t msg[] = {7, 7, 7};
        QVERIFY(sender.send(wire::kMedia, wire::kAudio, msg, sizeof(msg)));
        uint8_t pkt[2048];
        const ssize_t n = ::recv(raw, pkt, sizeof(pkt), 0);
        QVERIFY(n > 0);
        ::close(raw);

        // Forward it to B twice (replay), then a tampered copy with a fresh pn.
        int out = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dst.sin_port = htons(p.b.localPort());
        ::sendto(out, pkt, size_t(n), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        ::sendto(out, pkt, size_t(n), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        QTRY_COMPARE(p.rxB.size(), size_t(1));
        QTRY_COMPARE(p.b.stats().replays, 1u);

        pkt[11] ^= 0x40;        // new packet number: passes replay, fails AEAD (AD covers header)
        pkt[n - 1] ^= 0x01;
        ::sendto(out, pkt, size_t(n), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        QTRY_COMPARE(p.b.stats().authFailures, 1u);
        QCOMPARE(p.rxB.size(), size_t(1));
        ::close(out);
    }

    void oversizedPayloadRefused()
    {
        Pair p;
        QVERIFY(p.setup());
        std::vector<uint8_t> big(wire::kMaxPayload + 1);
        QVERIFY(!p.a.send(wire::kMedia, wire::kAudio, big.data(), big.size()));
    }

    void simulatedLoss()
    {
        Pair p;
        QVERIFY(p.setup());
        p.a.setSimulatedLoss(0.5);
        const uint8_t msg[] = {1};
        for (int i = 0; i < 1000; ++i) p.a.send(wire::kMedia, wire::kAudio, msg, 1);
        const auto dropped = p.a.stats().simDropped;
        QVERIFY2(dropped > 400 && dropped < 600, qPrintable(QString::number(dropped)));
        QTRY_COMPARE(p.rxB.size(), size_t(1000 - dropped));
    }
};

QTEST_GUILESS_MAIN(TestTransport)
#include "test_transport.moc"
