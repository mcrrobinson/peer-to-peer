#include "video/video_source.h"

namespace p2p::video {

namespace {
class NoCamera : public VideoSource {
public:
    bool start(Callback, QString* error) override
    {
        if (error) *error = QStringLiteral("camera capture not implemented on this platform yet");
        return false;
    }
    void stop() override {}
    QString description() const override { return QStringLiteral("no camera"); }
    int fps() const override { return 0; }
};
} // namespace

std::unique_ptr<VideoSource> createCameraSource(const CaptureConfig&) { return std::make_unique<NoCamera>(); }

} // namespace p2p::video
