// Native VA-API HEVC encoder (hevc_vaapi_encoder.cpp) against FFmpeg's
// hevc_vaapi: same picture size (cropping included), comparable quality and
// size, keyframes on demand, colours signalled. Pattern frames only -- never
// opens the camera. Skips without a hardware HEVC encoder.
#include "video/video_decoder.h"
#include "video/video_encoder.h"

#include <QTest>

#include <algorithm>
#include <cmath>

using namespace p2p::video;

namespace {

// Luma PSNR of |dec| against |src| over |src|'s picture (FFmpeg's stream is
// padded to the coded size: compare the visible part).
double psnrY(const VideoFrame& dec, const VideoFrame& src)
{
    if (!dec.ensureCpu() || !src.ensureCpu() || dec.width < src.width || dec.height < src.height) return 0;
    double se = 0;
    for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < src.width; ++x) {
            const double d = double(dec.y()[size_t(y) * size_t(dec.width) + size_t(x)]) -
                             double(src.y()[size_t(y) * size_t(src.width) + size_t(x)]);
            se += d * d;
        }
    return se == 0 ? 99 : 10 * std::log10(255.0 * 255.0 * double(src.width) * double(src.height) / se);
}

// Moving texture with some noise: something for CBR to spend bits on.
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

struct Run {
    bool opened = false;
    QString name;
    int frames = 0, keyframes = 0, decodeErrors = 0;
    int outW = 0, outH = 0;
    Colour colour = Colour::Bt709Limited;
    double bytes = 0, psnr = 0; // per frame, after the first second
    std::vector<int> keyAt;
};

// Encodes |n| pattern frames (CPU NV12 in), decodes in software.
Run encodeRun(bool native, int w, int h, int kbps, int n, Colour colour = Colour::Bt709Limited,
              std::vector<int> forceKeyAt = {})
{
    Run r;
    VideoEncoder::setNativeHevc(native);
    VideoEncoder enc;
    VideoEncoder::Config c;
    c.width = w;
    c.height = h;
    c.fps = 30;
    c.bitrateKbps = kbps;
    c.colour = colour;
    QString why;
    r.opened = enc.init(c, &why);
    VideoEncoder::setNativeHevc(true); // the default
    if (!r.opened) return r;
    r.name = enc.name();
    VideoDecoder dec;
    if (!dec.init(p2p::media::Codec::HEVC, false, &why)) return r;
    int measured = 0;
    for (int i = 0; i < n; ++i) {
        VideoFrame f;
        f.allocate(w, h);
        f.colour = colour;
        pattern(f, i);
        std::vector<uint8_t> bits;
        bool key = false;
        const bool force = std::find(forceKeyAt.begin(), forceKeyAt.end(), i) != forceKeyAt.end();
        if (!enc.encode(f, i == 0 || force, [&](const uint8_t* d, size_t s, bool k) {
                bits.insert(bits.end(), d, d + s);
                key |= k;
            }))
            return r;
        if (key) {
            ++r.keyframes;
            r.keyAt.push_back(i);
        }
        bool err = false;
        auto out = dec.decode(bits.data(), bits.size(), &err);
        r.decodeErrors += err;
        if (!out) continue;
        ++r.frames;
        r.outW = out->width;
        r.outH = out->height;
        r.colour = out->colour;
        if (i >= 30) {
            r.bytes += double(bits.size());
            r.psnr += psnrY(*out, f);
            ++measured;
        }
    }
    if (measured) {
        r.bytes /= measured;
        r.psnr /= measured;
    }
    return r;
}

} // namespace

