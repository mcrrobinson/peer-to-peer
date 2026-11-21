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
        s.frame = std::move(frame);
        ++s.generation;
    }
    emit frameAvailable(stream);
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
