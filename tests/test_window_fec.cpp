#include "video/video_packetizer.h"
#include "video/video_stream.h"
#include "video/window_fec.h"

#include <QtTest>

#include <map>
#include <random>

using namespace p2p::video;

namespace {
constexpr size_t kMax = 1400;
using Packet = std::vector<uint8_t>;

// Data packets of frame |id| (|size| bytes of random data).
std::vector<Packet> frame(uint32_t id, size_t size, std::mt19937& rng, int window)
{
    std::vector<uint8_t> data(size);
    for (auto& b : data) b = uint8_t(rng());
    std::vector<Packet> out;
    PacketizeOptions opt;
    packetizeFrame(id, int64_t(id) * 16'667, id == 0, 1, data.data(), data.size(), kMax - WindowFecEncoder::overhead(window),
                   [&](const uint8_t* p, size_t n) { out.emplace_back(p, p + n); }, opt);
    return out;
}

uint64_t keyOf(const Packet& p)
{
    auto h = FragmentHeader::read(p.data(), p.size());
    return uint64_t(h->frameId) << 16 | h->index;
}
} // namespace

class TestWindowFec : public QObject {
    Q_OBJECT

    // Sends |frames| frames through the code, dropping packets |drop| says
    // to (data and parity: index in send order). Returns {lost data packets,
    // rebuilt}; fails the test if anything rebuilt differs from what was sent.
    struct Result {
        int lost = 0, rebuilt = 0, parity = 0;
        int64_t span = 0;
    };
    static Result run(int frames, std::function<size_t(int)> sizeOf, double rate, int window,
                      std::function<bool(uint64_t seq, bool parity)> drop, std::mt19937& rng)
    {
        WindowFecEncoder enc(window);
        WindowFecDecoder dec;
        std::map<uint64_t, Packet> sent, got;
        Result r;
        uint64_t seq = 0;
        auto deliver = [&](std::vector<Packet> rebuilt) {
            for (auto& p : rebuilt) {
                const uint64_t k = keyOf(p);
                if (got.count(k)) QTest::qFail("rebuilt a packet we had", __FILE__, __LINE__);
                if (sent.at(k) != p) QTest::qFail("rebuilt packet differs", __FILE__, __LINE__);
                got[k] = p;
                ++r.rebuilt;
            }
        };
        for (int f = 0; f < frames; ++f) {
            for (auto& p : frame(uint32_t(f), sizeOf(f), rng, window)) {
                enc.add(uint32_t(f), p.data(), p.size());
                sent[keyOf(p)] = p;
                if (drop(seq++, false)) {
                    ++r.lost;
                    continue;
                }
                got[keyOf(p)] = p;
                deliver(dec.onData(p.data(), p.size()));
            }
            for (auto& q : enc.endFrame(uint32_t(f), rate, kMax)) {
                if (q.size() > kMax) QTest::qFail("parity over the payload limit", __FILE__, __LINE__);
                ++r.parity;
                if (drop(seq++, true)) continue;
                deliver(dec.onParity(q.data(), q.size()));
            }
        }
        r.span = dec.spanUs();
        return r;
    }

private slots:
    void wholeFrameFromLaterParity()
    {
        // Frame 5 (3 packets) is lost outright; frames 6-8 carry parity
        // covering it.
        std::mt19937 rng(1);
        WindowFecEncoder enc(4);
        WindowFecDecoder dec;
        std::map<uint64_t, Packet> sent;
        int rebuilt = 0;
        for (int f = 0; f < 10; ++f) {
            for (auto& p : frame(uint32_t(f), 3000, rng, 4)) {
                enc.add(uint32_t(f), p.data(), p.size());
                sent[keyOf(p)] = p;
                if (f == 5) continue;
                for (auto& x : dec.onData(p.data(), p.size())) {
                    QCOMPARE(x, sent.at(keyOf(x)));
                    ++rebuilt;
                }
            }
            for (auto& q : enc.endFrame(uint32_t(f), 0.5, kMax))
                for (auto& x : dec.onParity(q.data(), q.size())) {
                    QCOMPARE(x, sent.at(keyOf(x)));
                    QCOMPARE(FragmentHeader::read(x.data(), x.size())->frameId, 5u);
                    ++rebuilt;
                }
        }
        QCOMPARE(rebuilt, 3);
        QCOMPARE(dec.recovered(), 3u);
    }

