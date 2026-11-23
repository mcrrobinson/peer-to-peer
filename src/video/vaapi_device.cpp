#include "video/vaapi_device.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

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

} // namespace p2p::video
