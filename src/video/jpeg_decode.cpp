#include "video/jpeg_decode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libswscale/swscale.h>
}

#include <va/va.h>
#include <va/va_vpp.h>

#include <algorithm>
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
        // Full-range BT.601 (any subsampling) -> limited BT.709 NV12.
        sws_ = sws_getCachedContext(sws_, frame_->width, frame_->height, AVPixelFormat(frame_->format), f.width,
                                    f.height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;
        sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_ITU601), 1, sws_getCoefficients(SWS_CS_ITU709), 0,
                                 0, 1 << 16, 1 << 16);
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

// Full-range BT.601 YCbCr -> limited BT.709 YCbCr, 16.16 fixed point. Luma
// picks up a little chroma (BT.601 and BT.709 weight R/G/B differently);
// chroma only mixes Cb/Cr.
struct Csc601To709 {
    int ky, kyu, kyv, kuu, kuv, kvu, kvv;
    Csc601To709()
    {
        const double toRgb[3][3] = {{1, 0, 1.402}, {1, -0.344136, -0.714136}, {1, 1.772, 0}};
        const double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
        const double toYuv[3][3] = {{kr, kg, kb},
                                    {-kr / 1.8556, -kg / 1.8556, (1 - kb) / 1.8556},
                                    {(1 - kr) / 1.5748, -kg / 1.5748, -kb / 1.5748}};
        double m[3][3] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) m[i][j] += toYuv[i][k] * toRgb[k][j];
        auto fx = [](double v) { return int(v * 65536.0 + (v < 0 ? -0.5 : 0.5)); };
        ky = fx(m[0][0] * 219 / 255);
        kyu = fx(m[0][1] * 219 / 255);
        kyv = fx(m[0][2] * 219 / 255);
        kuu = fx(m[1][1] * 224 / 255);
        kuv = fx(m[1][2] * 224 / 255);
        kvu = fx(m[2][1] * 224 / 255);
        kvv = fx(m[2][2] * 224 / 255);
    }
};

