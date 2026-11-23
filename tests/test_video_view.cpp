// VideoView on Metal (macOS): a VideoToolbox-decoded frame shown without a
// copy (CVPixelBuffer -> Metal textures) must look the same as the same
// picture uploaded from the CPU (the established path). Opens a small window.
#include "app/video_view.h"
#include "video/video_decoder.h"
#include "video/video_encoder.h"
#include "video/video_source.h"

#include <QQuickWindow>
#include <QRegularExpression>
#include <QTest>

#include <cmath>

using namespace p2p::video;
using p2p::app::VideoView;

namespace {

struct Decoded {
    std::shared_ptr<VideoFrame> gpu;   // last frame, VideoToolbox (on the GPU)
    std::shared_ptr<VideoFrame> cpu;   // the same frame, software decoder (bit-identical)
    std::shared_ptr<VideoFrame> other; // an earlier frame, software decoder
};

// Decodes |n| test-pattern frames (with detail) in hardware and software.
Decoded decodedFrames(int w, int h, int n, Colour colour)
{
    VideoEncoder enc;
    VideoEncoder::Config c;
    c.width = w;
    c.height = h;
    c.fps = 30;
    c.bitrateKbps = 4000;
    c.colour = colour;
    QString why;
    VideoDecoder dec, sw;
    if (!enc.init(c, &why) || !dec.init(p2p::media::Codec::HEVC, true, &why) || !dec.hardware() ||
        !sw.init(p2p::media::Codec::HEVC, false, &why))
        return {};
    Decoded r;
    for (int i = 0; i < n; ++i) {
        VideoFrame f;
        f.allocate(w, h);
        f.colour = colour;
        TestPatternSource::render(f, uint32_t(1000 + i), 2);
        std::vector<uint8_t> bits;
        if (!enc.encode(f, i == 0, [&](const uint8_t* d, size_t s, bool) { bits.insert(bits.end(), d, d + s); }))
            return {};
        if (bits.empty()) continue;
        bool err = false, swErr = false;
        auto g = dec.decode(bits.data(), bits.size(), &err);
        auto s = sw.decode(bits.data(), bits.size(), &swErr);
        if (!g || err || !s || swErr) return {};
        if (i == n / 2) r.other = s;
        r.gpu = g;
        r.cpu = s;
    }
    return r;
}

// Mean absolute difference per channel, and how much the picture varies.
void compare(const QImage& a, const QImage& b, double* meanDiff, double* spread)
{
    double diff = 0, sum = 0, sumSq = 0;
    const int n = a.width() * a.height();
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            diff += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
            const double l = qGray(p);
            sum += l;
            sumSq += l * l;
        }
    *meanDiff = diff / (3.0 * n);
    const double mean = sum / n;
    *spread = std::sqrt(std::max(0.0, sumSq / n - mean * mean));
}

} // namespace

class TestVideoView : public QObject {
    Q_OBJECT
private slots:
    void gpuFrameMatchesUpload_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addColumn<int>("colour");
        QTest::newRow("1280x720 BT.709") << 1280 << 720 << int(Colour::Bt709Limited);
        QTest::newRow("1280x720 BT.601 full") << 1280 << 720 << int(Colour::Bt601Full);
        QTest::newRow("426x240") << 426 << 240 << int(Colour::Bt709Limited);
    }
    void gpuFrameMatchesUpload()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        QFETCH(int, colour);
        const Decoded fr = decodedFrames(w, h, 10, Colour(colour));
        if (!fr.gpu) QSKIP("no VideoToolbox HEVC encode + decode");
        QVERIFY(fr.gpu->gpu && fr.gpu->gpu->cvPixelBuffer());
        QVERIFY(fr.cpu && !fr.cpu->gpu && fr.other);
        // Falling back to the upload path would also match: not allowed.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("video view:")));

        QQuickWindow window;
        window.resize(w / 2, h / 2); // same aspect as the frame: fills the window
        auto* view = new VideoView(window.contentItem());
        view->setStream(QStringLiteral("test"));
        view->setWidth(window.width());
        view->setHeight(window.height());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        if (window.rendererInterface()->graphicsApi() != QSGRendererInterface::Metal) QSKIP("not on the Metal backend");

        auto& hub = FrameHub::instance();
        QTest::ignoreMessage(QtInfoMsg, "video view: GPU frames shown without a copy (CVPixelBuffer -> Metal texture)");
        auto grab = [&](const VideoFramePtr& f) {
            hub.publish(QStringLiteral("test"), f);
            QCoreApplication::processEvents();
            return window.grabWindow();
        };
        const QImage a = grab(fr.gpu), b = grab(fr.cpu), c = grab(fr.other);
        hub.clear(QStringLiteral("test"));
        QVERIFY(!a.isNull() && a.size() == b.size() && b.size() == c.size());

        double diff = 0, spread = 0, otherDiff = 0, unused = 0;
        compare(a, b, &diff, &spread);
        compare(b, c, &otherDiff, &unused);
        qInfo("%dx%d: grab %dx%d, mean |gpu - upload| %.3f per channel (another frame: %.1f), luma spread %.1f", w, h,
              a.width(), a.height(), diff, otherDiff, spread);
        QVERIFY2(spread > 20, "picture looks blank");
        QVERIFY2(otherDiff > 5, "grabs don't follow the published frame");
        // Same samples, same shader: only sampling of a differently made
        // texture could differ.
        QVERIFY2(diff < 1.0, "GPU frame differs from the uploaded one");
        QVERIFY(!fr.gpu->data.size()); // the renderer never read it back
    }
};

QTEST_MAIN(TestVideoView)
#include "test_video_view.moc"
