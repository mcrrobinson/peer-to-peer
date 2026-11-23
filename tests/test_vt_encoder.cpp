// Native VideoToolbox HEVC encoder (hevc_vt_encoder.cpp): picture sizes,
// keyframes on demand, live bitrate, LTR loss recovery, colours. Decoded
// with FFmpeg's software HEVC decoder. Pattern frames only -- never opens
// the camera. Skips without a hardware HEVC encoder.
#include "video/video_decoder.h"
#include "video/video_encoder.h"

#include <QTest>

#include <algorithm>
#include <cmath>

using namespace p2p::video;

namespace {

// Moving texture with some noise: something for the rate control to spend
// bits on (same as test_hevc_encoder).
void pattern(VideoFrame& f, int n)
{
    uint32_t seed = 12345u + uint32_t(n);
    auto rnd = [&] { return (seed = seed * 1103515245u + 12345u) >> 16; };
    const int w = f.width, h = f.height;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = ((x + 3 * n) * 255 / w + (y + 2 * n) * 128 / h) & 255;
            if ((((x + 5 * n) / 40) + ((y + n) / 40)) & 1) v = 255 - v;
            const int bx = w / 2 + int(w / 4 * std::sin(n * 0.05)), by = h / 2;
            if (std::abs(x - bx) < w / 16 && std::abs(y - by) < w / 16) v = (x * 7 + y * 13) & 255;
            f.y()[size_t(y) * size_t(w) + size_t(x)] = uint8_t(v);
        }
    for (size_t i = 0; i < size_t(w) * size_t(h) / 2; i += 2) {
        f.uv()[i] = uint8_t(128 + int((i / 64 + size_t(n)) % 64) - 32);
        f.uv()[i + 1] = uint8_t(128 + int((i / size_t(w)) % 64) - 32);
    }
    for (int k = 0; k < w * h / 200; ++k) f.y()[(rnd() * 65536u + rnd()) % (size_t(w) * size_t(h))] ^= 0x20;
}

bool openNative(VideoEncoder& enc, int w, int h, int kbps, Colour colour = Colour::Bt709Limited)
{
    VideoEncoder::Config c;
    c.width = w;
    c.height = h;
    c.fps = 30;
    c.bitrateKbps = kbps;
    c.colour = colour;
    QString why;
    return enc.init(c, &why) && enc.name() == QLatin1String("hevc_videotoolbox_native");
}

} // namespace

