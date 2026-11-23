#pragma once

#include <QString>

#include <cstdint>
#include <memory>

namespace p2p::app {

// macOS: shows a VideoToolbox frame (NV12 CVPixelBuffer on an IOSurface)
// without a copy -- its Y and UV planes wrapped as Metal textures (R8 and
// RG8) through a CVMetalTextureCache, for QRhiTexture::createFrom. Render
// thread only.
class MetalNv12Importer {
public:
    MetalNv12Importer();
    ~MetalNv12Importer();
    MetalNv12Importer(const MetalNv12Importer&) = delete;
    MetalNv12Importer& operator=(const MetalNv12Importer&) = delete;

    // |mtlDevice|: the renderer's id<MTLDevice> (QRhiMetalNativeHandles::dev).
    bool init(void* mtlDevice);
    // id<MTLTexture>s (as integers) for |pixelBuffer|'s planes, |w| x |h|
    // (the picture; the buffer may be larger). They stay valid, and keep the
    // buffer alive, for the next few imports (frames in flight on the GPU).
    bool import(void* pixelBuffer, int w, int h, uint64_t tex[2]);
    QString error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    QString error_;
};

} // namespace p2p::app
