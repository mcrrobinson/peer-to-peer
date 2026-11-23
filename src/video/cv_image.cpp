#include "video/cv_image.h"

#include <CoreVideo/CoreVideo.h>

#include <cstring>

namespace p2p::video {

namespace {
class CvImage : public GpuImage {
public:
    explicit CvImage(CVPixelBufferRef pb) : pb_(CVPixelBufferRetain(pb)) {}
    ~CvImage() override { CVPixelBufferRelease(pb_); }
    void* nv12Surface(int, int) override { return nullptr; }
    void* cvPixelBuffer() override { return pb_; }
    bool download(VideoFrame& f) override { return copyCvPixelBuffer(pb_, f); }

private:
    CVPixelBufferRef pb_;
};
} // namespace

std::shared_ptr<GpuImage> wrapCvPixelBuffer(void* pixelBuffer)
{
    return std::make_shared<CvImage>(static_cast<CVPixelBufferRef>(pixelBuffer));
}

bool copyCvPixelBuffer(void* pixelBuffer, VideoFrame& f)
{
    auto pb = static_cast<CVPixelBufferRef>(pixelBuffer);
    if (!pb || CVPixelBufferGetPlaneCount(pb) != 2 || int(CVPixelBufferGetWidth(pb)) < f.width ||
        int(CVPixelBufferGetHeight(pb)) < f.height ||
        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return false;
    for (size_t p = 0; p < 2; ++p) {
        const auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, p));
        const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        uint8_t* dst = p ? f.uv() : f.y();
        for (int r = 0; r < (p ? f.height / 2 : f.height); ++r)
            std::memcpy(dst + size_t(r) * size_t(f.width), src + size_t(r) * stride, size_t(f.width));
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return true;
}

} // namespace p2p::video