class TestHevcEncoder : public QObject {
    Q_OBJECT
private slots:
    // Ladder-like sizes, including ones that aren't a multiple of 16 (the
    // encoder codes 16-aligned and crops in the SPS).
    void matchesFfmpeg_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addColumn<int>("kbps");
        QTest::newRow("1280x720") << 1280 << 720 << 4000;
        QTest::newRow("1920x1080") << 1920 << 1080 << 8000;
        QTest::newRow("960x540") << 960 << 540 << 2000;
        QTest::newRow("426x240") << 426 << 240 << 500;
    }
    void matchesFfmpeg()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        QFETCH(int, kbps);
        const Run ff = encodeRun(false, w, h, kbps, 90);
        if (!ff.opened || !ff.name.contains(QLatin1String("vaapi"))) QSKIP("no hardware HEVC encoder");
        const Run nat = encodeRun(true, w, h, kbps, 90);
        QVERIFY(nat.opened);
        QCOMPARE(nat.name, QStringLiteral("hevc_vaapi_native"));
        qInfo("%dx%d @%d kbps: native %.0f B/frame %.2f dB, ffmpeg %.0f B/frame %.2f dB", w, h, kbps, nat.bytes,
              nat.psnr, ff.bytes, ff.psnr);
        QCOMPARE(nat.decodeErrors, 0);
        QCOMPARE(nat.frames, 90);
        QCOMPARE(nat.keyframes, 1);
        QCOMPARE(nat.outW, w); // cropped to the picture, no padding rows
        QCOMPARE(nat.outH, h);
        // Same encoder hardware and settings: same quality and size, give or take.
        QVERIFY2(nat.psnr > ff.psnr - 0.5, "native encoder quality below FFmpeg's");
        QVERIFY2(nat.bytes < ff.bytes * 1.1, "native encoder over budget");
    }

    void keyframeOnDemand()
    {
        const Run r = encodeRun(true, 640, 360, 1000, 40, Colour::Bt709Limited, {10, 25});
        if (!r.opened || r.name != QLatin1String("hevc_vaapi_native")) QSKIP("no native HEVC encoder");
        QCOMPARE(r.keyAt, (std::vector<int>{0, 10, 25}));
        QCOMPARE(r.decodeErrors, 0);
        QCOMPARE(r.frames, 40);
    }

    // Bitrate changes take effect within a few frames, with no keyframe.
    void liveBitrate()
    {
        VideoEncoder enc;
        VideoEncoder::Config c;
        c.width = 1280;
        c.height = 720;
        c.fps = 30;
        c.bitrateKbps = 4000;
        QString why;
        if (!enc.init(c, &why) || !enc.liveBitrate()) QSKIP("no native HEVC encoder");
        VideoDecoder dec;
        QVERIFY(dec.init(p2p::media::Codec::HEVC, false, &why));
        const int rates[] = {4000, 2000, 6000};
        int keyframes = 0;
        for (int phase = 0; phase < 3; ++phase) {
            enc.setBitrateKbps(rates[phase]);
            double bytes = 0;
            for (int i = 0; i < 30; ++i) {
                VideoFrame f;
                f.allocate(1280, 720);
                pattern(f, phase * 30 + i);
                std::vector<uint8_t> bits;
                QVERIFY(enc.encode(f, phase == 0 && i == 0, [&](const uint8_t* d, size_t n, bool k) {
                    bits.insert(bits.end(), d, d + n);
                    keyframes += k;
                }));
                bool err = false;
                QVERIFY(dec.decode(bits.data(), bits.size(), &err));
                QVERIFY(!err);
                if (i >= 5) bytes += double(bits.size());
            }
            const double kbps = bytes * 8 * 30 / 25 / 1000;
            qInfo("target %d kbps: %.0f kbps", rates[phase], kbps);
            QVERIFY2(std::abs(kbps - rates[phase]) < rates[phase] * 0.1, "rate didn't follow");
        }
        QCOMPARE(keyframes, 1);
    }

    // Frames 38, 39 lost: frame 40 predicts from 37 (or the newest kept
    // frame of its chain before it). A decoder that never saw 38, 39 must
    // then match one that saw everything, sample for sample.
    void recoversWithoutKeyframe_data()
    {
        QTest::addColumn<int>("interval");
        QTest::newRow("every frame kept") << 1;
        QTest::newRow("anchors 8 apart") << 8;
    }
    void recoversWithoutKeyframe()
    {
        QFETCH(int, interval);
        VideoEncoder enc;
        VideoEncoder::Config c;
        c.width = 640;
        c.height = 360;
        c.fps = 30;
        c.bitrateKbps = 1500;
        QString why;
        if (!enc.init(c, &why) || !enc.liveBitrate()) QSKIP("no native HEVC encoder");
        enc.setAnchorInterval(interval);
        VideoDecoder full, lossy;
        QVERIFY(full.init(p2p::media::Codec::HEVC, false, &why));
        QVERIFY(lossy.init(p2p::media::Codec::HEVC, false, &why));
        int keyframes = 0, refBackAt40 = -1;
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
        QCOMPARE(keyframes, 1);
        qInfo("anchors %d apart: recovery frame predicts from %d back", interval, refBackAt40);
        QVERIFY(refBackAt40 >= 3);
        if (interval == 1) QCOMPARE(refBackAt40, 3);
        // Long gone: no recovery possible.
        QVERIFY(!enc.recoverFrom(2));
    }

    void signalsColours()
    {
        for (Colour c : {Colour::Bt709Limited, Colour::Bt601Full}) {
            const Run r = encodeRun(true, 640, 360, 1000, 3, c);
            if (!r.opened || r.name != QLatin1String("hevc_vaapi_native")) QSKIP("no native HEVC encoder");
            QCOMPARE(r.frames, 3);
            QCOMPARE(int(r.colour), int(c));
        }
    }
};

QTEST_GUILESS_MAIN(TestHevcEncoder)
#include "test_hevc_encoder.moc"
