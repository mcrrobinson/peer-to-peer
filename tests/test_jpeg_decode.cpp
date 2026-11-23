// Hardware (VA-API) vs. software camera MJPEG decode: same picture, same
// colours. Synthetic JPEG only -- never opens the camera.
#include "video/jpeg_decode.h"
#include "video/video_decoder.h"
#include "video/video_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace p2p::video;

namespace {

// Full-range 4:2:2 JPEG like a webcam's.
std::vector<uint8_t> makeJpeg(int w, int h)
{
    const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!c) return {};
    AVCodecContext* e = avcodec_alloc_context3(c);
    e->width = w;
    e->height = h;
    e->pix_fmt = AV_PIX_FMT_YUVJ422P;
    e->time_base = {1, 30};
    e->flags |= AV_CODEC_FLAG_QSCALE;
    e->global_quality = FF_QP2LAMBDA * 2;
    std::vector<uint8_t> out;
    if (avcodec_open2(e, c, nullptr) == 0) {
        AVFrame* f = av_frame_alloc();
        f->format = e->pix_fmt;
        f->width = w;
        f->height = h;
        av_frame_get_buffer(f, 0);
        // Top: 8 RGB colour bars (in gamut, like real pictures); bottom: a
        // grey ramp. Converted to full-range BT.601 YCbCr, 4:2:2.
        static const int bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                       {255, 0, 255}, {255, 0, 0},     {0, 0, 255},   {0, 0, 0}};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; x += 2) {
                double cb = 0, cr = 0;
                for (int k = 0; k < 2; ++k) {
                    double r, g, bl;
                    if (y < h / 2) {
                        const int* c = bars[(x + k) * 8 / w];
                        r = c[0], g = c[1], bl = c[2];
                    } else {
                        r = g = bl = (x + k) * 255.0 / (w - 1);
                    }
                    const double yy = 0.299 * r + 0.587 * g + 0.114 * bl;
                    f->data[0][y * f->linesize[0] + x + k] = uint8_t(std::lround(yy));
                    cb += (bl - yy) / 1.772 / 2;
                    cr += (r - yy) / 1.402 / 2;
                }
                f->data[1][y * f->linesize[1] + x / 2] = uint8_t(std::clamp(std::lround(128 + cb), 0L, 255L));
                f->data[2][y * f->linesize[2] + x / 2] = uint8_t(std::clamp(std::lround(128 + cr), 0L, 255L));
            }
        f->quality = e->global_quality;
        AVPacket* p = av_packet_alloc();
        if (avcodec_send_frame(e, f) == 0 && avcodec_receive_packet(e, p) == 0) out.assign(p->data, p->data + p->size);
        av_packet_free(&p);
        av_frame_free(&f);
    }
    avcodec_free_context(&e);
    return out;
}

} // namespace

struct Diff {
    double madY = 0, madC = 0, maxD = 0;
};
Diff compare(const VideoFrame& a, const VideoFrame& b)
{
    Diff d;
    if (!a.ensureCpu() || !b.ensureCpu()) {
        d.madY = d.madC = d.maxD = 1e9;
        return d;
    }
    const size_t ny = size_t(a.width) * size_t(a.height), n = ny * 3 / 2;
    for (size_t i = 0; i < n; ++i) {
        const double x = std::abs(int(a.data[i]) - int(b.data[i]));
        (i < ny ? d.madY : d.madC) += x;
        d.maxD = std::max(d.maxD, x);
    }
    d.madY /= double(ny);
    d.madC /= double(n - ny);
    return d;
}

VideoFrame sized(int w, int h)
{
    VideoFrame f;
    f.width = w;
    f.height = h;
    return f;
}

