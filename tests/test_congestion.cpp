// Congestion control: feedback wire format, and the controller driven by a
// simulated bottleneck link (discrete time, 100 us ticks).
#include "net/cc/congestion_controller.h"
#include "net/cc/transport_feedback.h"
#include "video/rate_ladder.h"

#include <QTest>

#include <algorithm>
#include <deque>
#include <functional>
#include <random>

using namespace p2p::net::cc;
using p2p::video::buildLadder;
using p2p::video::RungSelector;

namespace {

// One direction of media over a bottleneck: a drop-tail queue drained at
// |rateBps|, then |delayUs| of propagation. Feedback comes back after
// |delayUs| without queueing. Video is generated as frame bursts at the
// controller's rate (minus audio), paced when the controller asks.
struct LinkSim {
    struct Config {
        int64_t rateBps = 10'000'000;
        int64_t delayUs = 1'000;
        int64_t queueUs = 100'000;     // drop-tail buffer, in drain time
        int64_t ceThresholdUs = 0;     // L4S step marking (0 = no ECN marks)
        double randomLoss = 0.0;
        int fps = 60;
        CongestionController::Config cc;
    };
    struct Window {
        double avgTargetBps = 0, avgQueueUs = 0, maxQueueUs = 0, lossRate = 0;
        uint64_t sent = 0, delivered = 0, ceMarked = 0;
    };

    explicit LinkSim(Config c) : cfg(c), cc(c.cc), rng(42) {}

    Config cfg;
    CongestionController cc;
    FeedbackBuilder rx;
    std::mt19937 rng;
    int64_t now = 1'000'000;
    uint64_t nextPn = 1;
    int64_t linkFreeUs = 0;
    int64_t nextFrameUs = 0, nextAudioUs = 0, nextFeedbackUs = 0, pacerNextUs = 0;
    std::deque<size_t> pacerQueue;
    struct InFlight { int64_t at; uint64_t pn; uint8_t ecn; };
    std::deque<InFlight> inFlight;
    struct Fb { int64_t at; TransportFeedback fb; };
    std::deque<Fb> feedback;
    Window w;
    double sumQueue = 0;
    uint64_t queueSamples = 0, lost = 0, targetSamples = 0;
    double sumTarget = 0;

    void send(size_t bytes, uint8_t stream)
    {
        const uint64_t pn = nextPn++;
        cc.onPacketSent(pn, now, bytes, stream);
        ++w.sent;
        if (cfg.randomLoss > 0 && std::uniform_real_distribution<>(0, 1)(rng) < cfg.randomLoss) {
            ++lost;
            return;
        }
        const int64_t start = std::max(now, linkFreeUs);
        const int64_t queued = start - now;
        if (queued > cfg.queueUs) {
            ++lost;
            return;
        }
        linkFreeUs = start + int64_t(double(bytes) * 8e6 / double(cfg.rateBps));
        const int64_t twoPackets = int64_t(2 * 1468 * 8e6 / double(cfg.rateBps));
        const bool ce = cfg.ceThresholdUs > 0 && queued > std::max(cfg.ceThresholdUs, twoPackets);
        if (ce) ++w.ceMarked;
        inFlight.push_back({linkFreeUs + cfg.delayUs, pn, uint8_t(ce ? 3 : 1)});
        sumQueue += double(queued);
        w.maxQueueUs = std::max(w.maxQueueUs, double(queued));
        ++queueSamples;
    }

