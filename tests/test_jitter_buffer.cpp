#include "audio/jitter_buffer.h"

#include <QTest>

using namespace p2p::audio;
using Action = JitterBuffer::Action;

namespace {
constexpr int64_t kFrameUs = 10'000;

EncodedFrame frame(uint32_t seq, int64_t arrivalUs)
{
    EncodedFrame f;
    f.seq = seq;
    f.timestamp = seq * 480;
    f.arrivalUs = arrivalUs;
    f.data = {uint8_t(seq & 0xff)};
    return f;
}
} // namespace

class TestJitterBuffer : public QObject {
    Q_OBJECT
private slots:
    void silenceUntilStarted()
    {
        JitterBuffer jb;
        QCOMPARE(jb.pull().action, Action::Silence);
        jb.insert(frame(100, 0));
        auto r = jb.pull();
        QCOMPARE(r.action, Action::Decode); // LAN target = 1 frame
        QCOMPARE(r.frame.seq, 100u);
    }

    void inOrderPlayout()
    {
        JitterBuffer jb;
        for (uint32_t s = 0; s < 50; ++s) {
            jb.insert(frame(s, s * kFrameUs));
            auto r = jb.pull();
            QCOMPARE(r.action, Action::Decode);
            QCOMPARE(r.frame.seq, s);
        }
        QCOMPARE(jb.targetFrames(), 1);
        QCOMPARE(jb.stats().concealed, 0u);
        QCOMPARE(jb.stats().underruns, 0u);
    }

    void lossIsConcealedWithNextFrame()
    {
        JitterBuffer jb;
        jb.insert(frame(0, 0));
        QCOMPARE(jb.pull().action, Action::Decode);
        // seq 1 lost, 2 arrives
        jb.insert(frame(2, 2 * kFrameUs));
        auto r = jb.pull();
        QCOMPARE(r.action, Action::Conceal);
        QCOMPARE(r.frame.seq, 2u);   // handed over for FEC/DRED
        QCOMPARE(r.distance, 1);
        r = jb.pull();
        QCOMPARE(r.action, Action::Decode);
        QCOMPARE(r.frame.seq, 2u);
        QCOMPARE(jb.stats().concealed, 1u);
        // the lost packet turning up late is discarded
        jb.insert(frame(1, 3 * kFrameUs));
        QCOMPARE(jb.stats().late, 1u);
    }

    void burstLossReportsDistance()
    {
        JitterBuffer jb;
        jb.insert(frame(0, 0));
        jb.pull();
        jb.insert(frame(4, 4 * kFrameUs)); // 1,2,3 lost
        for (int d = 3; d >= 1; --d) {
            auto r = jb.pull();
            QCOMPARE(r.action, Action::Conceal);
            QCOMPARE(r.distance, d);
        }
        QCOMPARE(jb.pull().frame.seq, 4u);
    }

