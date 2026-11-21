#include "audio/audio_io.h"

#include <chrono>
#include <vector>

namespace p2p::audio {

bool SimulatedAudioIO::start(Callback cb, QString*)
{
    if (running_) return false;
    running_ = true;
    thread_ = std::thread([this, cb = std::move(cb)] {
        using clock = std::chrono::steady_clock;
        const int frames = 48 * periodMs_;
        std::vector<int16_t> out(static_cast<size_t>(frames));
        auto next = clock::now();
        while (running_.load(std::memory_order_relaxed)) {
            next += std::chrono::milliseconds(periodMs_);
            std::this_thread::sleep_until(next);
            cb(nullptr, out.data(), frames);
        }
    });
    return true;
}

void SimulatedAudioIO::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

} // namespace p2p::audio
