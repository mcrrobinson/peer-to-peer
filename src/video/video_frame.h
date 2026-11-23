#pragma once

#include "video/latency_stages.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace p2p::video {

struct VideoFrame;

// An NV12 picture as two dma-buf planes (Y as DRM R8, UV as DRM GR88), for
// showing a GPU frame without a copy. The fds belong to the GpuImage.
struct DmaBufPlane {
    int fd = -1;
    uint32_t offset = 0, pitch = 0;
    uint32_t drmFormat = 0;
    uint64_t modifier = 0;
};
struct DmaBufNv12 {
    int width = 0, height = 0; // the picture (the buffer may be padded)
    DmaBufPlane y, uv;
};

// Colours of a frame's YUV: what the encoder signals in the bitstream and
// the renderer converts from. Camera MJPEG stays in the JPEG's own colours
// (no conversion on the send path); everything else is BT.709 limited.
enum class Colour : uint8_t { Bt709Limited, Bt601Full };

// A frame that lives on the GPU (Linux: a VA-API surface; macOS: a
// VideoToolbox CVPixelBuffer). The VA-API encoder takes
// it as is; CPU readers call VideoFrame::ensureCpu(), which downloads once.
class GpuImage {
public:
    virtual ~GpuImage() = default;
    // The picture as an NV12 VA surface at w x h (scaled on the GPU), cached
    // per size; an AVFrame* (AV_PIX_FMT_VAAPI) owned by this image, or
    // nullptr. Thread-safe.
    virtual void* nv12Surface(int w, int h) = 0;
    // Fills |f|'s CPU planes (f.width x f.height); called once, under ensureCpu.
    virtual bool download(VideoFrame& f) = 0;
    // The picture (at its own size) as dma-bufs, exported once and kept
    // while the image lives; nullptr if not possible. Thread-safe.
    virtual const DmaBufNv12* dmaBuf() { return nullptr; }
    // macOS: the picture as an NV12 CVPixelBufferRef (IOSurface-backed),
    // owned by this image; nullptr if it isn't one.
    virtual void* cvPixelBuffer() { return nullptr; }

private:
    friend struct VideoFrame;
    std::mutex cpuMutex_;
    int cpuState_ = 0; // 0 not yet, 1 done, -1 failed
};

// A tightly packed NV12 picture (Y plane, then interleaved UV at half
// resolution) in |colour|. With |gpu| set, |data| stays empty until
// ensureCpu().
struct VideoFrame {
    int width = 0;
    int height = 0;
    Colour colour = Colour::Bt709Limited;
    std::shared_ptr<GpuImage> gpu;
    int64_t captureUs = 0; // capture time on the clock of the machine that captured it
    int64_t decodedUs = 0; // receiver: decoder output (local clock); 0 for local frames
    bool repeated = false; // a screen source re-sent an unchanged picture (new timestamp, old content)
    uint32_t id = 0;
    std::vector<uint8_t> data;

    const uint8_t* y() const { return data.data(); }
    const uint8_t* uv() const { return data.data() + size_t(width) * size_t(height); }
    uint8_t* y() { return data.data(); }
    uint8_t* uv() { return data.data() + size_t(width) * size_t(height); }
    void allocate(int w, int h)
    {
        width = w;
        height = h;
        data.resize(size_t(w) * size_t(h) * 3 / 2);
    }
    // CPU planes available (downloads a GPU frame once; any thread). Frames
    // are created non-const, so filling |data| here is well-defined.
    bool ensureCpu() const
    {
        if (!gpu) return !data.empty();
        std::lock_guard l(gpu->cpuMutex_);
        if (gpu->cpuState_ == 0) {
            auto& self = const_cast<VideoFrame&>(*this);
            self.data.resize(size_t(width) * size_t(height) * 3 / 2);
            gpu->cpuState_ = gpu->download(self) ? 1 : -1;
        }
        return gpu->cpuState_ == 1;
    }
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

// Latest-frame mailboxes ("local" preview, "remote" decoded) shared between
// media threads and the renderer. Only the newest frame is kept: a slow
// renderer drops frames instead of building latency.
class FrameHub : public QObject {
    Q_OBJECT
public:
    static FrameHub& instance();

    void publish(const QString& stream, VideoFramePtr frame); // any thread
    VideoFramePtr latest(const QString& stream, uint64_t* generation = nullptr) const;
    void clear(const QString& stream);

    // The renderer put |generation| of |stream| on screen (swap returned at
    // |swapUs|). Feeds the display part of the latency breakdown.
    void noteShown(const QString& stream, uint64_t generation, int64_t swapUs);
    struct DisplayStats {
        uint64_t shown = 0, superseded = 0; // superseded: replaced before it was ever shown
        StageStats::Stage wait, total;      // decoded -> swap, capture -> swap (ms)
        double shownFps = 0;                // new frames put on screen per second (recent)
    };
    DisplayStats displayStats(const QString& stream) const;

signals:
    void frameAvailable(const QString& stream);

private:
    struct Slot {
        VideoFramePtr frame;
        uint64_t generation = 0;
        uint64_t shownGeneration = 0;
        uint64_t shown = 0, superseded = 0;
        StageStats display{{"wait", "total"}};
        std::deque<int64_t> shownAt; // recent swap times, for shownFps
    };
    mutable std::mutex m_;
    std::map<QString, Slot> slots_;
};

} // namespace p2p::video