    void lateArrivalAfterUnderrunRaisesTarget()
    {
        JitterBuffer jb;
        jb.insert(frame(0, 0));
        jb.pull();
        QCOMPARE(jb.pull().action, Action::Underrun); // seq 1 not here yet, nothing newer
        QCOMPARE(jb.targetFrames(), 1);               // not yet known to be late
        jb.insert(frame(1, 12'000));                  // 2 ms late: jitter alone keeps target 1
        QCOMPARE(jb.targetFrames(), 2);
        QCOMPARE(jb.stats().lateAfterWait, 1u);
        auto r = jb.pull();
        QCOMPARE(r.action, Action::Decode);
        QCOMPARE(r.frame.seq, 1u);
    }

    void lossAfterUnderrunDoesNotRaiseTarget()
    {
        JitterBuffer jb;
        jb.insert(frame(0, 0));
        jb.pull();
        QCOMPARE(jb.pull().action, Action::Underrun); // seq 1 awaited...
        jb.insert(frame(2, 20'000));                  // ...but 2 shows up: 1 was lost
        auto r = jb.pull();
        QCOMPARE(r.action, Action::Conceal);
        QCOMPARE(r.distance, 1);
        QCOMPARE(jb.pull().frame.seq, 2u);
        jb.insert(frame(1, 21'000)); // straggler is just late, no boost
        QCOMPARE(jb.targetFrames(), 1);
        QCOMPARE(jb.stats().late, 1u);
    }

    void jitterRaisesTarget()
    {
        JitterBuffer jb;
        // Every 4th packet is 35 ms late.
        for (uint32_t s = 0; s < 200; ++s)
            jb.insert(frame(s, s * kFrameUs + (s % 4 == 0 ? 35'000 : 0)));
        QVERIFY2(jb.jitterP95Ms() > 30, qPrintable(QString::number(jb.jitterP95Ms())));
        QCOMPARE(jb.targetFrames(), 4); // ceil((35 + 2) / 10)
    }

    void excessDepthIsTrimmed()
    {
        JitterBuffer jb;
        // A burst of 10 frames arrives at once, then steady flow (and nobody
        // drains it, unlike the real playout resampler). The burst
        // reads as ~90 ms of jitter until it leaves the 200-packet window,
        // after which the target falls back and the excess is trimmed.
        for (uint32_t s = 0; s < 10; ++s) jb.insert(frame(s, 0));
        uint32_t next = 10;
        for (int i = 0; i < 500; ++i) {
            jb.insert(frame(next, int64_t(next) * kFrameUs));
            ++next;
            jb.pull();
        }
        QVERIFY(jb.stats().dropped > 0);
        QCOMPARE(jb.targetFrames(), 1);
        QVERIFY(jb.levelFrames() <= jb.targetFrames() + 6);
    }

    void startupBacklogIsDiscarded()
    {
        // 3 s of steadily paced audio queues up before playout starts (the
        // first LAN call on the Mac): play from the newest, not the oldest.
        JitterBuffer jb;
        for (uint32_t s = 0; s < 300; ++s) jb.insert(frame(s, int64_t(s) * kFrameUs));
        const int target = jb.targetFrames();
        auto r = jb.pull();
        QCOMPARE(r.action, Action::Decode);
        QCOMPARE(r.frame.seq, uint32_t(300 - target));
        QCOMPARE(jb.stats().dropped, uint64_t(300 - target));
        QCOMPARE(jb.levelFrames(), target - 1);
    }

    void stallBacklogIsDiscarded()
    {
        // Steady flow, then a 1 s stall whose audio all arrives at once. It
        // reads as jitter (the target rises), but the excess beyond that goes
        // in one step rather than draining for seconds.
        JitterBuffer jb;
        uint32_t next = 0;
        for (; next < 300; ++next) {
            jb.insert(frame(next, int64_t(next) * kFrameUs));
            jb.pull();
        }
        for (int i = 0; i < 100; ++i) jb.pull(); // underruns hold the playhead
        const int64_t burstAt = int64_t(next + 100) * kFrameUs;
        for (int i = 0; i < 100; ++i, ++next) jb.insert(frame(next, burstAt));
        const int target = jb.targetFrames();
        auto r = jb.pull();
        QCOMPARE(r.action, Action::Decode);
        QCOMPARE(r.frame.seq, next - uint32_t(target));
        QCOMPARE(jb.levelFrames(), target - 1);
        QCOMPARE(jb.stats().dropped, uint64_t(100 - target));
    }

    void duplicatesIgnored()
    {
        JitterBuffer jb;
        jb.insert(frame(0, 0));
        jb.insert(frame(0, 1));
        QCOMPARE(jb.stats().duplicates, 1u);
    }

    void largeJumpResets()
    {
        JitterBuffer jb;
        jb.insert(frame(0, 0));
        jb.pull();
        jb.insert(frame(100000, 1));
        QCOMPARE(jb.stats().resets, 1u);
        QCOMPARE(jb.pull().frame.seq, 100000u);
    }
};

QTEST_GUILESS_MAIN(TestJitterBuffer)
#include "test_jitter_buffer.moc"
