#include "video/video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <QLoggingCategory>

#include <cstring>

Q_LOGGING_CATEGORY(lcDec, "p2p.decoder")

namespace p2p::video {

namespace {
AVPixelFormat pickVaapi(AVCodecContext*, const AVPixelFormat* fmts)
{
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_VAAPI) return *p;
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
QString VideoDecoder::name() const
{
    return d->codec ? QString::fromLatin1(d->codec->name) + (d->hw ? QStringLiteral(" (vaapi)") : QString())
                    : QStringLiteral("none");
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

#ifndef __APPLE__
    if (preferHardware && av_hwdevice_ctx_create(&d->device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) == 0) {
        c->hw_device_ctx = av_buffer_ref(d->device);
        c->get_format = pickVaapi;
        d->hw = true;
    }
#endif
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
        if (d->frame->format == AV_PIX_FMT_VAAPI) {
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
        // Display size comes from the decoded frame; a VAAPI read-back is
        // the whole (padded, e.g. 1088-line) surface.
        int w = d->frame->width, h = d->frame->height;
        if (displayW > 0 && displayW < w) w = displayW;
        if (displayH > 0 && displayH < h) h = displayH;
        f->allocate(w & ~1, h & ~1);
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