    void step()
    {
        if (now >= nextAudioUs) {
            send(120, 0);
            nextAudioUs = now + 10'000;
        }
        if (now >= nextFrameUs) {
            const double videoBps = std::max(0.0, double(cc.targetBps() - cc.streamBps(0)));
            size_t bytes = size_t(videoBps / 8.0 / cfg.fps);
            while (bytes > 0) {
                const size_t n = std::min<size_t>(bytes, 1200);
                pacerQueue.push_back(n + 40);
                bytes -= n;
            }
            nextFrameUs = now + 1'000'000 / cfg.fps;
        }
        const int64_t pace = cc.pacingBps(now);
        while (!pacerQueue.empty() && (pace == 0 || pacerNextUs <= now)) {
            const size_t n = pacerQueue.front();
            pacerQueue.pop_front();
            send(n, 1);
            if (pace) pacerNextUs = std::max(pacerNextUs, now - 1'000) + int64_t(double(n) * 8e6 / double(pace));
        }
        while (!inFlight.empty() && inFlight.front().at <= now) {
            rx.onPacket(inFlight.front().pn, now, inFlight.front().ecn);
            ++w.delivered;
            inFlight.pop_front();
        }
        if (now >= nextFeedbackUs && rx.hasNew()) {
            if (auto fb = rx.build(now, TransportFeedback::maxEntries(1400))) feedback.push_back({now + cfg.delayUs, *fb});
            nextFeedbackUs = now + 10'000;
        }
        while (!feedback.empty() && feedback.front().at <= now) {
            cc.onFeedback(feedback.front().fb, now);
            feedback.pop_front();
        }
        sumTarget += double(cc.targetBps());
        ++targetSamples;
        now += 100;
    }

    // Runs for |us| and returns stats for that stretch only.
    Window run(int64_t us)
    {
        w = {};
        sumQueue = sumTarget = 0;
        queueSamples = targetSamples = lost = 0;
        const int64_t end = now + us;
        while (now < end) step();
        w.avgTargetBps = targetSamples ? sumTarget / double(targetSamples) : 0;
        w.avgQueueUs = queueSamples ? sumQueue / double(queueSamples) : 0;
        w.lossRate = w.sent ? double(lost) / double(w.sent) : 0;
        return w;
    }
};

void report(const char* label, const LinkSim& s, const LinkSim::Window& w)
{
    const auto st = s.cc.stats(s.now);
    qInfo("%s: target avg %.2f Mbps (now %.2f), queue avg %.1f ms max %.1f ms, loss %.2f%%, ce %llu, "
          "events delay %llu loss %llu ce %llu, srtt %.1f ms",
          label, w.avgTargetBps / 1e6, double(st.targetBps) / 1e6, w.avgQueueUs / 1000, w.maxQueueUs / 1000,
          w.lossRate * 100, (unsigned long long)w.ceMarked, (unsigned long long)st.delayEvents,
          (unsigned long long)st.lossEvents, (unsigned long long)st.ceEvents, double(st.srttUs) / 1000);
}

} // namespace

class TestCongestion : public QObject {
    Q_OBJECT
private slots:
    void feedbackRoundTrip()
    {
        FeedbackBuilder b;
        b.onPacket(10, 1'000'000, 1);
        b.onPacket(12, 1'000'500, 3);
        b.onPacket(11, 1'001'000, 0); // reordered
        QVERIFY(b.hasNew());
        auto fb = b.build(1'010'000, 100);
        QVERIFY(fb);
        QCOMPARE(fb->basePn, 10u);
        QCOMPARE(fb->entries.size(), size_t(3));
        uint8_t buf[1400];
        const size_t n = fb->serialize(buf, sizeof(buf));
        QCOMPARE(n, TransportFeedback::kHeaderSize + 6);
        auto p = TransportFeedback::parse(buf, n);
        QVERIFY(p);
        QCOMPARE(p->reportUs, 1'010'000);
        QVERIFY(p->entries[0].received && p->entries[1].received && p->entries[2].received);
        QCOMPARE(p->entries[1].ecn, uint8_t(0));
        QCOMPARE(p->entries[2].ecn, uint8_t(3));
        QVERIFY(std::abs(p->entries[0].arrivalUs - 1'000'000) <= 32);
        QVERIFY(std::abs(p->entries[1].arrivalUs - 1'001'000) <= 32);
        QVERIFY(!b.hasNew());

        // Next report: a gap at 14, and the overlap repeats 10..12.
        b.onPacket(13, 1'020'000, 1);
        b.onPacket(15, 1'021'000, 1);
        fb = b.build(1'030'000, 100);
        QCOMPARE(fb->basePn, 10u);
        QCOMPARE(fb->entries.size(), size_t(6));
        QVERIFY(fb->entries[3].received);
        QVERIFY(!fb->entries[4].received);
        QVERIFY(fb->entries[5].received);

        // Capped report keeps the newest packets.
        for (uint64_t pn = 16; pn < 500; ++pn) b.onPacket(pn, 1'040'000, 1);
        fb = b.build(1'050'000, 100);
        QCOMPARE(fb->entries.size(), size_t(100));
        QCOMPARE(fb->basePn, 400u);

        QVERIFY(!TransportFeedback::parse(buf, 10));
    }

