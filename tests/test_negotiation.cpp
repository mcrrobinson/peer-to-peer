#include "media/codec_caps.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace p2p::media;

namespace {
CodecCap cap(Codec c, bool hw) { return {c, hw, 3840, 2160}; }

// What the dev box (RX 5700, Navi 10) reports: HEVC hw both ways, AV1 sw only.
MediaCaps navi10()
{
    return {{cap(Codec::HEVC, true), cap(Codec::AV1, false), cap(Codec::HEVC, false)},
            {cap(Codec::HEVC, true), cap(Codec::AV1, false), cap(Codec::HEVC, false)}};
}
// A newer box with AV1 hw encode + decode.
MediaCaps av1Hw()
{
    return {{cap(Codec::AV1, true), cap(Codec::HEVC, true)}, {cap(Codec::AV1, true), cap(Codec::HEVC, true)}};
}
} // namespace

class TestNegotiation : public QObject {
    Q_OBJECT
private slots:
    void hevcHwIsDefaultOnNavi10()
    {
        auto s = negotiate(navi10().encoders, navi10().decoders, {});
        QVERIFY(s);
        QCOMPARE(s->codec, Codec::HEVC);
        QVERIFY(s->encoderHw && s->decoderHw);
    }

    void preferAv1PicksSoftwareAv1()
    {
        auto s = negotiate(navi10().encoders, navi10().decoders, {.preferAv1 = true});
        QVERIFY(s);
        QCOMPARE(s->codec, Codec::AV1);
        QVERIFY(!s->encoderHw);
    }

    void av1HwBeatsHevcHw()
    {
        auto s = negotiate(av1Hw().encoders, av1Hw().decoders, {});
        QCOMPARE(s->codec, Codec::AV1);
        QVERIFY(s->encoderHw && s->decoderHw);
    }

    void av1HwEncodeButSoftwareDecodeLosesToHevcHw()
    {
        // av1Hw -> navi10: AV1 hw enc + dav1d sw dec (110) < HEVC hw/hw (140)
        auto s = negotiate(av1Hw().encoders, navi10().decoders, {});
        QCOMPARE(s->codec, Codec::HEVC);
    }

    void noCommonCodec()
    {
        QVERIFY(!negotiate({cap(Codec::AV1, false)}, {cap(Codec::HEVC, true)}, {}));
        QVERIFY(!negotiate({}, navi10().decoders, {}));
    }

    void maxResolutionIsIntersection()
    {
        auto s = negotiate({{Codec::HEVC, true, 4096, 2304}}, {{Codec::HEVC, true, 1920, 1080}}, {});
        QCOMPARE(s->maxWidth, 1920);
        QCOMPARE(s->maxHeight, 1080);
    }

    void jsonRoundTripIgnoresUnknownCodecs()
    {
        QJsonObject o = navi10().toJson();
        auto encArr = o.value("enc").toArray();
        encArr.append(QJsonObject{{"codec", "vvc"}, {"hw", true}});
        o.insert("enc", encArr);
        const MediaCaps back = MediaCaps::fromJson(o);
        QCOMPARE(back.encoders, navi10().encoders);
        QCOMPARE(back.decoders, navi10().decoders);
    }

    void dredRevisionRoundTripsAndGates()
    {
        MediaCaps a = navi10(), b = navi10();
        a.audioDredRev = 12;
        QCOMPARE(MediaCaps::fromJson(a.toJson()).audioDredRev, 12);
        // A peer from before the field existed: no DRED.
        QJsonObject old = navi10().toJson();
        old.remove("dredRev");
        QCOMPARE(MediaCaps::fromJson(old).audioDredRev, 0);

        b.audioDredRev = 12;
        QVERIFY(dredCompatible(a, b));
        b.audioDredRev = 10; // libopus 1.5 vs 1.6
        QVERIFY(!dredCompatible(a, b));
        a.audioDredRev = b.audioDredRev = 0; // neither built with DRED
        QVERIFY(!dredCompatible(a, b));
    }

    void selectionJsonRoundTrip()
    {
        auto s = *negotiate(navi10().encoders, navi10().decoders, {});
        QCOMPARE(*StreamSelection::fromJson(s.toJson()), s);
        QVERIFY(!StreamSelection::fromJson(QJsonObject{{"codec", "vp9"}}));
    }
};

QTEST_GUILESS_MAIN(TestNegotiation)
#include "test_negotiation.moc"
