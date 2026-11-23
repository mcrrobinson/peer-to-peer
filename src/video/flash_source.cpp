// Black / white flash source for the glass-to-glass probe: no camera needed
// to time the encode -> network -> decode -> display path.
#include "core/clock.h"
#include "video/video_source.h"

#include <QFile>
#include <QRandomGenerator>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace p2p::video {

namespace {
class FlashSource : public VideoSource {
public:
    FlashSource(int w, int h, int fps, QString log) : w_(w & ~1), h_(h & ~1), fps_(std::max(1, fps)), log_(std::move(log))
    {
    }
    ~FlashSource() override { stop(); }

    bool start(Callback cb, QString* error) override
    {
        if (running_) return false;
        log_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text | QIODevice::Unbuffered);
        if (!log_.isOpen()) {
            if (error) *error = QStringLiteral("can't write the flash log %1").arg(log_.fileName());
            return false;
        }
        running_ = true;
        thread_ = std::thread([this, cb = std::move(cb)] {
            using clock = std::chrono::steady_clock;
            const auto interval = std::chrono::nanoseconds(1'000'000'000LL / fps_);
            auto next = clock::now();
            bool white = false;
            int64_t switchAt = nowUs() + 1'000'000;
            uint32_t n = 0;
            while (running_.load(std::memory_order_relaxed)) {
                auto f = std::make_shared<VideoFrame>();
                f->allocate(w_, h_);
                f->id = n++;
                f->captureUs = nowUs();
                bool switched = false;
                if (f->captureUs >= switchAt) {
                    white = !white;
                    switched = true;
                    switchAt = f->captureUs + QRandomGenerator::global()->bounded(500'000, 900'000);
                }
                std::memset(f->y(), white ? 235 : 16, size_t(w_) * size_t(h_));
                std::memset(f->uv(), 128, size_t(w_) * size_t(h_) / 2);
                if (switched) log_.write(QStringLiteral("%1 %2\n").arg(f->captureUs).arg(white ? 1 : 0).toUtf8());
                cb(std::move(f));
                next += interval;
                if (clock::now() > next + interval * 2) next = clock::now();
                std::this_thread::sleep_until(next);
            }
        });
        return true;
    }

    void stop() override
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        log_.close();
    }
    QString description() const override
    {
        return QStringLiteral("flash %1x%2 @ %3 fps -> %4").arg(w_).arg(h_).arg(fps_).arg(log_.fileName());
    }
    int fps() const override { return fps_; }

private:
    int w_, h_, fps_;
    QFile log_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
} // namespace

std::unique_ptr<VideoSource> createFlashSource(int width, int height, int fps, const QString& logPath)
{
    return std::make_unique<FlashSource>(width, height, fps, logPath);
}

} // namespace p2p::video
