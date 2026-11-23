// Native VideoToolbox HEVC encoder (hevc_vt_encoder.cpp): picture sizes,
// keyframes on demand, live bitrate, LTR loss recovery, colours. Decoded
// with FFmpeg's software HEVC decoder. Pattern frames only -- never opens
// the camera. Skips without a hardware HEVC encoder.
#include "video/cv_image.h"
#include "video/video_decoder.h"
#include "video/video_encoder.h"

#include <CoreVideo/CoreVideo.h>

#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>

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

// Smooth moving picture (no edges, no noise): any decent scaler agrees with
// a box average on it, so a scaled GPU frame can be checked against one.
void smooth(VideoFrame& f, int n)
{
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x)
            f.y()[size_t(y) * size_t(f.width) + size_t(x)] =
                uint8_t(128 + 70 * std::sin(x * 0.031 + n * 0.2) * std::cos(y * 0.023 - n * 0.1));
    for (int y = 0; y < f.height / 2; ++y)
        for (int x = 0; x < f.width / 2; ++x) {
            f.uv()[size_t(y) * size_t(f.width) + size_t(2 * x)] = uint8_t(128 + 40 * std::sin(x * 0.04 + n * 0.1));
            f.uv()[size_t(y) * size_t(f.width) + size_t(2 * x + 1)] = uint8_t(128 + 40 * std::cos(y * 0.05));
        }
}

// |f| copied into a new IOSurface-backed 420v CVPixelBuffer, as a GPU frame
// (what the camera delivers).
std::shared_ptr<VideoFrame> onGpu(const VideoFrame& f)
{
    CFMutableDictionaryRef attrs =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef none = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, none);
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferCreate(nullptr, size_t(f.width), size_t(f.height), kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                        attrs, &pb);
    CFRelease(none);
    CFRelease(attrs);
    if (!pb) return nullptr;
    CVPixelBufferLockBaseAddress(pb, 0);
    for (size_t p = 0; p < 2; ++p) {
        auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, p));
        const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        for (int r = 0; r < (p ? f.height / 2 : f.height); ++r)
            std::memcpy(dst + size_t(r) * stride, (p ? f.uv() : f.y()) + size_t(r) * size_t(f.width), size_t(f.width));
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    auto g = std::make_shared<VideoFrame>();
    g->width = f.width;
    g->height = f.height;
    g->gpu = wrapCvPixelBuffer(pb);
    CVPixelBufferRelease(pb);
    return g;
}

// Luma PSNR of |a| against |ref| scaled down to |a|'s size by averaging.
double lumaPsnr(const VideoFrame& a, const VideoFrame& ref)
{
    const int sx = ref.width / a.width, sy = ref.height / a.height;
    double se = 0;
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x) {
            int sum = 0;
            for (int j = 0; j < sy; ++j)
                for (int i = 0; i < sx; ++i) sum += ref.y()[size_t(y * sy + j) * size_t(ref.width) + size_t(x * sx + i)];
            const double d = a.y()[size_t(y) * size_t(a.width) + size_t(x)] - double(sum) / (sx * sy);
            se += d * d;
        }
    const double mse = se / (double(a.width) * a.height);
    return mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
}

