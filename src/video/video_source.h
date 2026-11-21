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
};

// V4L2 camera (Linux). macOS AVFoundation capture arrives with the macOS port.
std::unique_ptr<VideoSource> createCameraSource(const CaptureConfig& cfg);

// Synthetic moving picture with the frame number encoded in a row of
// black/white blocks so the receiver can verify what it decoded.
class TestPatternSource : public VideoSource {
public:
    static constexpr int kCodeBits = 32;
    TestPatternSource(int width, int height, int fps) : w_(width & ~1), h_(height & ~1), fps_(fps) {}
    ~TestPatternSource() override { stop(); }
    bool start(Callback cb, QString* error) override;
    void stop() override;
    QString description() const override;
    int fps() const override { return fps_; }

    static void render(VideoFrame& f, uint32_t frameNo);
    // Reads the frame number back from a (decoded) frame; nullopt if unreadable.
    static std::optional<uint32_t> readCode(const VideoFrame& f);

private:
    int w_, h_, fps_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace p2p::video
