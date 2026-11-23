#include "core/clock.h"
#include "video/video_source.h"

#include <chrono>
#include <cstring>

namespace p2p::video {

namespace {
constexpr int kBlock = 32;   // code block size (luma pixels)
constexpr int kMargin = 16;
constexpr int kPitch = 36;   // block spacing
constexpr uint8_t kBlack = 16, kWhite = 235;
} // namespace

bool TestPatternSource::start(Callback cb, QString* error)
{
    if (running_) return false;
    if (w_ < kMargin * 2 + kPitch * kCodeBits || h_ < 120) {
        if (error) *error = QStringLiteral("test pattern needs at least %1x120").arg(kMargin * 2 + kPitch * kCodeBits);
        return false;
    }
    running_ = true;
    thread_ = std::thread([this, cb = std::move(cb)] {
        using clock = std::chrono::steady_clock;
        const auto interval = std::chrono::nanoseconds(1'000'000'000LL / fps_);
        auto next = clock::now();
        uint32_t n = 0;
        while (running_.load(std::memory_order_relaxed)) {
            auto f = std::make_shared<VideoFrame>();
            f->allocate(w_, h_);
            f->id = n;
            render(*f, n++);
            f->captureUs = nowUs();
            cb(std::move(f));
            next += interval;
            if (clock::now() > next + interval * 2) next = clock::now(); // fell behind: don't burst
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}

void TestPatternSource::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

QString TestPatternSource::description() const
{
    return QStringLiteral("test pattern %1x%2 @ %3 fps").arg(w_).arg(h_).arg(fps_);
}

void TestPatternSource::render(VideoFrame& f, uint32_t frameNo)
{
    const int w = f.width, h = f.height;
    uint8_t* y = f.y();
    uint8_t* uv = f.uv();

    // Moving diagonal luma gradient plus a sweeping vertical bar.
    const int shift = int(frameNo * 4);
    const int bar = int((frameNo * 8) % uint32_t(w));
    for (int row = 0; row < h; ++row) {
        uint8_t* line = y + size_t(row) * size_t(w);
        for (int col = 0; col < w; ++col) {
            int v = 40 + ((col + row + shift) & 0xff) * 150 / 255;
            if (col >= bar && col < bar + 24) v = 220;
            line[col] = uint8_t(v);
        }
    }
    // Slowly rotating chroma.
    const int cu = 128 + int(60.0 * ((frameNo % 240) / 240.0) - 30);
    for (int row = 0; row < h / 2; ++row) {
        uint8_t* line = uv + size_t(row) * size_t(w);
        for (int col = 0; col < w / 2; ++col) {
            line[2 * col] = uint8_t(cu + (row * 64 / (h / 2)) - 32);
            line[2 * col + 1] = uint8_t(128 + (col * 64 / (w / 2)) - 32);
        }
    }
    // Frame-number code: black backing strip, then one block per bit (MSB first).
    for (int row = kMargin - 8; row < kMargin + kBlock + 8; ++row)
        std::memset(y + size_t(row) * size_t(w) + kMargin - 8, kBlack, size_t(kPitch * kCodeBits + 16));
    for (int b = 0; b < kCodeBits; ++b) {
        const uint8_t v = (frameNo >> (kCodeBits - 1 - b)) & 1 ? kWhite : kBlack;
        const int x0 = kMargin + b * kPitch;
        for (int row = kMargin; row < kMargin + kBlock; ++row)
            std::memset(y + size_t(row) * size_t(w) + x0, v, kBlock);
    }
    // Neutral chroma under the code so it survives subsampling cleanly.
    for (int row = (kMargin - 8) / 2; row < (kMargin + kBlock + 8) / 2; ++row)
        std::memset(uv + size_t(row) * size_t(w) + kMargin - 8, 128, size_t(kPitch * kCodeBits + 16));
}

std::optional<uint32_t> TestPatternSource::readCode(const VideoFrame& f)
{
    if (f.width < kMargin * 2 + kPitch * kCodeBits || f.height < kMargin + kBlock) return std::nullopt;
    if (!f.ensureCpu()) return std::nullopt; // a GPU-decoded frame: read it back
    uint32_t v = 0;
    for (int b = 0; b < kCodeBits; ++b) {
        // Average the block centre to ride out compression noise.
        const int x0 = kMargin + b * kPitch + kBlock / 4, y0 = kMargin + kBlock / 4;
        int sum = 0;
        for (int row = y0; row < y0 + kBlock / 2; ++row)
            for (int col = x0; col < x0 + kBlock / 2; ++col) sum += f.y()[size_t(row) * size_t(f.width) + size_t(col)];
        const int avg = sum / (kBlock / 2 * kBlock / 2);
        if (avg > 90 && avg < 160) return std::nullopt; // ambiguous
        v = (v << 1) | (avg >= 128 ? 1u : 0u);
    }
    return v;
}

} // namespace p2p::video