    void fractionalRateOnSmallFrames()
    {
        // One packet per frame and 25% overhead: a parity packet every 4th
        // frame, still able to replace a lost one.
        std::mt19937 rng(2);
        const auto r = run(40, [](int) { return 600; }, 0.25, 4,
                           [](uint64_t seq, bool parity) { return !parity && seq == 13; }, rng);
        QCOMPARE(r.parity, 10);
        QCOMPARE(r.lost, 1);
        QCOMPARE(r.rebuilt, 1);
        QVERIFY(r.span > 0);
    }

    void randomLossNeverWrong()
    {
        // 5% random loss, 20% overhead, mixed frame sizes: rebuilds most
        // losses, and every rebuilt packet is exact (checked in run()).
        std::mt19937 rng(3), lossRng(4);
        std::bernoulli_distribution loss(0.05);
        const auto r = run(1200, [](int f) { return f % 60 == 0 ? 20000 : 1500 + size_t(f % 7) * 400; }, 0.2, 4,
                           [&](uint64_t, bool) { return loss(lossRng); }, rng);
        QVERIFY(r.lost > 50);
        QVERIFY2(r.rebuilt >= r.lost * 8 / 10, qPrintable(QString("%1 of %2").arg(r.rebuilt).arg(r.lost)));
    }

    void resendIsTheSamePacket()
    {
        // A resend (kRetransmit set) counts as the original packet.
        std::mt19937 rng(5);
        WindowFecEncoder enc(2);
        WindowFecDecoder dec;
        auto a = frame(0, 2500, rng, 2);
        for (auto& p : a) enc.add(0, p.data(), p.size());
        auto par = enc.endFrame(0, 1.0, kMax); // 2 parity packets
        QCOMPARE(par.size(), size_t(2));
        // Lost a[0] and a[1]; a[1] comes back as a resend; one parity left.
        Packet resend = a[1];
        resend[16] |= FragmentHeader::kRetransmit;
        QVERIFY(dec.onData(resend.data(), resend.size()).empty());
        auto x = dec.onParity(par[0].data(), par[0].size());
        QCOMPARE(x.size(), size_t(1));
        QCOMPARE(x[0], a[0]);
    }

    void planMatchesPacketizer()
    {
        // planFragments (the hybrid's cost estimate) counts exactly what
        // packetizeFrame sends.
        std::vector<uint8_t> data(200000, 7);
        for (size_t size : {1u, 500u, 1372u, 1373u, 4000u, 45000u, 200000u})
            for (double rate : {0.0, 0.03, 0.07, 0.2, 0.5}) {
                PacketizeOptions opt;
                opt.fecRate = rate;
                int dataN = 0, parityN = 0;
                packetizeFrame(1, 0, false, 1, data.data(), size, 1360,
                               [&](const uint8_t* p, size_t n) {
                                   FragmentHeader::read(p, n)->isParity() ? ++parityN : ++dataN;
                               },
                               opt);
                const auto plan = planFragments(size, 1360, opt);
                QCOMPARE(int(plan.count), dataN);
                QCOMPARE(plan.parityTotal(), parityN);
            }
    }

    void hybridDecision()
    {
        // Rounded-up parity vs the spare budget.
        const size_t mp = 1360, chunk = mp - FragmentHeader::kSize - 2;
        QVERIFY(!perFrameFecFits(3 * chunk, mp, 0.0, 1.0));         // no FEC wanted
        QVERIFY(!perFrameFecFits(1 * chunk, mp, 0.07, 0.5));        // 1 parity / 1 packet = 100%
        QVERIFY(perFrameFecFits(1 * chunk, mp, 0.07, 1.0));         // ... fits a 100% headroom
        QVERIFY(perFrameFecFits(3 * chunk, mp, 0.07, 0.34));        // 1/3
        QVERIFY(!perFrameFecFits(3 * chunk, mp, 0.07, 0.30));
        QVERIFY(perFrameFecFits(32 * chunk, mp, 0.07, 0.1));        // big frame: 3/32
        QVERIFY(!perFrameFecFits(32 * chunk, mp, 0.07, 0.05));
    }

    void zeroRateSendsNothing()
    {
        std::mt19937 rng(6);
        const auto r = run(20, [](int) { return 3000; }, 0.0, 4, [](uint64_t, bool) { return false; }, rng);
        QCOMPARE(r.parity, 0);
    }
};

QTEST_GUILESS_MAIN(TestWindowFec)
#include "test_window_fec.moc"