    void controllerAcksAndDetectsLoss()
    {
        CongestionController cc;
        for (uint64_t pn = 1; pn <= 20; ++pn) cc.onPacketSent(pn, 1'000'000 + int64_t(pn) * 100, 1000, 1);
        TransportFeedback fb;
        fb.reportUs = 5'000'000;
        fb.basePn = 1;
        for (uint64_t pn = 1; pn <= 20; ++pn) {
            FeedbackEntry e;
            e.pn = pn;
            e.received = pn != 5;
            e.arrivalUs = 4'990'000 + int64_t(pn) * 100;
            fb.entries.push_back(e);
        }
        cc.onFeedback(fb, 1'012'000);
        auto s = cc.stats(1'012'000);
        QCOMPARE(s.acked, 19u);
        QCOMPARE(s.lost, 1u);
        QVERIFY(s.srttUs > 0 && s.srttUs < 20'000);
        // The "lost" packet turns up in an overlapping report: reordering.
        fb.entries[4].received = true;
        cc.onFeedback(fb, 1'013'000);
        s = cc.stats(1'013'000);
        QCOMPARE(s.lost, 0u);
        QCOMPARE(s.spuriousLost, 1u);
        QCOMPARE(s.acked, 20u);
    }

    void convergesToBottleneck()
    {
        LinkSim::Config c;
        c.rateBps = 6'000'000;
        c.delayUs = 2'000;
        c.cc.maxBps = 12'000'000;
        LinkSim s(c);
        report("6M start", s, s.run(3'000'000));
        const auto w = s.run(10'000'000);
        report("6M steady", s, w);
        QVERIFY2(w.avgTargetBps > 0.6 * c.rateBps && w.avgTargetBps < 1.05 * c.rateBps,
                 qPrintable(QString::number(w.avgTargetBps)));
        QVERIFY2(w.avgQueueUs < 25'000, qPrintable(QString::number(w.avgQueueUs)));
        QVERIFY(w.lossRate < 0.01);
        QVERIFY(s.cc.pacingBps(s.now) > 0); // bottleneck seen: pacing on
    }

    void followsCapacityChanges()
    {
        LinkSim::Config c;
        c.rateBps = 10'000'000;
        c.delayUs = 5'000;
        c.cc.maxBps = 12'000'000;
        LinkSim s(c);
        s.run(8'000'000);
        s.cfg.rateBps = 3'000'000;
        auto w = s.run(2'000'000);
        report("drop to 3M (first 2 s)", s, w);
        QVERIFY2(s.cc.targetBps() < 3'300'000, qPrintable(QString::number(s.cc.targetBps())));
        w = s.run(8'000'000);
        report("3M steady", s, w);
        QVERIFY(w.avgQueueUs < 25'000);
        s.cfg.rateBps = 10'000'000;
        w = s.run(15'000'000);
        report("back to 10M (15 s)", s, w);
        QVERIFY2(s.cc.targetBps() > 6'000'000, qPrintable(QString::number(s.cc.targetBps())));
    }

    void l4sKeepsQueueShort()
    {
        LinkSim::Config c;
        c.rateBps = 6'000'000;
        c.delayUs = 2'000;
        c.ceThresholdUs = 2'000;
        c.cc.maxBps = 12'000'000;
        LinkSim s(c);
        s.run(3'000'000);
        const auto w = s.run(10'000'000);
        report("L4S 6M", s, w);
        QVERIFY(w.ceMarked > 0);
        QVERIFY(s.cc.stats(s.now).ceEvents > 0);
        QVERIFY2(w.avgTargetBps > 0.55 * c.rateBps, qPrintable(QString::number(w.avgTargetBps)));
        QVERIFY2(w.avgQueueUs < 10'000, qPrintable(QString::number(w.avgQueueUs)));
        QCOMPARE(w.lossRate, 0.0);
    }

    void randomLossIsNotCongestion()
    {
        LinkSim::Config c;
        c.rateBps = 100'000'000; // never the bottleneck
        c.randomLoss = 0.05;
        c.cc.maxBps = 12'000'000;
        LinkSim s(c);
        const auto w = s.run(10'000'000);
        report("5% random loss, fat pipe", s, w);
        QVERIFY2(s.cc.targetBps() >= 11'000'000, qPrintable(QString::number(s.cc.targetBps())));
        QVERIFY(s.cc.stats(s.now).lossRate > 0.02);
        QCOMPARE(s.cc.pacingBps(s.now), int64_t(0)); // no bottleneck: bursts
    }

    void rampsUpFromLowStart()
    {
        LinkSim::Config c;
        c.rateBps = 50'000'000;
        c.cc.maxBps = 12'000'000;
        c.cc.startBps = 500'000;
        LinkSim s(c);
        s.run(10'000'000);
        report("ramp from 0.5M", s, s.run(1'000'000));
        QVERIFY2(s.cc.targetBps() >= 11'000'000, qPrintable(QString::number(s.cc.targetBps())));
    }

    void ladderShape()
    {
        const auto l = buildLadder(12000, 300, 1920, 1080, 60);
        QVERIFY(l.size() >= 8);
        QCOMPARE(l.front().kbps, 12000);
        QCOMPARE(l.front().width, 1920);
        QCOMPARE(l.front().height, 1080);
        QCOMPARE(l.back().kbps, 300);
        for (size_t i = 1; i < l.size(); ++i) {
            QVERIFY(l[i].kbps < l[i - 1].kbps);
            QVERIFY(l[i].width <= l[i - 1].width);
            QVERIFY(l[i].width % 2 == 0 && l[i].height % 2 == 0);
        }
        QVERIFY(l.back().width < 1920); // low rates drop resolution
        // A rate fixed at or below the minimum is a one-rung ladder.
        QCOMPARE(buildLadder(300, 300, 640, 360, 30).size(), size_t(1));
    }

    void selectorHysteresis()
    {
        RungSelector s(buildLadder(8000, 300, 1280, 720, 30)); // 8000, 5600, 3920, ...
        int64_t t = 1'000'000;
        // Slightly short: waits 300 ms before stepping down.
        QVERIFY(!s.update(7500, t));
        QVERIFY(!s.update(7500, t + 200'000));
        QVERIFY(s.update(7500, t + 300'000));
        QCOMPARE(s.rung().kbps, 5600);
        // Far short: steps straight down to what fits.
        t += 1'000'000;
        QVERIFY(s.update(2000, t));
        QVERIFY(s.rung().kbps <= 2000);
        const int low = s.index();
        // Plenty of room: one rung up after 2 s, not before.
        t += 1'000'000;
        QVERIFY(!s.update(20000, t));
        QVERIFY(!s.update(20000, t + 1'900'000));
        QVERIFY(s.update(20000, t + 2'000'000));
        QCOMPARE(s.index(), low - 1);
        // Room that doesn't cover the next rung with 10% headroom: stay.
        t += 3'000'000;
        const double next = s.ladder()[size_t(s.index() - 1)].kbps;
        QVERIFY(!s.update(next * 1.05, t));
        QVERIFY(!s.update(next * 1.05, t + 5'000'000));
        QCOMPARE(s.switches(), 3u);
    }

    void selectorBacksOffFailedProbes()
    {
        RungSelector s(buildLadder(8000, 300, 1280, 720, 30)); // 8000, 5600, 3920, 2744, 1921, 1345, ...
        int64_t t = 1'000'000;
        auto feed = [&](double kbps, int64_t forUs) {
            for (int64_t end = t + forUs; t < end; t += 33'000) s.update(kbps, t);
        };
        feed(1500, 2'000'000); // settle on 1345
        QCOMPARE(s.rung().kbps, 1345);
        const auto good = s.index();
        // Room appears: probe 1921 after 2 s.
        feed(2200, 2'100'000);
        QCOMPARE(s.rung().kbps, 1921);
        // The probe overloads the link; the controller's cut overshoots to
        // 1000 for a moment (< 0.8 x 1345 but not severe): back to 1345 only.
        feed(1000, 1'000'000);
        QCOMPARE(s.index(), good);
        QCOMPARE(s.failedProbes(), 1u);
        // Room again right away: no retry of 1921 for 10 s.
        feed(2200, 9'000'000);
        QCOMPARE(s.rung().kbps, 1345);
        feed(2200, 3'500'000);
        QCOMPARE(s.rung().kbps, 1921); // retried after the 10 s hold + the usual 2 s of room
        // Fails again: now held for 20 s.
        feed(1000, 500'000);
        QCOMPARE(s.index(), good);
        feed(2200, 15'000'000);
        QCOMPARE(s.rung().kbps, 1345);
        feed(2200, 8'000'000);
        QCOMPARE(s.rung().kbps, 1921);
        // This time it holds for 10 s: success, later probes higher are allowed.
        feed(2200, 10'500'000);
        QCOMPARE(s.rung().kbps, 1921);
        QCOMPARE(s.failedProbes(), 2u);
        // A severe drop still goes straight down past the previous rung.
        feed(400, 300'000);
        QVERIFY(s.rung().kbps <= 400);
    }
};

QTEST_GUILESS_MAIN(TestCongestion)
#include "test_congestion.moc"