inline uint8_t clip8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// NV12 in the source's colours (as the VPP repack leaves it) -> |f|. Per
// chroma row: chroma first, with each pixel's luma offset from its chroma
// kept in |t|; then the two luma rows as a plain element-wise pass the
// compiler can vectorise.
void convertNv12(const Csc601To709& k, const AVFrame& src, VideoFrame& f, std::vector<int32_t>& t)
{
    const int w = f.width, h = f.height;
    t.resize(size_t(w));
    for (int r = 0; r < h; r += 2) {
        const uint8_t* sc = src.data[1] + size_t(r / 2) * size_t(src.linesize[1]);
        uint8_t* c = f.uv() + size_t(r / 2) * size_t(w);
        for (int x = 0; x < w; x += 2) {
            const int u = sc[x] - 128, v = sc[x + 1] - 128;
            t[size_t(x)] = t[size_t(x) + 1] = (16 << 16) + 32768 + k.kyu * u + k.kyv * v;
            c[x] = clip8(128 + ((k.kuu * u + k.kuv * v + 32768) >> 16));
            c[x + 1] = clip8(128 + ((k.kvu * u + k.kvv * v + 32768) >> 16));
        }
        for (int rr = r; rr < r + 2; ++rr) {
            const uint8_t* sy = src.data[0] + size_t(rr) * size_t(src.linesize[0]);
            uint8_t* y = f.y() + size_t(rr) * size_t(w);
            const int32_t* tt = t.data();
            const int ky = k.ky;
            for (int x = 0; x < w; ++x) {
                const int v = (ky * sy[x] + tt[x]) >> 16;
                y[x] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    }
}

AVPixelFormat pickVaapi(AVCodecContext*, const AVPixelFormat* f)
{
    for (; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == AV_PIX_FMT_VAAPI) return *f;
    return AV_PIX_FMT_NONE; // no hardware path: fail rather than decode on the CPU silently
}

class VaapiJpegDecoder : public JpegDecoder {
public:
    ~VaapiJpegDecoder() override
    {
        if (dpy_) {
            if (vppCtx_ != VA_INVALID_ID) vaDestroyContext(dpy_, vppCtx_);
            if (vppCfg_ != VA_INVALID_ID) vaDestroyConfig(dpy_, vppCfg_);
        }
        av_frame_free(&decoded_);
        av_frame_free(&nv12Hw_);
        av_frame_free(&nv12_);
        av_buffer_unref(&outFrames_);
        avcodec_free_context(&ctx_);
        av_buffer_unref(&device_);
    }
    QString name() const override { return QStringLiteral("VA-API"); }

    bool init(QString* why)
    {
        auto fail = [&](const QString& s) {
            if (why) *why = s;
            return false;
        };
        if (av_hwdevice_ctx_create(&device_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
            return fail(QStringLiteral("no VA-API device"));
        dpy_ = static_cast<AVVAAPIDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device_->data)->hwctx)->display;

        int n = vaMaxNumEntrypoints(dpy_);
        std::vector<VAEntrypoint> eps(size_t(std::max(n, 1)));
        if (vaQueryConfigEntrypoints(dpy_, VAProfileJPEGBaseline, eps.data(), &n) != VA_STATUS_SUCCESS ||
            std::find(eps.begin(), eps.begin() + n, VAEntrypointVLD) == eps.begin() + n)
            return fail(QStringLiteral("GPU has no JPEG decode"));
        if (vaCreateConfig(dpy_, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &vppCfg_) != VA_STATUS_SUCCESS)
            return fail(QStringLiteral("GPU has no video processing"));

        const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        ctx_ = c ? avcodec_alloc_context3(c) : nullptr;
        if (!ctx_) return fail(QStringLiteral("no MJPEG decoder"));
        ctx_->hw_device_ctx = av_buffer_ref(device_);
        ctx_->get_format = pickVaapi;
        if (avcodec_open2(ctx_, c, nullptr) < 0) return fail(QStringLiteral("MJPEG decoder open failed"));
        decoded_ = av_frame_alloc();
        nv12Hw_ = av_frame_alloc();
        nv12_ = av_frame_alloc();
        return decoded_ && nv12Hw_ && nv12_;
    }

    bool decode(const uint8_t* jpeg, size_t len, VideoFrame& f) override
    {
        AVPacket* pkt = wrapPacket(jpeg, len);
        if (!pkt) return false;
        int rc = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        av_frame_unref(decoded_);
        if (rc < 0 || avcodec_receive_frame(ctx_, decoded_) < 0 || decoded_->format != AV_PIX_FMT_VAAPI) return false;
        if (!ensureOutput(f.width, f.height)) return false;

        // Repack (4:2:2 YUYV -> NV12) and scale on the GPU.
        av_frame_unref(nv12Hw_);
        if (av_hwframe_get_buffer(outFrames_, nv12Hw_, 0) < 0) return false;
        VAProcPipelineParameterBuffer pp{};
        pp.surface = VASurfaceID(uintptr_t(decoded_->data[3]));
        VABufferID buf = VA_INVALID_ID;
        const VASurfaceID dst = VASurfaceID(uintptr_t(nv12Hw_->data[3]));
        if (vaCreateBuffer(dpy_, vppCtx_, VAProcPipelineParameterBufferType, sizeof(pp), 1, &pp, &buf) !=
            VA_STATUS_SUCCESS)
            return false;
        bool ok = vaBeginPicture(dpy_, vppCtx_, dst) == VA_STATUS_SUCCESS &&
                  vaRenderPicture(dpy_, vppCtx_, &buf, 1) == VA_STATUS_SUCCESS &&
                  vaEndPicture(dpy_, vppCtx_) == VA_STATUS_SUCCESS && vaSyncSurface(dpy_, dst) == VA_STATUS_SUCCESS;
        vaDestroyBuffer(dpy_, buf);
        if (!ok) return false;

        av_frame_unref(nv12_);
        nv12_->format = AV_PIX_FMT_NV12;
        if (av_hwframe_transfer_data(nv12_, nv12Hw_, 0) < 0) return false;
        convertNv12(csc_, *nv12_, f, lumaOffset_);
        return true;
    }

private:
    bool ensureOutput(int w, int h)
    {
        if (outFrames_ && w == outW_ && h == outH_) return true;
        av_buffer_unref(&outFrames_);
        if (vppCtx_ != VA_INVALID_ID) vaDestroyContext(dpy_, vppCtx_);
        vppCtx_ = VA_INVALID_ID;
        outFrames_ = av_hwframe_ctx_alloc(device_);
        if (!outFrames_) return false;
        auto* fc = reinterpret_cast<AVHWFramesContext*>(outFrames_->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = w;
        fc->height = h;
        fc->initial_pool_size = 2;
        if (av_hwframe_ctx_init(outFrames_) < 0 ||
            vaCreateContext(dpy_, vppCfg_, w, h, VA_PROGRESSIVE, nullptr, 0, &vppCtx_) != VA_STATUS_SUCCESS) {
            av_buffer_unref(&outFrames_);
            vppCtx_ = VA_INVALID_ID;
            return false;
        }
        outW_ = w;
        outH_ = h;
        return true;
    }

    AVBufferRef* device_ = nullptr;
    VADisplay dpy_ = nullptr;
    VAConfigID vppCfg_ = VA_INVALID_ID;
    VAContextID vppCtx_ = VA_INVALID_ID;
    AVBufferRef* outFrames_ = nullptr;
    int outW_ = 0, outH_ = 0;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* decoded_ = nullptr;
    AVFrame* nv12Hw_ = nullptr;
    AVFrame* nv12_ = nullptr;
    Csc601To709 csc_;
    std::vector<int32_t> lumaOffset_;
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
