#include "video/video_decoder.h"

#ifdef __APPLE__
#include "video/cv_image.h"
#else
#include "video/vaapi_device.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#ifndef __APPLE__
#include <libavutil/hwcontext_vaapi.h>
#endif
#include <libswscale/swscale.h>
}

#include <QLoggingCategory>

#include <atomic>
#include <cstring>
#include <mutex>

Q_LOGGING_CATEGORY(lcDec, "p2p.decoder")

namespace p2p::video {

namespace {
std::atomic<bool> gGpuFrames{true};

#ifndef __APPLE__
// A hardware-decoded frame kept on the GPU.
class DecodedImage : public GpuImage {
public:
    explicit DecodedImage(AVFrame* f) : frame_(f) {}
    ~DecodedImage() override
    {
        closeDmaBuf(dmaBuf_);
        av_frame_free(&frame_);
    }
    void* nv12Surface(int w, int h) override
    {
        return w == frame_->width && h == frame_->height ? frame_ : nullptr;
    }
    bool download(VideoFrame& f) override
    {
        AVFrame* sw = av_frame_alloc();
        if (!sw) return false;
        sw->format = AV_PIX_FMT_NV12;
        const bool ok = av_hwframe_transfer_data(sw, frame_, 0) >= 0;
        if (ok) {
            for (int r = 0; r < f.height; ++r)
                std::memcpy(f.y() + size_t(r) * size_t(f.width), sw->data[0] + r * sw->linesize[0], size_t(f.width));
            for (int r = 0; r < f.height / 2; ++r)
                std::memcpy(f.uv() + size_t(r) * size_t(f.width), sw->data[1] + r * sw->linesize[1], size_t(f.width));
        }
        av_frame_free(&sw);
        return ok;
    }
    const DmaBufNv12* dmaBuf() override
    {
        std::lock_guard l(m_);
        if (!exported_) {
            exported_ = true;
            have_ = exportVaapiNv12(frame_, w_, h_, dmaBuf_);
        }
        return have_ ? &dmaBuf_ : nullptr;
    }
    void setSize(int w, int h)
    {
        w_ = w;
        h_ = h;
    }

private:
    AVFrame* frame_;
    int w_ = 0, h_ = 0;
    std::mutex m_;
    DmaBufNv12 dmaBuf_;
    bool exported_ = false, have_ = false;
};
#endif

#ifdef __APPLE__
// A VideoToolbox-decoded frame: an NV12 CVPixelBuffer (IOSurface), shown
// by the Metal renderer without a copy.
class DecodedImage : public GpuImage {
public:
    explicit DecodedImage(AVFrame* f) : frame_(f) {}
    ~DecodedImage() override { av_frame_free(&frame_); }
    void* nv12Surface(int, int) override { return nullptr; }
    void* cvPixelBuffer() override { return frame_->data[3]; }
    bool download(VideoFrame& f) override { return copyCvPixelBuffer(frame_->data[3], f); }

private:
    AVFrame* frame_;
};
constexpr AVPixelFormat kHwFormat = AV_PIX_FMT_VIDEOTOOLBOX;
constexpr AVHWDeviceType kHwDevice = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
constexpr AVPixelFormat kHwFormat = AV_PIX_FMT_VAAPI;
constexpr AVHWDeviceType kHwDevice = AV_HWDEVICE_TYPE_VAAPI;
#endif

AVPixelFormat pickHw(AVCodecContext*, const AVPixelFormat* fmts)
{
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == kHwFormat) return *p;
    return fmts[0]; // hwaccel unusable for this stream: software fallback
}
} // namespace

struct VideoDecoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* device = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* swFrame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    bool hw = false;

    ~Impl()
    {
        if (sws) sws_freeContext(sws);
        av_packet_free(&pkt);
        av_frame_free(&swFrame);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        av_buffer_unref(&device);
    }
};

