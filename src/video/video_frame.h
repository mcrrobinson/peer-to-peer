#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
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

signals:
    void frameAvailable(const QString& stream);

private:
    struct Slot {
        VideoFramePtr frame;
        uint64_t generation = 0;
    };
    mutable std::mutex m_;
    std::map<QString, Slot> slots_;
};

} // namespace p2p::video