class TestVtEncoder : public QObject {
    Q_OBJECT
private slots:
    // Ladder sizes, including ones that aren't a multiple of 16: decoded at
    // the picture size, one keyframe, no errors.
    void encodesLadderSizes_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addColumn<int>("kbps");
        QTest::newRow("1920x1080") << 1920 << 1080 << 8000;
        QTest::newRow("1280x720") << 1280 << 720 << 4000;
        QTest::newRow("960x540") << 960 << 540 << 2000;
        QTest::newRow("426x240") << 426 << 240 << 500;
    }
    void encodesLadderSizes()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        QFETCH(int, kbps);
        VideoEncoder enc;
        if (!openNative(enc, w, h, kbps)) QSKIP("no native VideoToolbox HEVC encoder");
        VideoDecoder dec;
        QString why;
        QVERIFY(dec.init(p2p::media::Codec::HEVC, false, &why));
        int frames = 0, keyframes = 0, errors = 0, outW = 0, outH = 0;
        for (int i = 0; i < 60; ++i) {
            VideoFrame f;
            f.allocate(w & ~1, h & ~1);
            pattern(f, i);
            std::vector<uint8_t> bits;
            bool key = false;
            QVERIFY(enc.encode(f, i == 0, [&](const uint8_t* d, size_t n, bool k) {
                bits.insert(bits.end(), d, d + n);
                key |= k;
            }));
            if (bits.empty()) continue; // dropped by the rate control
            keyframes += key;
            bool err = false;
            auto out = dec.decode(bits.data(), bits.size(), &err);
            errors += err;
            if (!out) continue;
            ++frames;
            outW = out->width;
            outH = out->height;
        }
        qInfo("%dx%d @%d kbps: %d frames decoded", w, h, kbps, frames);
        QCOMPARE(errors, 0);
        QVERIFY(frames >= 55);
        QCOMPARE(keyframes, 1);
        QCOMPARE(outW, w & ~1);
        QCOMPARE(outH, h & ~1);
    }

    void keyframeOnDemand()
    {
        VideoEncoder enc;
        if (!openNative(enc, 640, 360, 1000)) QSKIP("no native VideoToolbox HEVC encoder");
        std::vector<int> keyAt;
        for (int i = 0; i < 40; ++i) {
            VideoFrame f;
            f.allocate(640, 360);
            pattern(f, i);
            QVERIFY(enc.encode(f, i == 0 || i == 10 || i == 25, [&](const uint8_t*, size_t, bool k) {
                if (k) keyAt.push_back(i);
                if (k) QCOMPARE(enc.refBack(), 0);
            }));
        }
        QCOMPARE(keyAt, (std::vector<int>{0, 10, 25}));
    }

    // A new target is followed without a keyframe. VideoToolbox's
    // low-latency rate control settles within about a second, so each
    // phase is 2 s and only its second half is measured.
    void liveBitrate()
    {
        VideoEncoder enc;
        if (!openNative(enc, 1280, 720, 4000)) QSKIP("no native VideoToolbox HEVC encoder");
        QVERIFY(enc.liveBitrate());
        const int rates[] = {4000, 1500, 6000};
        int keyframes = 0, n = 0;
        for (int phase = 0; phase < 3; ++phase) {
            enc.setBitrateKbps(rates[phase]);
            double bytes = 0;
            for (int i = 0; i < 60; ++i, ++n) {
                VideoFrame f;
                f.allocate(1280, 720);
                pattern(f, n);
                QVERIFY(enc.encode(f, n == 0, [&](const uint8_t*, size_t s, bool k) {
                    if (i >= 30) bytes += double(s);
                    keyframes += k;
                }));
            }
            const double kbps = bytes * 8 / 1000; // 30 frames = 1 s
            qInfo("target %d kbps: %.0f kbps", rates[phase], kbps);
            QVERIFY2(std::abs(kbps - rates[phase]) < rates[phase] * 0.2, "rate didn't follow");
        }
        QCOMPARE(keyframes, 1);
    }

    // Frames 38, 39 lost: frame 40 refreshes from an LTR the receiver has
    // (frame 0, the keyframe: VideoToolbox marks LTRs ~2 s apart). A decoder
    // that never saw 38, 39 must then match one that saw everything.
    void recoversWithoutKeyframe()
    {
        VideoEncoder enc;
        if (!openNative(enc, 640, 360, 1500)) QSKIP("no native VideoToolbox HEVC encoder");
        VideoDecoder full, lossy;
        QString why;
        QVERIFY(full.init(p2p::media::Codec::HEVC, false, &why));
        QVERIFY(lossy.init(p2p::media::Codec::HEVC, false, &why));
        int keyframes = 0, refBackAt40 = -1;
        size_t bytesAt40 = 0, bytesAt39 = 0;
        for (uint32_t id = 0; id < 60; ++id) {
            VideoFrame f;
            f.allocate(640, 360);
            pattern(f, int(id));
            if (id == 40) QVERIFY(enc.recoverFrom(37));
            std::vector<uint8_t> bits;
            QVERIFY(enc.encode(
                f, id == 0,
                [&](const uint8_t* d, size_t n, bool k) {
                    bits.insert(bits.end(), d, d + n);
                    keyframes += k;
                    if (id == 40) refBackAt40 = enc.refBack();
                    if (id != 40 && !k) QCOMPARE(enc.refBack(), 1);
                },
                id));
            QVERIFY2(!bits.empty(), "frame dropped by the rate control");
            if (id == 39) bytesAt39 = bits.size();
            if (id == 40) bytesAt40 = bits.size();
            bool err = false;
            auto a = full.decode(bits.data(), bits.size(), &err);
            QVERIFY(a && !err);
            if (id == 38 || id == 39) continue;
            auto b = lossy.decode(bits.data(), bits.size(), &err);
            QVERIFY(b && !err);
            if (id >= 40) {
                QVERIFY(a->ensureCpu() && b->ensureCpu());
                QVERIFY2(a->data == b->data, qPrintable(QStringLiteral("frame %1 differs after recovery").arg(id)));
            }
        }
        qInfo("recovery frame predicts from %d back, %zu bytes (frame before: %zu)", refBackAt40, bytesAt40, bytesAt39);
        QCOMPARE(keyframes, 1);
        QCOMPARE(refBackAt40, 40);
        // After a keyframe nothing before it can be referenced.
        VideoFrame f;
        f.allocate(640, 360);
        pattern(f, 60);
        QVERIFY(enc.encode(f, true, [](const uint8_t*, size_t, bool) {}, 60));
        QVERIFY(!enc.recoverFrom(59));
    }

    // Losses one after another, each starting right after the previous
    // recovery frame (lost 1 frame, recovery 3 frames later, every 4
    // frames). Acknowledging only VideoToolbox's newest LTR fails here: it
    // marks most frames as LTRs and every loss became a keyframe.
    void recoversFromBackToBackLosses()
    {
        VideoEncoder enc;
        if (!openNative(enc, 640, 360, 1500)) QSKIP("no native VideoToolbox HEVC encoder");
        VideoDecoder full, lossy;
        QString why;
        QVERIFY(full.init(p2p::media::Codec::HEVC, false, &why));
        QVERIFY(lossy.init(p2p::media::Codec::HEVC, false, &why));
        int keyframes = 0, recoveries = 0, compared = 0;
        for (uint32_t id = 0; id < 150; ++id) {
            VideoFrame f;
            f.allocate(640, 360);
            pattern(f, int(id));
            // Loss at 30, 34, 38, ...: frames 30..32 never decode (30 lost,
            // 31, 32 predict from it); 33 recovers from 29 or earlier.
            const bool undecodable = id >= 30 && (id - 30) % 4 < 3;
            if (id > 30 && (id - 30) % 4 == 3) {
                QVERIFY2(enc.recoverFrom(id - 4), qPrintable(QStringLiteral("no LTR for frame %1").arg(id)));
                ++recoveries;
            }
            std::vector<uint8_t> bits;
            QVERIFY(enc.encode(
                f, id == 0,
                [&](const uint8_t* d, size_t n, bool k) {
                    bits.insert(bits.end(), d, d + n);
                    keyframes += k;
                },
                id));
            QVERIFY2(!bits.empty(), "frame dropped by the rate control");
            bool err = false;
            auto a = full.decode(bits.data(), bits.size(), &err);
            QVERIFY(a && !err);
            if (undecodable) continue;
            auto b = lossy.decode(bits.data(), bits.size(), &err);
            QVERIFY(b && !err);
            QVERIFY(a->ensureCpu() && b->ensureCpu());
            QVERIFY2(a->data == b->data, qPrintable(QStringLiteral("frame %1 differs").arg(id)));
            ++compared;
        }
        qInfo("%d recoveries, %d keyframes, %d frames compared", recoveries, keyframes, compared);
        QCOMPARE(keyframes, 1);
    }

    void signalsColours()
    {
        for (Colour c : {Colour::Bt709Limited, Colour::Bt601Full}) {
            VideoEncoder enc;
            if (!openNative(enc, 640, 360, 1000, c)) QSKIP("no native VideoToolbox HEVC encoder");
            VideoDecoder dec;
            QString why;
            QVERIFY(dec.init(p2p::media::Codec::HEVC, false, &why));
            VideoFrame f;
            f.allocate(640, 360);
            f.colour = c;
            pattern(f, 0);
            std::vector<uint8_t> bits;
            QVERIFY(enc.encode(f, true, [&](const uint8_t* d, size_t n, bool) { bits.insert(bits.end(), d, d + n); }));
            bool err = false;
            auto out = dec.decode(bits.data(), bits.size(), &err);
            QVERIFY(out && !err);
            QCOMPARE(int(out->colour), int(c));
        }
    }
};

QTEST_GUILESS_MAIN(TestVtEncoder)
#include "test_vt_encoder.moc"
