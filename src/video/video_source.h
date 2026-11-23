#pragma once

#include "video/video_frame.h"

#include <QString>

#include <atomic>
#include <functional>
#include <optional>
#include <memory>
#include <thread>

namespace p2p::video {

struct CaptureConfig {
    enum class Format { Auto, Mjpeg, Nv12 };
    QString device = QStringLiteral("/dev/video0");
    int width = 1920;
    int height = 1080;
    int fps = 60;
    Format format = Format::Auto; // Auto: NV12 if it supports the fps, else MJPEG
    bool hwJpegDecode = true;     // MJPEG: VA-API decode where the GPU has it
    // Test pattern only: % of luma pixels below the code strip replaced with
    // noise, for tests that need an encoder to spend its whole budget (the
    // plain pattern is a smooth gradient: VideoToolbox codes it far below
    // the target and doesn't pad). 0 = plain pattern.
    int patternDetail = 0;
};

struct ScreenConfig {
    enum class Mode { Text, Motion };
    Mode mode = Mode::Text;
    int maxFps = 30;          // text: 30, motion: 60
    int maxWidth = 3840;      // larger captures are scaled down to fit
    int maxHeight = 2160;
    QString restoreTokenFile; // portal restore token (skip the picker next time); empty = don't persist
    // macOS: capture the main display without the system picker (scripted
    // tests; needs Screen Recording access). Otherwise the picker asks.
    bool mainDisplay = false;
};

// Produces NV12 frames on its own thread. The callback runs on that thread
// and may take a while (it encodes); sources drop stale frames rather than
// queueing them.
class VideoSource {
public:
    using Callback = std::function<void(std::shared_ptr<VideoFrame> frame)>;
    virtual ~VideoSource() = default;
    virtual bool start(Callback cb, QString* error) = 0;
    virtual void stop() = 0;
    virtual QString description() const = 0;
    virtual int fps() const = 0;
    // Deliver the current picture again soon even if nothing changed (a
    // screen that only produces frames on damage; keyframe requests).
    virtual void requestFrame() {}
};

// Black / white frames that switch at random 0.5-0.9 s intervals, for the
// latency probe (tools/latency_probe --source-log): each switch's capture
// time is appended to |logPath| as "<captureUs> <0|1>".
std::unique_ptr<VideoSource> createFlashSource(int width, int height, int fps, const QString& logPath);

// V4L2 camera (Linux). macOS AVFoundation capture arrives with the macOS port.
std::unique_ptr<VideoSource> createCameraSource(const CaptureConfig& cfg);

// Screen capture: xdg-desktop-portal ScreenCast + PipeWire (Linux),
// ScreenCaptureKit with the system's content picker (macOS). start()
// returns at once: the user picks a screen or window and frames follow. Must be
// created, started and stopped on a thread with a Qt event loop (D-Bus).
// |onEnded| runs (any thread) when sharing stops by itself: the dialog was
// cancelled, the user stopped it from the desktop, or the stream failed.
std::unique_ptr<VideoSource> createScreenSource(const ScreenConfig& cfg, std::function<void(QString)> onEnded);

// Synthetic moving picture with the frame number encoded in a row of
// black/white blocks so the receiver can verify what it decoded.
class TestPatternSource : public VideoSource {
public:
    static constexpr int kCodeBits = 32;
    TestPatternSource(int width, int height, int fps, int detailPct = 0)
        : w_(width & ~1), h_(height & ~1), fps_(fps), detail_(detailPct) {}
    ~TestPatternSource() override { stop(); }
    bool start(Callback cb, QString* error) override;
    void stop() override;
    QString description() const override;
    int fps() const override { return fps_; }

    // |detailPct|: CaptureConfig::patternDetail.
    static void render(VideoFrame& f, uint32_t frameNo, int detailPct = 0);
    // Reads the frame number back from a (decoded) frame; nullopt if unreadable.
    static std::optional<uint32_t> readCode(const VideoFrame& f);

private:
    int w_, h_, fps_, detail_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace p2p::video
