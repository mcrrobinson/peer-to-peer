// End-to-end signalling test: two CallControllers in one process talking
// over a real localhost TCP connection.
#include "app/call_controller.h"
#include "net/crypto.h"

#include <QSignalSpy>
#include <QTest>

using p2p::app::CallController;
using State = CallController::State;
using namespace p2p::media;

namespace {
MediaCaps navi10Caps()
{
    CodecCap hevcHw{Codec::HEVC, true, 4096, 2304}, av1Sw{Codec::AV1, false, 3840, 2160},
        hevcSw{Codec::HEVC, false, 3840, 2160};
    return {{hevcHw, av1Sw, hevcSw}, {hevcHw, av1Sw, hevcSw}};
}

std::unique_ptr<CallController> makePeer(const QString& name, bool autoAccept, MediaCaps caps = navi10Caps(),
                                         double simLoss = 0.0)
{
    CallController::Options o;
    o.name = name;
    o.port = 0;
    o.autoAccept = autoAccept;
    o.discovery = false;
    o.caps = std::move(caps);
    // Media plane with a simulated clock and a test tone: no devices needed.
    o.mediaConfig.audio.useDevice = false;
    o.mediaConfig.audio.source = p2p::audio::AudioPipeline::Source::Tone;
    o.mediaConfig.simulatedLoss = simLoss;
    // Synthetic video too: tests must never grab the real camera.
    o.mediaConfig.video.source = p2p::video::VideoStreamConfig::Source::Pattern;
    o.mediaConfig.video.capture.width = 1280;
    o.mediaConfig.video.capture.height = 720;
    o.mediaConfig.video.capture.fps = 30;
    o.mediaConfig.video.bitrateKbps = 4000;
    auto c = std::make_unique<CallController>(o);
    if (!c->start()) return nullptr;
    return c;
}

QString addrOf(const CallController& c) { return QStringLiteral("127.0.0.1:%1").arg(c.listenPort()); }
} // namespace

