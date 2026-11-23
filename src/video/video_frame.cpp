#include "video/video_frame.h"

namespace p2p::video {

FrameHub& FrameHub::instance()
{
    static FrameHub hub;
    return hub;
}

void FrameHub::publish(const QString& stream, VideoFramePtr frame)
{
    {
        std::lock_guard l(m_);
        auto& s = slots_[stream];
        if (s.frame && s.shownGeneration != s.generation) ++s.superseded;
        s.frame = std::move(frame);
        ++s.generation;
    }
    emit frameAvailable(stream);
}

void FrameHub::noteShown(const QString& stream, uint64_t generation, int64_t swapUs)
{
    std::lock_guard l(m_);
    auto it = slots_.find(stream);
    if (it == slots_.end() || generation == 0 || it->second.shownGeneration == generation) return;
    auto& s = it->second;
    s.shownGeneration = generation;
    ++s.shown;
    s.shownAt.push_back(swapUs);
    while (s.shownAt.size() > 120) s.shownAt.pop_front();
    // Only the current frame is still known; a stale generation counts as shown but adds no timing.
    if (generation != s.generation || !s.frame) return;
    const double wait = s.frame->decodedUs ? double(swapUs - s.frame->decodedUs) / 1000.0 : NAN;
    s.display.add({wait, double(swapUs - s.frame->captureUs) / 1000.0});
}

FrameHub::DisplayStats FrameHub::displayStats(const QString& stream) const
{
    std::lock_guard l(m_);
    DisplayStats d;
    auto it = slots_.find(stream);
    if (it == slots_.end()) return d;
    d.shown = it->second.shown;
    d.superseded = it->second.superseded;
    const auto& at = it->second.shownAt;
    if (at.size() >= 2 && at.back() > at.front())
        d.shownFps = double(at.size() - 1) * 1e6 / double(at.back() - at.front());
    const auto sum = it->second.display.summary();
    d.wait = sum[0];
    d.total = sum[1];
    return d;
}

VideoFramePtr FrameHub::latest(const QString& stream, uint64_t* generation) const
{
    std::lock_guard l(m_);
    auto it = slots_.find(stream);
    if (it == slots_.end()) {
        if (generation) *generation = 0;
        return nullptr;
    }
    if (generation) *generation = it->second.generation;
    return it->second.frame;
}

void FrameHub::clear(const QString& stream)
{
    {
        std::lock_guard l(m_);
        auto& s = slots_[stream];
        s.frame.reset();
        ++s.generation;
    }
    emit frameAvailable(stream);
}

} // namespace p2p::video
