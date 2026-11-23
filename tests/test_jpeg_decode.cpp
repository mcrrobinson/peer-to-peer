// Hardware (VA-API) vs. software camera MJPEG decode: same picture, same
// colours. Synthetic JPEG only -- never opens the camera.
#include "video/jpeg_decode.h"

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

class TestJpegDecode : public QObject {
    Q_OBJECT
private slots:
    void softwareDecodesToLimitedRange()
    {
        const auto jpeg = makeJpeg(640, 360);
        QVERIFY(!jpeg.empty());
        auto sw = createSoftwareJpegDecoder();
        QVERIFY(sw);
        VideoFrame f;
        f.allocate(640, 360);
        QVERIFY(sw->decode(jpeg.data(), jpeg.size(), f));
        const auto [lo, hi] = std::minmax_element(f.y(), f.y() + 640 * 360);
        QVERIFY(*lo >= 14 && *lo <= 20);
        QVERIFY(*hi >= 230 && *hi <= 237);
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
            VideoFrame a, b;
            a.allocate(w, h);
            b.allocate(w, h);
            for (int i = 0; i < 3; ++i) QVERIFY(hw->decode(jpeg.data(), jpeg.size(), a)); // reused surfaces
            QVERIFY(sw->decode(jpeg.data(), jpeg.size(), b));
            const size_t ny = size_t(w) * size_t(h), n = ny * 3 / 2;
            double madY = 0, madC = 0, maxD = 0;
            for (size_t i = 0; i < n; ++i) {
                const double d = std::abs(int(a.data[i]) - int(b.data[i]));
                (i < ny ? madY : madC) += d;
                maxD = std::max(maxD, d);
            }
            madY /= double(ny);
            madC /= double(n - ny);
            qInfo("%dx%d: luma MAD %.2f, chroma MAD %.2f, max %.0f", w, h, madY, madC, maxD);
            QVERIFY2(madY < 1.5 && madC < 1.5, "colours differ from the software path");
        }
    }
};

QTEST_GUILESS_MAIN(TestJpegDecode)
#include "test_jpeg_decode.moc"
