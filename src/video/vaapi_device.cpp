#include "video/vaapi_device.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}

#include <va/va.h>
#include <va/va_drmcommon.h>

#include <unistd.h>

#include <mutex>

namespace p2p::video {

AVBufferRef* sharedVaapiDevice()
{
    static std::mutex m;
    static AVBufferRef* device = nullptr; // kept for the process lifetime
    std::lock_guard l(m);
    if (!device && av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) device = nullptr;
    return device ? av_buffer_ref(device) : nullptr;
}

bool exportVaapiNv12(const AVFrame* frame, int w, int h, DmaBufNv12& out)
{
    if (!frame || !frame->hw_frames_ctx) return false;
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    VADisplay dpy = static_cast<AVVAAPIDeviceContext*>(fc->device_ctx->hwctx)->display;
    const VASurfaceID surface = VASurfaceID(uintptr_t(frame->data[3]));
    VADRMPRIMESurfaceDescriptor desc{};
    if (vaExportSurfaceHandle(dpy, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                              &desc) != VA_STATUS_SUCCESS)
        return false;
    auto closeAll = [&] {
        for (uint32_t i = 0; i < desc.num_objects; ++i) ::close(desc.objects[i].fd);
    };
    // Expect two single-plane layers (Y, UV) -- what NV12 exports as.
    if (desc.num_layers != 2 || desc.layers[0].num_planes != 1 || desc.layers[1].num_planes != 1) {
        closeAll();
        return false;
    }
    // Each plane gets its own fd (dup) so the two can be closed independently.
    auto plane = [&](int layer, DmaBufPlane& p) {
        const auto& L = desc.layers[layer];
        const auto& obj = desc.objects[L.object_index[0]];
        p.fd = ::dup(obj.fd);
        p.offset = L.offset[0];
        p.pitch = L.pitch[0];
        p.drmFormat = L.drm_format;
        p.modifier = obj.drm_format_modifier;
        return p.fd >= 0;
    };
    DmaBufNv12 d;
    d.width = w;
    d.height = h;
    const bool ok = plane(0, d.y) && plane(1, d.uv);
    closeAll();
    if (!ok) {
        closeDmaBuf(d);
        return false;
    }
    // The export can race the decoder / VPP still writing: wait for it.
    vaSyncSurface(dpy, surface);
    out = d;
    return true;
}

void closeDmaBuf(DmaBufNv12& d)
{
    if (d.y.fd >= 0) ::close(d.y.fd);
    if (d.uv.fd >= 0) ::close(d.uv.fd);
    d.y.fd = d.uv.fd = -1;
}

} // namespace p2p::video
