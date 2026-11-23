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

// A tightly packed NV12 picture (Y plane, then interleaved UV at half
// resolution), BT.709 limited range.
struct VideoFrame {
    int width = 0;
    int height = 0;
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
