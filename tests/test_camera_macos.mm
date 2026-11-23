// AVFoundation camera source (camera_avfoundation.mm) on the real camera:
// frame size and rate, GPU frames, timestamps, format choice, stop. Skips
// unless camera access was already granted (never asks: run p2pcall with
// the camera once to get the prompt). The camera light comes on.
#include "core/clock.h"
#include "video/video_source.h"

#import <AVFoundation/AVFoundation.h>

#include <QTest>

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

using namespace p2p::video;

namespace {

struct Collected {
    std::mutex m;
    std::vector<std::shared_ptr<VideoFrame>> frames;
    std::vector<int64_t> lagUs; // nowUs() at delivery - captureUs
    std::atomic<int> count{0};
};

std::unique_ptr<VideoSource> startCamera(int w, int h, int fps, Collected& c, QString* err, const QString& device = {})
{
    CaptureConfig cfg;
    cfg.device = device.isEmpty() ? QStringLiteral("default") : device;
    cfg.width = w;
    cfg.height = h;
    cfg.fps = fps;
    auto cam = createCameraSource(cfg);
    const bool ok = cam->start(
        [&c](std::shared_ptr<VideoFrame> f) {
            const int64_t lag = p2p::nowUs() - f->captureUs;
            std::lock_guard l(c.m);
            c.lagUs.push_back(lag);
            // Only the newest few: held frames keep the camera's buffers.
            if (c.frames.size() == 3) c.frames.erase(c.frames.begin());
            c.frames.push_back(std::move(f));
            ++c.count;
        },
        err);
    return ok ? std::move(cam) : nullptr;
}

} // namespace

class TestCameraMacos : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized)
            QSKIP("camera access not granted (run p2pcall with the camera once to be asked)");
        if (![AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo]) QSKIP("no camera");
    }

    void deliversGpuFrames()
    {
        Collected c;
        QString err;
        auto cam = startCamera(1280, 720, 30, c, &err);
        QVERIFY2(cam, qPrintable(err));
        QCOMPARE(cam->fps(), 30);
        qInfo("%s", qPrintable(cam->description()));
        QTRY_VERIFY_WITH_TIMEOUT(c.count >= 60, 8000);
        cam->stop();
        const int atStop = c.count;
        QTest::qWait(300);
        QCOMPARE(c.count.load(), atStop); // nothing after stop()

        std::lock_guard l(c.m);
        for (auto& f : c.frames) {
            QCOMPARE(f->width, 1280);
            QCOMPARE(f->height, 720);
            QVERIFY(f->gpu && f->gpu->cvPixelBuffer());
            QVERIFY(f->data.empty()); // not read back unless asked
            QCOMPARE(int(f->colour), int(Colour::Bt709Limited));
        }
        for (size_t i = 1; i < c.frames.size(); ++i) QVERIFY(c.frames[i]->id > c.frames[i - 1]->id);
        // Timestamps: in the recent past (the camera's own, carried over to
        // our clock), not in the future. Steady state ~40-50 ms on the M3's
        // FaceTime camera; the first frames take up to a few hundred.
        double lagSum = 0;
        int steady = 0;
        for (size_t i = 0; i < c.lagUs.size(); ++i) {
            const int64_t lag = c.lagUs[i];
            QVERIFY2(lag >= 0 && lag < (i < 10 ? 1'000'000 : 200'000),
                     qPrintable(QStringLiteral("frame %1: capture -> delivery %2 us").arg(i).arg(lag)));
            if (i >= 10) {
                lagSum += lag;
                ++steady;
            }
        }
        // Not a blank buffer: some variation even in a dark room (noise).
        auto& f = *c.frames.back();
        QVERIFY(f.ensureCpu());
        double sum = 0, sumSq = 0;
        const size_t n = size_t(f.width) * size_t(f.height);
        for (size_t i = 0; i < n; ++i) {
            sum += f.y()[i];
            sumSq += double(f.y()[i]) * f.y()[i];
        }
        const double mean = sum / n, sd = std::sqrt(std::max(0.0, sumSq / n - mean * mean));
        qInfo("%d frames, capture -> delivery avg %.1f ms (from frame 10), last frame luma mean %.1f sd %.1f", atStop,
              lagSum / steady / 1000.0, mean, sd);
        QVERIFY2(sd > 0.5, "camera picture is uniform");
    }

    // A size the camera has (640x480) is used as is; a size it hasn't
    // (1000x500) gets the smallest format that covers it.
    void picksFormat_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::newRow("640x480") << 640 << 480;
        QTest::newRow("1000x500") << 1000 << 500;
    }
    void picksFormat()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        Collected c;
        QString err;
        auto cam = startCamera(w, h, 60, c, &err);
        QVERIFY2(cam, qPrintable(err));
        QVERIFY(cam->fps() >= 15 && cam->fps() <= 60);
        QTRY_VERIFY_WITH_TIMEOUT(c.count >= 5, 8000);
        cam->stop();
        std::lock_guard l(c.m);
        const auto& f = *c.frames.back();
        qInfo("asked %dx%d, got %dx%d at %d fps", w, h, f.width, f.height, cam->fps());
        QVERIFY(f.width >= w && f.height >= h);
        if (w == 640) QCOMPARE(f.width, 640);
    }

    void unknownCameraFails()
    {
        Collected c;
        QString err;
        QVERIFY(!startCamera(1280, 720, 30, c, &err, QStringLiteral("no such camera xyz")));
        qInfo("%s", qPrintable(err));
        QVERIFY(err.contains(QLatin1String("no camera matching")));
    }
};

QTEST_GUILESS_MAIN(TestCameraMacos)
#include "test_camera_macos.moc"
