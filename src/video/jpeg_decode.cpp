#include "video/jpeg_decode.h"

#include "video/vaapi_device.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libswscale/swscale.h>
}

#include <va/va.h>
#include <va/va_vpp.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace p2p::video {

namespace {

AVPacket* wrapPacket(const uint8_t* data, size_t len)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return nullptr;
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = int(len);
    return pkt;
}

class SoftwareJpegDecoder : public JpegDecoder {
public:
    SoftwareJpegDecoder()
    {
        const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        ctx_ = c ? avcodec_alloc_context3(c) : nullptr;
        if (ctx_ && avcodec_open2(ctx_, c, nullptr) < 0) avcodec_free_context(&ctx_);
        frame_ = av_frame_alloc();
    }
    ~SoftwareJpegDecoder() override
    {
        if (sws_) sws_freeContext(sws_);
        av_frame_free(&frame_);
        avcodec_free_context(&ctx_);
    }
    bool ok() const { return ctx_ && frame_; }
    QString name() const override { return QStringLiteral("software"); }

    bool decode(const uint8_t* jpeg, size_t len, VideoFrame& f) override
    {
        AVPacket* pkt = wrapPacket(jpeg, len);
        if (!pkt) return false;
        int rc = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        if (rc < 0 || avcodec_receive_frame(ctx_, frame_) < 0) return false;
        f.allocate(f.width, f.height);
        f.colour = Colour::Bt601Full;
        // Repack only (any subsampling -> NV12), keeping the JPEG's colours.
        sws_ = sws_getCachedContext(sws_, frame_->width, frame_->height, AVPixelFormat(frame_->format), f.width,
                                    f.height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;
        const int* bt601 = sws_getCoefficients(SWS_CS_ITU601);
        sws_setColorspaceDetails(sws_, bt601, 1, bt601, 1, 0, 1 << 16, 1 << 16);
        uint8_t* dst[2] = {f.y(), f.uv()};
        const int dstStride[2] = {f.width, f.width};
        sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, dstStride);
        return true;
    }

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ = nullptr;
};

// VA video processing on the shared device: any decoded surface -> NV12 at
// a given size (repack + scale; colours untouched). Shared by the decoder
// and the GPU images it hands out, which may outlive it.
class VaProcessor {
public:
    ~VaProcessor()
    {
        for (auto& [size, t] : targets_) {
            vaDestroyContext(dpy_, t.ctx);
            av_buffer_unref(&t.pool);
        }
        if (cfg_ != VA_INVALID_ID) vaDestroyConfig(dpy_, cfg_);
        av_buffer_unref(&device_);
    }

    bool init(AVBufferRef* device, QString* why)
    {
        device_ = device;
        dpy_ = static_cast<AVVAAPIDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device_->data)->hwctx)->display;
        if (vaCreateConfig(dpy_, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &cfg_) != VA_STATUS_SUCCESS) {
            cfg_ = VA_INVALID_ID;
            if (why) *why = QStringLiteral("GPU has no video processing");
            return false;
        }
        if (!disableEncodeFormatConversion()) {
            if (why) *why = QStringLiteral("couldn't set up video processing");
            return false;
        }
        return true;
    }
    VADisplay display() const { return dpy_; }

    // A new NV12 frame (caller frees) holding |src|'s top-left srcW x srcH
    // scaled to w x h, finished on the GPU; nullptr on failure.
    AVFrame* toNv12(const AVFrame* src, int srcW, int srcH, int w, int h)
    {
        std::lock_guard l(m_);
        Target* t = target(w, h);
        if (!t) return nullptr;
        AVFrame* out = av_frame_alloc();
        if (!out || av_hwframe_get_buffer(t->pool, out, 0) < 0) {
            av_frame_free(&out);
            return nullptr;
        }
        if (!process(t->ctx, src, srcW, srcH, out, w, h)) av_frame_free(&out);
        if (out) {
            out->width = w;
            out->height = h;
        }
        return out;
    }

