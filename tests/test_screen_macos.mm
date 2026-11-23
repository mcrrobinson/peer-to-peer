// ScreenCaptureKit source (screen_sck.mm) on the main display, no picker:
// GPU frames at the configured cap, frame rate, repeats on request, stop.
// Skips unless Screen Recording access was already granted (never asks).
// The picker path needs a person to click: see plan.md for the manual runs.
#include "core/clock.h"
#include "video/video_source.h"

#import <CoreGraphics/CoreGraphics.h>

#include <QTest>

#include <atomic>
#include <mutex>
#include <vector>

using namespace p2p::video;

namespace {

struct Collected {
    std::mutex m;
    std::shared_ptr<VideoFrame> last; // only the newest: frames hold capture buffers
    std::vector<int64_t> times;       // captureUs of new (not repeated) frames
    std::atomic<int> count{0}, repeated{0};
    std::atomic<bool> ended{false};
    QString why;
};

std::unique_ptr<VideoSource> startScreen(ScreenConfig cfg, Collected& c)
{
    cfg.mainDisplay = true;
    auto src = createScreenSource(cfg, [&c](QString why) {
        std::lock_guard l(c.m);
        c.why = why;
        c.ended = true;
    });
    QString err;
    if (!src->start(
            [&c](std::shared_ptr<VideoFrame> f) {
                std::lock_guard l(c.m);
                if (f->repeated) ++c.repeated;
                else c.times.push_back(f->captureUs);
                c.last = std::move(f);
                ++c.count;
            },
            &err))
        return nullptr;
    return src;
}

} // namespace

class TestScreenMacos : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        if (!CGPreflightScreenCaptureAccess())
            QSKIP("Screen Recording access not granted (System Settings > Privacy & Security > Screen & System "
                  "Audio Recording)");
    }

    void deliversGpuFramesWithinLimits()
    {
        Collected c;
        ScreenConfig cfg;
        cfg.mode = ScreenConfig::Mode::Text;
        cfg.maxFps = 30;
        cfg.maxWidth = 1280;
        cfg.maxHeight = 720;
        auto src = startScreen(cfg, c);
        QVERIFY(src);
        // Something has to change on screen for new frames: the first one
        // always comes; repeats follow on request.
        QTRY_VERIFY_WITH_TIMEOUT(c.count >= 1, 5000);
        const int before = c.repeated;
        src->requestFrame();
        QTRY_VERIFY_WITH_TIMEOUT(c.repeated > before, 1000);
        QTest::qWait(1500); // idle repeats about once a second, new frames at <= 30 fps
        src->stop();
        const int atStop = c.count;
        QTest::qWait(300);
        QCOMPARE(c.count.load(), atStop);
        QVERIFY2(!c.ended, qPrintable(c.why));

        std::lock_guard l(c.m);
        const auto& f = *c.last;
        qInfo("%s: %d frames (%d repeated), last %dx%d", qPrintable(src->description()), atStop, c.repeated.load(),
              f.width, f.height);
        QVERIFY(f.gpu && f.gpu->cvPixelBuffer());
        QVERIFY(f.width <= 1280 && f.height <= 720 && f.width >= 16 && f.height >= 16);
        QVERIFY(f.width == 1280 || f.height == 720); // scaled to fit, aspect kept
        QCOMPARE(int(f.colour), int(Colour::Bt709Limited));
        // The cap: new frames at least ~1/30 s apart (some jitter allowed).
        for (size_t i = 1; i < c.times.size(); ++i)
            QVERIFY2(c.times[i] - c.times[i - 1] > 25'000,
                     qPrintable(QStringLiteral("frames %1 us apart").arg(c.times[i] - c.times[i - 1])));
        QVERIFY(f.ensureCpu()); // readable
    }

    void stopsWhileStarting()
    {
        Collected c;
        auto src = startScreen(ScreenConfig{}, c);
        QVERIFY(src);
        src->stop(); // before the first frame: no crash, no callback afterwards
        const int n = c.count;
        QTest::qWait(500);
        QCOMPARE(c.count.load(), n);
    }
};

QTEST_GUILESS_MAIN(TestScreenMacos)
#include "test_screen_macos.moc"