VideoDecoder::VideoDecoder() : d(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::hardware() const { return d->hw; }
void VideoDecoder::setGpuFrames(bool on) { gGpuFrames = on; }
QString VideoDecoder::name() const
{
#ifdef __APPLE__
    const auto hwName = QStringLiteral(" (videotoolbox)");
#else
    const auto hwName = QStringLiteral(" (vaapi)");
#endif
    return d->codec ? QString::fromLatin1(d->codec->name) + (d->hw ? hwName : QString()) : QStringLiteral("none");
}

bool VideoDecoder::init(media::Codec codec, bool preferHardware, QString* error)
{
    if (codec == media::Codec::HEVC)
        d->codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    else // native av1 decoder only works with hwaccel; dav1d for software
        d->codec = avcodec_find_decoder_by_name(preferHardware ? "av1" : "libdav1d");
    if (!d->codec) {
        if (error) *error = QStringLiteral("no decoder for %1").arg(media::codecName(codec));
        return false;
    }
    AVCodecContext* c = d->ctx = avcodec_alloc_context3(d->codec);
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (preferHardware && av_hwdevice_ctx_create(&d->device, kHwDevice, nullptr, nullptr, 0) == 0) {
        c->hw_device_ctx = av_buffer_ref(d->device);
        c->get_format = pickHw;
        // Frames held downstream (latest in the hub, the one on screen, the
        // one before it) must not starve the decoder's surface pool.
        c->extra_hw_frames = 4;
        d->hw = true;
    }
    if (!d->hw) {
        c->thread_count = 4;
        c->thread_type = FF_THREAD_SLICE;
        if (QLatin1String(d->codec->name) == QLatin1String("libdav1d"))
            av_opt_set_int(c->priv_data, "max_frame_delay", 1, 0);
    }
    if (avcodec_open2(c, d->codec, nullptr) < 0) {
        if (error) *error = QStringLiteral("cannot open %1 decoder").arg(QString::fromLatin1(d->codec->name));
        return false;
    }
    d->frame = av_frame_alloc();
    d->swFrame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    qCInfo(lcDec).noquote() << "decoder" << name();
    return true;
}

std::shared_ptr<VideoFrame> VideoDecoder::decode(const uint8_t* data, size_t size, bool* error, int displayW,
                                                 int displayH)
{
    *error = false;
    if (av_new_packet(d->pkt, int(size)) < 0) return nullptr;
    std::memcpy(d->pkt->data, data, size);
    int e = avcodec_send_packet(d->ctx, d->pkt);
    av_packet_unref(d->pkt);
    if (e < 0 && e != AVERROR(EAGAIN)) {
        *error = true;
        return nullptr;
    }

    std::shared_ptr<VideoFrame> out;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        AVFrame* src = d->frame;
        // Display size comes from the decoded frame; a VAAPI surface is
        // padded (e.g. 1088 lines) and the bitstream doesn't say by how much.
        int w = d->frame->width, h = d->frame->height;
        if (displayW > 0 && displayW < w) w = displayW;
        if (displayH > 0 && displayH < h) h = displayH;
        const bool full = d->frame->color_range == AVCOL_RANGE_JPEG;
        const Colour colour =
            full && (d->frame->colorspace == AVCOL_SPC_SMPTE170M || d->frame->colorspace == AVCOL_SPC_BT470BG)
                ? Colour::Bt601Full
                : Colour::Bt709Limited;
#ifndef __APPLE__
        if (d->frame->format == AV_PIX_FMT_VAAPI && gGpuFrames) {
            // Keep it on the GPU; wait for the decode so the stage timing
            // (and anyone mapping it) sees a finished picture.
            auto* fc = reinterpret_cast<AVHWFramesContext*>(d->frame->hw_frames_ctx->data);
            vaSyncSurface(static_cast<AVVAAPIDeviceContext*>(fc->device_ctx->hwctx)->display,
                          VASurfaceID(uintptr_t(d->frame->data[3])));
            auto f = std::make_shared<VideoFrame>();
            f->width = w & ~1;
            f->height = h & ~1;
            f->colour = colour;
            auto img = std::make_shared<DecodedImage>(av_frame_clone(d->frame));
            img->setSize(f->width, f->height);
            f->gpu = std::move(img);
            out = std::move(f);
            av_frame_unref(d->frame);
            continue;
        }
#else
        if (d->frame->format == AV_PIX_FMT_VIDEOTOOLBOX && gGpuFrames) {
            // VideoToolbox decodes synchronously in FFmpeg: the picture is done.
            auto f = std::make_shared<VideoFrame>();
            f->width = w & ~1;
            f->height = h & ~1;
            f->colour = colour;
            f->gpu = std::make_shared<DecodedImage>(av_frame_clone(d->frame));
            out = std::move(f);
            av_frame_unref(d->frame);
            continue;
        }
#endif
        if (d->frame->format == kHwFormat) {
            av_frame_unref(d->swFrame);
            d->swFrame->format = AV_PIX_FMT_NV12;
            if (av_hwframe_transfer_data(d->swFrame, d->frame, 0) < 0) {
                *error = true;
                av_frame_unref(d->frame);
                return nullptr;
            }
            src = d->swFrame;
        }
        auto f = std::make_shared<VideoFrame>();
        f->allocate(w & ~1, h & ~1);
        f->colour = colour; // camera MJPEG is sent in its own colours (full-range BT.601)
        if (src->format == AV_PIX_FMT_NV12) {
            for (int r = 0; r < f->height; ++r)
                std::memcpy(f->y() + size_t(r) * size_t(f->width), src->data[0] + r * src->linesize[0], size_t(f->width));
            for (int r = 0; r < f->height / 2; ++r)
                std::memcpy(f->uv() + size_t(r) * size_t(f->width), src->data[1] + r * src->linesize[1], size_t(f->width));
        } else {
            d->sws = sws_getCachedContext(d->sws, f->width, f->height, AVPixelFormat(src->format), f->width,
                                          f->height, AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr, nullptr);
            if (!d->sws) {
                *error = true;
                return nullptr;
            }
            // Repack only: the renderer converts from the stream's colours.
            const int* k = sws_getCoefficients(full ? SWS_CS_ITU601 : SWS_CS_ITU709);
            sws_setColorspaceDetails(d->sws, k, full ? 1 : 0, k, full ? 1 : 0, 0, 1 << 16, 1 << 16);
            uint8_t* dst[2] = {f->y(), f->uv()};
            const int stride[2] = {f->width, f->width};
            sws_scale(d->sws, src->data, src->linesize, 0, f->height, dst, stride);
        }
        out = std::move(f); // keep only the newest
        av_frame_unref(d->frame);
    }
    return out;
}

} // namespace p2p::video