private:
    // Mesa's "EFC" shortcut: an unscaled, unfiltered repack whose source
    // format the encoder claims to take is not done; the destination just
    // records the source and the encoder reads that instead
    // (postproc.c, picture.c). radeonsi claims packed 4:2:2 (our JPEG
    // surfaces) and then encodes garbage. A second repack while one is still
    // pending switches EFC off for the whole display (efc_count = -1), so do
    // two on throwaway surfaces before any real one. Harmless elsewhere.
    bool disableEncodeFormatConversion()
    {
        VASurfaceAttrib fourcc{};
        fourcc.type = VASurfaceAttribPixelFormat;
        fourcc.flags = VA_SURFACE_ATTRIB_SETTABLE;
        fourcc.value.type = VAGenericValueTypeInteger;
        fourcc.value.value.i = VA_FOURCC_YUY2;
        VASurfaceID src = VA_INVALID_SURFACE, dst[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
        VAContextID ctx = VA_INVALID_ID;
        bool ok = vaCreateSurfaces(dpy_, VA_RT_FORMAT_YUV422, 64, 64, &src, 1, &fourcc, 1) == VA_STATUS_SUCCESS &&
                  vaCreateSurfaces(dpy_, VA_RT_FORMAT_YUV420, 64, 64, dst, 2, nullptr, 0) == VA_STATUS_SUCCESS &&
                  vaCreateContext(dpy_, cfg_, 64, 64, VA_PROGRESSIVE, dst, 2, &ctx) == VA_STATUS_SUCCESS;
        for (int i = 0; ok && i < 2; ++i) {
            VAProcPipelineParameterBuffer pp{};
            pp.surface = src;
            VABufferID buf = VA_INVALID_ID;
            ok = vaCreateBuffer(dpy_, ctx, VAProcPipelineParameterBufferType, sizeof(pp), 1, &pp, &buf) ==
                     VA_STATUS_SUCCESS &&
                 vaBeginPicture(dpy_, ctx, dst[i]) == VA_STATUS_SUCCESS &&
                 vaRenderPicture(dpy_, ctx, &buf, 1) == VA_STATUS_SUCCESS &&
                 vaEndPicture(dpy_, ctx) == VA_STATUS_SUCCESS;
            if (buf != VA_INVALID_ID) vaDestroyBuffer(dpy_, buf);
        }
        if (ok) vaSyncSurface(dpy_, dst[1]);
        if (ctx != VA_INVALID_ID) vaDestroyContext(dpy_, ctx);
        if (dst[0] != VA_INVALID_SURFACE) vaDestroySurfaces(dpy_, dst, 2);
        if (src != VA_INVALID_SURFACE) vaDestroySurfaces(dpy_, &src, 1);
        return ok;
    }

    bool process(VAContextID ctx, const AVFrame* src, int srcW, int srcH, AVFrame* out, int w, int h)
    {
        VARectangle outRegion{0, 0, uint16_t(w), uint16_t(h)};
        const VASurfaceID dst = VASurfaceID(uintptr_t(out->data[3]));
        VARectangle region{0, 0, uint16_t(srcW), uint16_t(srcH)};
        VAProcPipelineParameterBuffer pp{};
        pp.surface = VASurfaceID(uintptr_t(src->data[3]));
        pp.surface_region = &region;
        pp.output_region = &outRegion;
        VABufferID buf = VA_INVALID_ID;
        bool ok = vaCreateBuffer(dpy_, ctx, VAProcPipelineParameterBufferType, sizeof(pp), 1, &pp, &buf) ==
                  VA_STATUS_SUCCESS;
        ok = ok && vaBeginPicture(dpy_, ctx, dst) == VA_STATUS_SUCCESS &&
             vaRenderPicture(dpy_, ctx, &buf, 1) == VA_STATUS_SUCCESS &&
             vaEndPicture(dpy_, ctx) == VA_STATUS_SUCCESS && vaSyncSurface(dpy_, dst) == VA_STATUS_SUCCESS;
        if (buf != VA_INVALID_ID) vaDestroyBuffer(dpy_, buf);
        return ok;
    }

    struct Target {
        VAContextID ctx = VA_INVALID_ID;
        AVBufferRef* pool = nullptr;
    };
    Target* target(int w, int h)
    {
        auto it = targets_.find({w, h});
        if (it != targets_.end()) return &it->second;
        Target t;
        t.pool = av_hwframe_ctx_alloc(device_);
        if (!t.pool) return nullptr;
        auto* fc = reinterpret_cast<AVHWFramesContext*>(t.pool->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = w;
        fc->height = h;
        fc->initial_pool_size = 0; // grows with what downstream holds (preview, encoder)
        if (av_hwframe_ctx_init(t.pool) < 0 ||
            vaCreateContext(dpy_, cfg_, w, h, VA_PROGRESSIVE, nullptr, 0, &t.ctx) != VA_STATUS_SUCCESS) {
            av_buffer_unref(&t.pool);
            return nullptr;
        }
        if (targets_.size() >= 8) { // rung sizes come and go; don't collect them forever
            for (auto& [size, old] : targets_) {
                vaDestroyContext(dpy_, old.ctx);
                av_buffer_unref(&old.pool); // surfaces still out keep their pool alive
            }
            targets_.clear();
        }
        return &targets_.emplace(std::pair{w, h}, t).first->second;
    }

    AVBufferRef* device_ = nullptr;
    VADisplay dpy_ = nullptr;
    VAConfigID cfg_ = VA_INVALID_ID;
    std::mutex m_;
    std::map<std::pair<int, int>, Target> targets_;
};

// A decoded camera JPEG on the GPU; NV12 versions made on demand per size.
class VaapiImage : public GpuImage {
public:
    VaapiImage(std::shared_ptr<VaProcessor> proc, AVFrame* decoded, int w, int h)
        : proc_(std::move(proc)), decoded_(decoded), w_(w), h_(h)
    {
    }
    ~VaapiImage() override
    {
        closeDmaBuf(dmaBuf_);
        for (auto& [size, f] : nv12_) av_frame_free(&f);
        av_frame_free(&decoded_);
    }

    void* nv12Surface(int w, int h) override
    {
        std::lock_guard l(m_);
        return nv12Locked(w, h);
    }

    const DmaBufNv12* dmaBuf() override
    {
        std::lock_guard l(m_);
        if (!exported_) {
            exported_ = true;
            AVFrame* f = nv12Locked(w_, h_);
            haveDmaBuf_ = f && exportVaapiNv12(f, w_, h_, dmaBuf_);
        }
        return haveDmaBuf_ ? &dmaBuf_ : nullptr;
    }

private:
    AVFrame* nv12Locked(int w, int h)
    {
        auto it = nv12_.find({w, h});
        if (it != nv12_.end()) return it->second;
        AVFrame* f = proc_->toNv12(decoded_, w_, h_, w, h);
        if (f) nv12_.emplace(std::pair{w, h}, f);
        return f;
    }

public:

    bool download(VideoFrame& f) override
    {
        auto* hw = static_cast<AVFrame*>(nv12Surface(f.width, f.height));
        if (!hw) return false;
        AVFrame* sw = av_frame_alloc();
        if (!sw) return false;
        sw->format = AV_PIX_FMT_NV12;
        bool ok = av_hwframe_transfer_data(sw, hw, 0) >= 0;
        if (ok) {
            for (int r = 0; r < f.height; ++r)
                std::memcpy(f.y() + size_t(r) * size_t(f.width), sw->data[0] + size_t(r) * size_t(sw->linesize[0]),
                            size_t(f.width));
            for (int r = 0; r < f.height / 2; ++r)
                std::memcpy(f.uv() + size_t(r) * size_t(f.width), sw->data[1] + size_t(r) * size_t(sw->linesize[1]),
                            size_t(f.width));
        }
        av_frame_free(&sw);
        return ok;
    }

private:
    std::shared_ptr<VaProcessor> proc_;
    AVFrame* decoded_;
    int w_, h_;
    std::mutex m_;
    std::map<std::pair<int, int>, AVFrame*> nv12_;
    DmaBufNv12 dmaBuf_;
    bool exported_ = false, haveDmaBuf_ = false;
};

AVPixelFormat pickVaapi(AVCodecContext*, const AVPixelFormat* f)
{
    for (; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == AV_PIX_FMT_VAAPI) return *f;
    return AV_PIX_FMT_NONE; // no hardware path: fail rather than decode on the CPU silently
}

class VaapiJpegDecoder : public JpegDecoder {
public:
    ~VaapiJpegDecoder() override { avcodec_free_context(&ctx_); }
    QString name() const override { return QStringLiteral("VA-API"); }

    bool init(QString* why)
    {
        auto fail = [&](const QString& s) {
            if (why) *why = s;
            return false;
        };
        AVBufferRef* device = sharedVaapiDevice();
        if (!device) return fail(QStringLiteral("no VA-API device"));
        proc_ = std::make_shared<VaProcessor>();
        if (!proc_->init(device, why)) return false;
        VADisplay dpy = proc_->display();

        int n = vaMaxNumEntrypoints(dpy);
        std::vector<VAEntrypoint> eps(size_t(std::max(n, 1)));
        if (vaQueryConfigEntrypoints(dpy, VAProfileJPEGBaseline, eps.data(), &n) != VA_STATUS_SUCCESS ||
            std::find(eps.begin(), eps.begin() + n, VAEntrypointVLD) == eps.begin() + n)
            return fail(QStringLiteral("GPU has no JPEG decode"));

        const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        ctx_ = c ? avcodec_alloc_context3(c) : nullptr;
        if (!ctx_) return fail(QStringLiteral("no MJPEG decoder"));
        ctx_->hw_device_ctx = sharedVaapiDevice();
        ctx_->get_format = pickVaapi;
        // Decoded surfaces stay referenced downstream (preview, encoder in flight).
        ctx_->extra_hw_frames = 8;
        if (avcodec_open2(ctx_, c, nullptr) < 0) return fail(QStringLiteral("MJPEG decoder open failed"));
        return true;
    }

    bool decode(const uint8_t* jpeg, size_t len, VideoFrame& f) override
    {
        AVPacket* pkt = wrapPacket(jpeg, len);
        if (!pkt) return false;
        int rc = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        AVFrame* decoded = av_frame_alloc();
        if (!decoded) return false;
        if (rc < 0 || avcodec_receive_frame(ctx_, decoded) < 0 || decoded->format != AV_PIX_FMT_VAAPI ||
            decoded->width < f.width || decoded->height < f.height) {
            av_frame_free(&decoded);
            return false;
        }
        f.colour = Colour::Bt601Full;
        f.gpu = std::make_shared<VaapiImage>(proc_, decoded, f.width, f.height);
        return true;
    }

private:
    std::shared_ptr<VaProcessor> proc_;
    AVCodecContext* ctx_ = nullptr;
};

} // namespace

std::unique_ptr<JpegDecoder> createSoftwareJpegDecoder()
{
    auto d = std::make_unique<SoftwareJpegDecoder>();
    return d->ok() ? std::move(d) : nullptr;
}

std::unique_ptr<JpegDecoder> createVaapiJpegDecoder(QString* why)
{
    auto d = std::make_unique<VaapiJpegDecoder>();
    if (!d->init(why)) return nullptr;
    return d;
}

} // namespace p2p::video
