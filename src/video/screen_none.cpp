#include "video/video_source.h"

namespace p2p::video {

namespace {
class NoScreenSource : public VideoSource {
public:
    bool start(Callback, QString* error) override
    {
        if (error) *error = QStringLiteral("screen capture is not available on this platform yet");
        return false;
    }
    void stop() override {}
    QString description() const override { return QStringLiteral("no screen capture"); }
    int fps() const override { return 0; }
};
} // namespace

std::unique_ptr<VideoSource> createScreenSource(const ScreenConfig&, std::function<void(QString)>)
{
    return std::make_unique<NoScreenSource>();
}

} // namespace p2p::video
