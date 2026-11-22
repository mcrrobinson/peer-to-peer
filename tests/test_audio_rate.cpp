#include "audio/audio_rate.h"

#include <QTest>

using p2p::audio::AudioRateController;

namespace {
constexpr int64_t kOverhead = 57'600; // 72-byte headers at 100 packets/s
constexpr int64_t kVideoMin = 300'000;
constexpr int64_t kSec = 1'000'000;

AudioRateController make()
{
    AudioRateController::Config c;
    c.minBps = 12'000;
    c.maxBps = 48'000;
    c.overheadBps = kOverhead;
    return AudioRateController(c);
}
} // namespace

class TestAudioRate : public QObject {
    Q_OBJECT
private slots:
    void startsAtMaximumAndStaysThereOnAFastLink()
    {
        auto r = make();
        QCOMPARE(r.bitrate(), 48'000);
        for (int i = 0; i < 100; ++i) QVERIFY(!r.update(5'000'000, kVideoMin, i * 100'000));
        QCOMPARE(r.bitrate(), 48'000);
    }

    void dropsAtOnceToTheLargestStepThatFits()
    {
        auto r = make();
        // Controller floor: video min + 80 kbps -> 22.4 kbps left for speech.
        QVERIFY(r.update(kVideoMin + 80'000, kVideoMin, 0));
        QCOMPARE(r.bitrate(), 20'000);
    }

    void neverGoesBelowTheMinimum()
    {
        auto r = make();
        r.update(100'000, kVideoMin, 0);
        QCOMPARE(r.bitrate(), 12'000);
    }

    void climbsOneStepPerHold()
    {
        auto r = make();
        r.update(kVideoMin + 80'000, kVideoMin, 0);
        QCOMPARE(r.bitrate(), 20'000);
        // Plenty of room again: one step every 2 s, not straight to the top.
        int64_t t = kSec;
        QVERIFY(!r.update(5'000'000, kVideoMin, t));
        QVERIFY(!r.update(5'000'000, kVideoMin, t + kSec));
        QVERIFY(r.update(5'000'000, kVideoMin, t + 2 * kSec));
        QCOMPARE(r.bitrate(), 24'000);
        QVERIFY(r.update(5'000'000, kVideoMin, t + 4 * kSec));
        QCOMPARE(r.bitrate(), 32'000);
    }

    void aDipRestartsTheHold()
    {
        auto r = make();
        r.update(kVideoMin + 80'000, kVideoMin, 0); // 20k
        r.update(5'000'000, kVideoMin, kSec);
        r.update(kVideoMin + 80'000, kVideoMin, 2 * kSec); // back to the same allowance
        QVERIFY(!r.update(5'000'000, kVideoMin, 3 * kSec));
        QVERIFY(!r.update(5'000'000, kVideoMin, 4 * kSec));
        QCOMPARE(r.bitrate(), 20'000);
        QVERIFY(r.update(5'000'000, kVideoMin, 5 * kSec));
    }

    void audioOnlyCallsKeepTheMaximum()
    {
        // No video reserved: even the controller floor is plenty.
        auto r = make();
        QVERIFY(!r.update(80'000 + kOverhead, 0, 0));
        QCOMPARE(r.bitrate(), 48'000);
    }

    void customMaximumIsAStep()
    {
        AudioRateController::Config c;
        c.maxBps = 36'000;
        c.overheadBps = kOverhead;
        AudioRateController r(c);
        QCOMPARE(r.bitrate(), 36'000);
        r.update(kVideoMin + kOverhead + 33'000, kVideoMin, 0);
        QCOMPARE(r.bitrate(), 32'000);
        r.update(5'000'000, kVideoMin, kSec);
        QVERIFY(r.update(5'000'000, kVideoMin, 3 * kSec));
        QCOMPARE(r.bitrate(), 36'000);
    }
};

QTEST_GUILESS_MAIN(TestAudioRate)
#include "test_audio_rate.moc"