class TestJpegDecode : public QObject {
    Q_OBJECT
private slots:
    // First: it needs a fresh VA display (destroying surfaces elsewhere in
    // this process also turns Mesa's EFC shortcut off, hiding the bug).
    // Camera path end to end without a copy: GPU JPEG frame -> hardware HEVC
    // -> decoder, which must see full-range BT.601 and the same picture. Both
    // at the camera's size (Mesa's unscaled-repack shortcut once encoded
    // garbage here) and scaled down on the GPU (a lower rung).
    void zeroCopyEncodeKeepsColours_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::newRow("unscaled") << 1280 << 720;
        QTest::newRow("scaled") << 640 << 360;
    }
    void zeroCopyEncodeKeepsColours()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        QString why;
        auto hw = createVaapiJpegDecoder(&why);
        if (!hw) QSKIP(qPrintable(QStringLiteral("no VA-API JPEG decode: %1").arg(why)));
        VideoEncoder enc;
        VideoEncoder::Config ec;
        ec.width = w;
        ec.height = h;
        ec.fps = 30;
        ec.bitrateKbps = 8000;
        ec.colour = Colour::Bt601Full;
        if (!enc.init(ec, &why)) QSKIP(qPrintable(QStringLiteral("no hardware HEVC encoder: %1").arg(why)));
        VideoDecoder dec;
        QVERIFY(dec.init(p2p::media::Codec::HEVC, true, &why));

        const auto jpeg = makeJpeg(1280, 720);
        std::shared_ptr<VideoFrame> out;
        for (int i = 0; i < 5; ++i) {
            VideoFrame f = sized(1280, 720);
            QVERIFY(hw->decode(jpeg.data(), jpeg.size(), f));
            f.captureUs = 1000 + i * 33'000;
            std::vector<uint8_t> bits;
            QVERIFY(enc.encode(f, i == 0, [&](const uint8_t* d, size_t n, bool) { bits.insert(bits.end(), d, d + n); }));
            QVERIFY(f.data.empty()); // encoded straight from the GPU surface
            bool err = false;
            if (!bits.empty())
                if (auto o = dec.decode(bits.data(), bits.size(), &err, w, h)) out = o;
            QVERIFY(!err);
        }
        QVERIFY(out);
        QCOMPARE(out->width, w);
        QCOMPARE(out->colour, Colour::Bt601Full);
        // Received frames stay on the GPU and export as the Y / UV dma-bufs
        // the renderer imports (DRM R8 + GR88).
        QVERIFY(out->gpu);
        const DmaBufNv12* dma = out->gpu->dmaBuf();
        QVERIFY(dma);
        QCOMPARE(dma->width, w);
        QCOMPARE(dma->y.drmFormat, 0x20203852u);  // DRM_FORMAT_R8
        QCOMPARE(dma->uv.drmFormat, 0x38385247u); // DRM_FORMAT_GR88
        QVERIFY(dma->y.fd >= 0 && dma->uv.fd >= 0 && dma->y.pitch >= uint32_t(w));

        // Against the software decode at the same size.
        auto sw = createSoftwareJpegDecoder();
        VideoFrame ref = sized(w, h);
        QVERIFY(sw->decode(jpeg.data(), jpeg.size(), ref));
        const Diff d = compare(*out, ref);
        qInfo("zero-copy 1280x720 -> %dx%d HEVC: luma MAD %.2f, chroma MAD %.2f", w, h, d.madY, d.madC);
        QVERIFY2(d.madY < 4 && d.madC < 4, "picture or colours changed on the way");
    }

    void softwareKeepsJpegColours()
    {
        const auto jpeg = makeJpeg(640, 360);
        QVERIFY(!jpeg.empty());
        auto sw = createSoftwareJpegDecoder();
        QVERIFY(sw);
        VideoFrame f = sized(640, 360);
        QVERIFY(sw->decode(jpeg.data(), jpeg.size(), f));
        QCOMPARE(f.colour, Colour::Bt601Full);
        QVERIFY(!f.gpu);
        // Full range: the black and white bars stay at ~0 and ~255.
        const auto [lo, hi] = std::minmax_element(f.y(), f.y() + 640 * 360);
        QVERIFY(*lo <= 3);
        QVERIFY(*hi >= 252);
    }

    void hardwareMatchesSoftware()
    {
        QString why;
        auto hw = createVaapiJpegDecoder(&why);
        if (!hw) QSKIP(qPrintable(QStringLiteral("no VA-API JPEG decode: %1").arg(why)));
        auto sw = createSoftwareJpegDecoder();
        for (auto [w, h] : {std::pair{640, 360}, std::pair{1280, 720}}) {
            const auto jpeg = makeJpeg(w, h);
            QVERIFY(!jpeg.empty());
            for (int i = 0; i < 3; ++i) { // reused surfaces
                VideoFrame a = sized(w, h);
                QVERIFY(hw->decode(jpeg.data(), jpeg.size(), a));
                QVERIFY(a.gpu);
                QCOMPARE(a.colour, Colour::Bt601Full);
                QVERIFY(a.data.empty()); // nothing downloaded until asked
                QVERIFY(a.ensureCpu());
                QVERIFY(a.ensureCpu()); // once
                if (i < 2) continue;
                VideoFrame b = sized(w, h);
                QVERIFY(sw->decode(jpeg.data(), jpeg.size(), b));
                const Diff d = compare(a, b);
                qInfo("%dx%d: luma MAD %.2f, chroma MAD %.2f, max %.0f", w, h, d.madY, d.madC, d.maxD);
                QVERIFY2(d.madY < 1.5 && d.madC < 1.5, "hardware and software pictures differ");
            }
        }
    }

};

QTEST_GUILESS_MAIN(TestJpegDecode)
#include "test_jpeg_decode.moc"
