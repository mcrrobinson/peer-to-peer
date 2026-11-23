#include "app/metal_nv12_import.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <array>

namespace p2p::app {

namespace {
// Imports kept alive: the frame on screen plus what the GPU may still be
// drawing (QRhi's Metal backend has up to 2 frames in flight).
constexpr size_t kKeep = 4;
} // namespace

struct MetalNv12Importer::Impl {
    CVMetalTextureCacheRef cache = nullptr;
    std::array<std::array<CVMetalTextureRef, 2>, kKeep> kept{};
    size_t next = 0;

    void releaseSlot(size_t i)
    {
        for (auto& t : kept[i])
            if (t) {
                CFRelease(t);
                t = nullptr;
            }
    }
    ~Impl()
    {
        for (size_t i = 0; i < kKeep; ++i) releaseSlot(i);
        if (cache) CFRelease(cache);
    }
};

MetalNv12Importer::MetalNv12Importer() : d(std::make_unique<Impl>()) {}
MetalNv12Importer::~MetalNv12Importer() = default;

bool MetalNv12Importer::init(void* mtlDevice)
{
    if (!mtlDevice) return false;
    const CVReturn r = CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, (__bridge id<MTLDevice>)mtlDevice,
                                                 nullptr, &d->cache);
    if (r != kCVReturnSuccess) {
        error_ = QStringLiteral("CVMetalTextureCacheCreate: %1").arg(r);
        return false;
    }
    return true;
}

bool MetalNv12Importer::import(void* pixelBuffer, int w, int h, uint64_t tex[2])
{
    auto pb = static_cast<CVPixelBufferRef>(pixelBuffer);
    if (!d->cache || !pb) return false;
    const OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    if ((fmt != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && fmt != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
        int(CVPixelBufferGetWidthOfPlane(pb, 0)) < w || int(CVPixelBufferGetHeightOfPlane(pb, 0)) < h) {
        error_ = QStringLiteral("unsupported pixel buffer (format 0x%1, %2x%3)")
                     .arg(fmt, 0, 16)
                     .arg(CVPixelBufferGetWidth(pb))
                     .arg(CVPixelBufferGetHeight(pb));
        return false;
    }
    const size_t slot = d->next;
    d->next = (d->next + 1) % kKeep;
    d->releaseSlot(slot); // the oldest import: no longer in flight
    const MTLPixelFormat formats[2] = {MTLPixelFormatR8Unorm, MTLPixelFormatRG8Unorm};
    for (size_t p = 0; p < 2; ++p) {
        const int div = p ? 2 : 1;
        CVMetalTextureRef t = nullptr;
        const CVReturn r = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, d->cache, pb, nullptr,
                                                                     formats[p], size_t(w / div), size_t(h / div), p, &t);
        if (r != kCVReturnSuccess || !t) {
            error_ = QStringLiteral("CVMetalTextureCacheCreateTextureFromImage plane %1: %2").arg(p).arg(r);
            d->releaseSlot(slot);
            return false;
        }
        d->kept[slot][p] = t;
        tex[p] = uint64_t(reinterpret_cast<uintptr_t>((__bridge void*)CVMetalTextureGetTexture(t)));
    }
    CVMetalTextureCacheFlush(d->cache, 0);
    return true;
}

} // namespace p2p::app