class TestCall : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(p2p::crypto::init()); }

    void callConnectsAndNegotiates()
    {
        auto a = makePeer("alice", false), b = makePeer("bob", true);
        QVERIFY(a && b);
        a->setPreferAv1Screen(true);
        a->callAddress(addrOf(*b));

        QTRY_COMPARE(a->state(), State::InCall);
        QTRY_COMPARE(b->state(), State::InCall);
        QCOMPARE(a->remoteName(), QStringLiteral("bob"));
        QCOMPARE(b->remoteName(), QStringLiteral("alice"));
        QVERIFY(!a->sas().isEmpty());
        QCOMPARE(a->sas(), b->sas());

        // What A sends is what B receives, and vice versa.
        QCOMPARE(a->sendSelections()->toJson(), b->recvSelections()->toJson());
        QCOMPARE(b->sendSelections()->toJson(), a->recvSelections()->toJson());
        QCOMPARE(a->sendSelections()->camera.codec, Codec::HEVC);
        QVERIFY(a->sendSelections()->camera.encoderHw);
        QCOMPARE(a->sendSelections()->screen->codec, Codec::AV1);   // A prefers AV1 for screen
        QCOMPARE(b->sendSelections()->screen->codec, Codec::HEVC);  // B uses the default

        // RTT / clock offset arrive from the ping loop; same host => offset ~0.
        QTRY_VERIFY(a->rttMs() >= 0);
        QVERIFY(a->rttMs() < 50);
        QVERIFY(std::abs(a->clockOffsetMs()) < 5);

        QSignalSpy bEnded(b.get(), &CallController::callEnded);
        a->hangup();
        QTRY_COMPARE(b->state(), State::Idle);
        QCOMPARE(a->state(), State::Idle);
        QCOMPARE(bEnded.size(), 1);
        QCOMPARE(bEnded.first().first().toString(), QStringLiteral("remote hangup"));

        // A second call on the same controllers works.
        b->callAddress(addrOf(*a));
        QTRY_COMPARE(a->state(), State::Incoming);
        a->accept();
        QTRY_COMPARE(b->state(), State::InCall);
        b->hangup();
        QTRY_COMPARE(a->state(), State::Idle);
    }

    void audioFlowsEndToEnd()
    {
        auto a = makePeer("alice", false), b = makePeer("bob", true);
        a->callAddress(addrOf(*b));
        QTRY_COMPARE(a->state(), State::InCall);
        QTRY_COMPARE(b->state(), State::InCall);
        QVERIFY(a->mediaSession() && b->mediaSession());

        // ~1.5 s of 10 ms frames each way.
        QTRY_VERIFY_WITH_TIMEOUT(b->mediaSession()->audioStats().jb.decoded > 120, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(a->mediaSession()->audioStats().jb.decoded > 120, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(b->mediaSession()->audioStats().outputRmsDb > -30, 3000);

        const auto s = b->mediaSession()->audioStats();
        QVERIFY2(s.toneFraction > 0.9, qPrintable(QString::number(s.toneFraction)));
        QVERIFY2(s.latencyAvgMs > 0 && s.latencyAvgMs < 60, qPrintable(QString::number(s.latencyAvgMs)));
        QCOMPARE(b->mediaSession()->transportStats().authFailures, 0u);
        QTRY_VERIFY(!b->mediaStats().isEmpty());

        a->hangup();
        QTRY_COMPARE(b->state(), State::Idle);
        QVERIFY(!b->mediaSession());
    }

    void audioSurvivesLoss()
    {
        // A drops 10% of its outgoing packets; B must conceal every gap
        // (DRED / LBRR / PLC) and keep playing the tone.
        auto a = makePeer("alice", false, navi10Caps(), 0.10), b = makePeer("bob", true);
        a->callAddress(addrOf(*b));
        QTRY_COMPARE(b->state(), State::InCall);
        QTRY_VERIFY_WITH_TIMEOUT(b->mediaSession()->audioStats().jb.decoded > 250, 8000);

        const auto s = b->mediaSession()->audioStats();
        const auto sent = a->mediaSession()->transportStats();
        QVERIFY(sent.simDropped > 10);
        QVERIFY2(s.jb.concealed > 10, qPrintable(QString::number(s.jb.concealed)));
        QCOMPARE(s.concealDred + s.concealFec + s.concealPlc, s.jb.concealed);
        QVERIFY2(s.toneFraction > 0.7, qPrintable(QString::number(s.toneFraction)));
        qInfo("loss test: dropped %llu, concealed %llu (dred %llu fec %llu plc %llu), tone %.2f",
              (unsigned long long)sent.simDropped, (unsigned long long)s.jb.concealed,
              (unsigned long long)s.concealDred, (unsigned long long)s.concealFec,
              (unsigned long long)s.concealPlc, s.toneFraction);
        a->hangup();
        QTRY_COMPARE(b->state(), State::Idle);
    }

    void videoFlowsAndDecodesCorrectly()
    {
        auto a = makePeer("alice", false), b = makePeer("bob", true);
        a->callAddress(addrOf(*b));
        QTRY_COMPARE(b->state(), State::InCall);
        QTRY_VERIFY(a->mediaSession() && b->mediaSession());

        // ~2 s of 30 fps test pattern each way.
        QTRY_VERIFY_WITH_TIMEOUT(b->mediaSession()->videoRecvStats()->decoded >= 50, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(a->mediaSession()->videoRecvStats()->decoded >= 50, 8000);
        const auto rx = *b->mediaSession()->videoRecvStats();
        const auto tx = *a->mediaSession()->videoSendStats();
        qInfo("video: %s -> %s %dx%d, enc avg %.2f ms, dec avg %.2f ms, capture->decoded avg %.1f ms p95 %.1f ms",
              qPrintable(tx.encoder), qPrintable(rx.decoder), rx.width, rx.height, tx.encodeAvgMs, rx.decodeAvgMs,
              rx.latencyAvgMs, rx.latencyP95Ms);
        QCOMPARE(rx.width, 1280);
        QCOMPARE(rx.height, 720);
        // Decoded pixels carry the sender's frame numbers: readable and increasing.
        QVERIFY2(rx.patternOk >= 45, qPrintable(QString::number(rx.patternOk)));
        QCOMPARE(rx.patternBad, 0u);
        QCOMPARE(rx.decodeErrors, 0u);
        QCOMPARE(rx.assembler.framesLost, 0u);
        QVERIFY(rx.latencyAvgMs > 0 && rx.latencyAvgMs < 100);
        QVERIFY(tx.keyframes >= 1);

        a->hangup();
        QTRY_COMPARE(b->state(), State::Idle);
    }

    void videoRecoversFromLossWithKeyframes()
    {
        // 5% packet loss from A: frames are lost, B asks for keyframes and
        // keeps decoding (no NACK/FEC until phase 5).
        auto a = makePeer("alice", false, navi10Caps(), 0.05), b = makePeer("bob", true);
        a->callAddress(addrOf(*b));
        QTRY_COMPARE(b->state(), State::InCall);
        QTRY_VERIFY(b->mediaSession());
        QTRY_VERIFY_WITH_TIMEOUT(b->mediaSession()->videoRecvStats()->decoded >= 30, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(b->mediaSession()->videoRecvStats()->pliSent >= 1, 5000);
        const auto rx = *b->mediaSession()->videoRecvStats();
        const auto tx = *a->mediaSession()->videoSendStats();
        qInfo("loss: lost %llu, pli sent %llu / received %llu, keyframes %llu, decoded %llu, pattern bad %llu",
              (unsigned long long)rx.assembler.framesLost, (unsigned long long)rx.pliSent,
              (unsigned long long)tx.pliReceived, (unsigned long long)tx.keyframes,
              (unsigned long long)rx.decoded, (unsigned long long)rx.patternBad);
        QVERIFY(rx.assembler.framesLost >= 1);
        QVERIFY(tx.keyframes >= 2); // initial + at least one on request
        QCOMPARE(rx.patternBad, 0u); // never displays a corrupt / out-of-order frame
        a->hangup();
        QTRY_COMPARE(b->state(), State::Idle);
    }

    void audioProcessingTogglesApplyLive()
    {
        auto a = makePeer("alice", false), b = makePeer("bob", true);
        QSignalSpy changed(a.get(), &CallController::audioProcessingChanged);
        const bool ns0 = a->noiseSuppression();
        a->setNoiseSuppression(!ns0);
        a->setNoiseSuppression(!ns0); // no-op: unchanged
        QCOMPARE(changed.size(), 1);
        QCOMPARE(a->noiseSuppression(), !ns0);

        a->callAddress(addrOf(*b));
        QTRY_COMPARE(a->state(), State::InCall);
        // Toggling mid-call must not disturb the call.
        a->setEchoCancel(!a->echoCancel());
        a->setAutoGain(!a->autoGain());
        a->setNoiseLevel(2);
        a->setNoiseLevel(9);           // clamped to 2: no change
        QCOMPARE(a->noiseLevel(), 2);
        QCOMPARE(changed.size(), 4);
        QTRY_VERIFY_WITH_TIMEOUT(b->mediaSession()->audioStats().jb.decoded > 50, 3000);
        QCOMPARE(a->state(), State::InCall);
        a->hangup();
        QTRY_COMPARE(b->state(), State::Idle);
    }

    void rejectedCall()
    {
        auto a = makePeer("alice", false), b = makePeer("bob", false);
        a->callAddress(addrOf(*b));
        QTRY_COMPARE(b->state(), State::Incoming);
        QCOMPARE(a->state(), State::Outgoing);
        b->reject();
        QTRY_COMPARE(a->state(), State::Idle);
        QVERIFY(a->lastError().contains("rejected"));
        QCOMPARE(b->state(), State::Idle);
    }

    void busyPeerRefusesSecondCaller()
    {
        auto a = makePeer("alice", false), b = makePeer("bob", true), c = makePeer("carol", false);
        a->callAddress(addrOf(*b));
        QTRY_COMPARE(a->state(), State::InCall);
        c->callAddress(addrOf(*b));
        QTRY_COMPARE(c->state(), State::Idle);
        QCOMPARE(b->state(), State::InCall);
    }

    void noCommonCodecFails()
    {
        auto a = makePeer("alice", false, {{{Codec::AV1, false, 1920, 1080}}, {{Codec::AV1, false, 1920, 1080}}});
        auto b = makePeer("bob", true, {{{Codec::HEVC, true, 1920, 1080}}, {{Codec::HEVC, true, 1920, 1080}}});
        a->callAddress(addrOf(*b));
        QTRY_COMPARE(a->state(), State::Idle);
        QVERIFY(a->lastError().contains("no codec"));
        QTRY_COMPARE(b->state(), State::Idle);
    }

    void connectFailure()
    {
        auto a = makePeer("alice", false);
        a->callAddress("127.0.0.1:1"); // nothing listens on port 1
        QTRY_COMPARE(a->state(), State::Idle);
        QVERIFY(a->lastError().contains("connect"));
    }

    void badAddress()
    {
        auto a = makePeer("alice", false);
        a->callAddress("nonsense");
        QCOMPARE(a->state(), State::Idle);
        QVERIFY(!a->lastError().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCall)
#include "test_call.moc"