// |ref| scaled down by averaging (both planes), as a CPU frame of |w| x |h|.
VideoFrame boxScaled(const VideoFrame& ref, int w, int h)
{
    VideoFrame o;
    o.allocate(w, h);
    const int sx = ref.width / w, sy = ref.height / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int j = 0; j < sy; ++j)
                for (int i = 0; i < sx; ++i) sum += ref.y()[size_t(y * sy + j) * size_t(ref.width) + size_t(x * sx + i)];
            o.y()[size_t(y) * size_t(w) + size_t(x)] = uint8_t(sum / (sx * sy));
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x)
            for (int c = 0; c < 2; ++c) {
                int sum = 0;
                for (int j = 0; j < sy; ++j)
                    for (int i = 0; i < sx; ++i)
                        sum += ref.uv()[size_t(y * sy + j) * size_t(ref.width) + size_t(2 * (x * sx + i) + c)];
                o.uv()[size_t(y) * size_t(w) + size_t(2 * x + c)] = uint8_t(sum / (sx * sy));
            }
    return o;
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

    // VideoToolbox decode (FFmpeg hwaccel): frames stay on the GPU as
    // CVPixelBuffers and read back identical to the software decoder's
    // (HEVC decoding is exact), at the picture size, in the stream's colours.
    void hardwareDecoderMatchesSoftware_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addColumn<int>("colour");
        QTest::newRow("1280x720") << 1280 << 720 << int(Colour::Bt709Limited);
        QTest::newRow("426x240") << 426 << 240 << int(Colour::Bt709Limited);
        QTest::newRow("640x360 BT.601 full") << 640 << 360 << int(Colour::Bt601Full);
    }
    void hardwareDecoderMatchesSoftware()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        QFETCH(int, colour);
        VideoEncoder enc;
        if (!openNative(enc, w, h, 2000, Colour(colour))) QSKIP("no native VideoToolbox HEVC encoder");
        VideoDecoder hw, sw;
        QString why;
        QVERIFY(hw.init(p2p::media::Codec::HEVC, true, &why));
        if (!hw.hardware()) QSKIP("no VideoToolbox HEVC decoder");
        QVERIFY(sw.init(p2p::media::Codec::HEVC, false, &why));
        int frames = 0;
        for (int i = 0; i < 30; ++i) {
            VideoFrame f;
            f.allocate(w & ~1, h & ~1);
            f.colour = Colour(colour);
            pattern(f, i);
            std::vector<uint8_t> bits;
            QVERIFY(enc.encode(f, i == 0, [&](const uint8_t* d, size_t n, bool) { bits.insert(bits.end(), d, d + n); }));
            if (bits.empty()) continue;
            bool err = false;
            auto a = hw.decode(bits.data(), bits.size(), &err);
            QVERIFY(a && !err);
            auto b = sw.decode(bits.data(), bits.size(), &err);
            QVERIFY(b && !err);
            QVERIFY(a->gpu && a->gpu->cvPixelBuffer());
            QCOMPARE(a->width, w & ~1);
            QCOMPARE(a->height, h & ~1);
            QCOMPARE(int(a->colour), colour);
            QVERIFY(a->ensureCpu() && b->ensureCpu());
            QVERIFY2(a->data == b->data, qPrintable(QStringLiteral("frame %1 differs").arg(i)));
            ++frames;
        }
        QVERIFY(frames >= 28);
    }

    // GPU frames (CVPixelBuffers, as from the camera) go in without a CPU
    // copy: as is at the encoder's size, scaled on the GPU otherwise.
    void takesGpuFrames_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::newRow("same size 1280x720") << 1280 << 720;
        QTest::newRow("scaled to 640x360") << 640 << 360;
        QTest::newRow("scaled to 320x180") << 320 << 180;
    }
    void takesGpuFrames()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        // Reference: the same pictures scaled on the CPU (box average) and
        // sent as CPU frames. Smooth pictures: the GPU scaler's filter is
        // softer than a box, which would only show on edges and noise.
        VideoEncoder enc, cpuEnc;
        if (!openNative(enc, w, h, 4000) || !openNative(cpuEnc, w, h, 4000)) QSKIP("no native VideoToolbox HEVC encoder");
        QVERIFY(enc.takesGpuFrames());
        VideoDecoder dec, cpuDec;
        QString why;
        QVERIFY(dec.init(p2p::media::Codec::HEVC, false, &why));
        QVERIFY(cpuDec.init(p2p::media::Codec::HEVC, false, &why));
        double worst = 99, cpuWorst = 99;
        int frames = 0;
        for (int i = 0; i < 20; ++i) {
            VideoFrame src;
            src.allocate(1280, 720);
            smooth(src, i);
            auto g = onGpu(src);
            QVERIFY(g);
            std::vector<uint8_t> bits;
            QVERIFY(enc.encode(*g, i == 0, [&](const uint8_t* d, size_t n, bool) { bits.insert(bits.end(), d, d + n); }));
            QVERIFY2(g->data.empty(), "the encoder read the GPU frame back");
            if (bits.empty()) continue;
            bool err = false;
            auto out = dec.decode(bits.data(), bits.size(), &err);
            QVERIFY(out && !err);
            QCOMPARE(out->width, w);
            QCOMPARE(out->height, h);
            QVERIFY(out->ensureCpu());
            if (i >= 5) worst = std::min(worst, lumaPsnr(*out, src));
            ++frames;

            const VideoFrame small = boxScaled(src, w, h);
            bits.clear();
            QVERIFY(cpuEnc.encode(small, i == 0, [&](const uint8_t* d, size_t n, bool) { bits.insert(bits.end(), d, d + n); }));
            if (bits.empty()) continue;
            auto ref = cpuDec.decode(bits.data(), bits.size(), &err);
            QVERIFY(ref && !err && ref->ensureCpu());
            if (i >= 5) cpuWorst = std::min(cpuWorst, lumaPsnr(*ref, src));
        }
        qInfo("%dx%d from 1280x720: worst luma PSNR over frames 5-19 vs the source, GPU frames %.1f dB, CPU-scaled "
              "frames %.1f dB",
              w, h, worst, cpuWorst);
        QVERIFY(frames >= 18);
        // Wrong geometry or colours would be far below 30 dB; at 1/4 size
        // the softer GPU filter costs ~6 dB against the box (40 vs 46 dB).
        QVERIFY2(worst > 35, "GPU frames don't match the (scaled) source");
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
