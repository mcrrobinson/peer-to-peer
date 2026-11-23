#include "video/video_encoder.h"

#ifdef __APPLE__
#include "video/hevc_vt_encoder.h"
#else
#include "video/hevc_vaapi_encoder.h"
#include "video/vaapi_device.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#ifndef __APPLE__
#include <libavutil/hwcontext_vaapi.h>
#endif
#include <libavutil/opt.h>
}

#include <QLoggingCategory>

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>

Q_LOGGING_CATEGORY(lcEnc, "p2p.encoder")

namespace p2p::video {

namespace {
std::atomic<bool> gNativeHevc{true};

QString averr(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

const char* encoderName(media::Codec c, bool hw)
{
#ifdef __APPLE__
    if (hw) return c == media::Codec::HEVC ? "hevc_videotoolbox" : nullptr;
#else
    if (hw) return c == media::Codec::HEVC ? "hevc_vaapi" : "av1_vaapi";
#endif
    return c == media::Codec::HEVC ? "libx265" : "libsvtav1";
}
} // namespace

struct VideoEncoder::Impl {
    Config cfg;
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* device = nullptr;
    AVBufferRef* frames = nullptr;
    AVFrame* sw = nullptr;
    AVFrame* hw = nullptr;
    AVPacket* pkt = nullptr;
    bool lastKey = false; // FFmpeg path: the packet being handed out is a keyframe
    uint32_t nextId = 0;  // frame numbering when the caller gives none
#ifdef __APPLE__
    std::unique_ptr<HevcVtEncoder> vt;
#else
    std::unique_ptr<HevcVaapiEncoder> native;
#endif

    ~Impl()
    {
#ifndef __APPLE__
        native.reset(); // before the device it uses
#endif
        av_packet_free(&pkt);
        av_frame_free(&hw);
        av_frame_free(&sw);
        avcodec_free_context(&ctx);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
    }
};

VideoEncoder::VideoEncoder() : d(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::hardware() const { return d->cfg.hardware; }

void VideoEncoder::setNativeHevc(bool on) { gNativeHevc = on; }

bool VideoEncoder::liveBitrate() const
{
#ifdef __APPLE__
    return d->vt != nullptr;
#else
    return d->native != nullptr;
#endif
}

int VideoEncoder::refBack() const
{
#ifdef __APPLE__
    if (d->vt) return d->vt->lastRefBack();
#else
    if (d->native) return d->native->lastRefBack();
#endif
    return d->lastKey ? 0 : 1;
}

bool VideoEncoder::recoverFrom(uint32_t goodFrameId)
{
#ifdef __APPLE__
    if (d->vt) return d->vt->recoverFrom(goodFrameId);
#else
    if (d->native) return d->native->recoverFrom(goodFrameId);
#endif
    (void)goodFrameId;
    return false;
}

void VideoEncoder::setAnchorInterval(int frames)
{
#ifndef __APPLE__
    if (d->native) d->native->setAnchorInterval(frames);
#endif
    (void)frames;
}

void VideoEncoder::setBitrateKbps(int kbps)
{
#ifdef __APPLE__
    if (d->vt) d->vt->setBitrateKbps(kbps);
#else
    if (d->native) d->native->setBitrateKbps(kbps);
#endif
    (void)kbps;
}

QString VideoEncoder::name() const
{
#ifdef __APPLE__
    if (d->vt) return QStringLiteral("hevc_videotoolbox_native");
#else
    if (d->native) return QStringLiteral("hevc_vaapi_native");
#endif
    return d->codec ? QString::fromLatin1(d->codec->name) : QStringLiteral("none");
}

bool VideoEncoder::init(const Config& cfg, QString* error)
{
    auto fail = [&](const QString& what, int e = 0) {
        if (error) *error = e ? QStringLiteral("%1: %2").arg(what, averr(e)) : what;
        return false;
    };
    d->cfg = cfg;
    const char* name = encoderName(cfg.codec, cfg.hardware);
    d->codec = name ? avcodec_find_encoder_by_name(name) : nullptr;
    if (!d->codec) return fail(QStringLiteral("encoder %1 not available").arg(QString::fromLatin1(name ? name : "?")));

    AVCodecContext* c = d->ctx = avcodec_alloc_context3(d->codec);
    c->width = cfg.width;
    c->height = cfg.height;
    c->time_base = {1, 1'000'000}; // pts in microseconds
    c->framerate = {cfg.fps, 1};
    c->gop_size = INT_MAX / 2;     // keyframes only on demand
    c->max_b_frames = 0;
    c->bit_rate = int64_t(cfg.bitrateKbps) * 1000;
    c->rc_max_rate = c->bit_rate;
    c->rc_buffer_size = int(c->bit_rate / cfg.fps); // ~1 frame VBV
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (cfg.colour == Colour::Bt601Full) { // camera MJPEG, passed through unconverted
        c->color_primaries = AVCOL_PRI_SMPTE170M;
        c->color_trc = AVCOL_TRC_SMPTE170M;
        c->colorspace = AVCOL_SPC_SMPTE170M;
        c->color_range = AVCOL_RANGE_JPEG;
    } else {
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc = AVCOL_TRC_BT709;
        c->colorspace = AVCOL_SPC_BT709;
        c->color_range = AVCOL_RANGE_MPEG;
    }

    if (cfg.hardware) {
#ifdef __APPLE__
        if (cfg.codec == media::Codec::HEVC && gNativeHevc) {
            HevcVtEncoder::Config nc;
            nc.width = cfg.width;
            nc.height = cfg.height;
            nc.fps = cfg.fps;
            nc.bitrateKbps = cfg.bitrateKbps;
            nc.colour = cfg.colour;
            auto vt = std::make_unique<HevcVtEncoder>();
            QString why;
            if (vt->init(nc, &why)) {
                d->vt = std::move(vt);
                avcodec_free_context(&d->ctx);
                qCInfo(lcEnc).noquote() << QStringLiteral("encoder hevc_videotoolbox_native %1x%2 @%3 fps %4 kbps, low-latency RC, LTR%5")
                                               .arg(cfg.width)
                                               .arg(cfg.height)
                                               .arg(cfg.fps)
                                               .arg(cfg.bitrateKbps)
                                               .arg(cfg.colour == Colour::Bt601Full ? QStringLiteral(", BT.601 full range") : QString());
                return true;
            }
            qCWarning(lcEnc).noquote() << "native HEVC encoder unavailable, using FFmpeg's:" << why;
        }
        c->pix_fmt = AV_PIX_FMT_NV12;
#else
        // The shared device: camera surfaces come in without a copy.
        d->device = sharedVaapiDevice();
        if (!d->device) return fail(QStringLiteral("VA-API device"));
        int e = 0;
        d->frames = av_hwframe_ctx_alloc(d->device);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(d->frames->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = cfg.width;
        fc->height = cfg.height;
        fc->initial_pool_size = 4;
        if ((e = av_hwframe_ctx_init(d->frames)) < 0) return fail(QStringLiteral("VA-API frames"), e);
        if (cfg.codec == media::Codec::HEVC && gNativeHevc) {
            auto* dev = reinterpret_cast<AVHWDeviceContext*>(d->device->data);
            HevcVaapiEncoder::Config nc;
            nc.width = cfg.width;
            nc.height = cfg.height;
            nc.fps = cfg.fps;
            nc.bitrateKbps = cfg.bitrateKbps;
            nc.colour = cfg.colour;
            auto native = std::make_unique<HevcVaapiEncoder>();
            QString why;
            if (native->init(static_cast<AVVAAPIDeviceContext*>(dev->hwctx)->display, nc, &why)) {
                d->native = std::move(native);
                avcodec_free_context(&d->ctx);
                d->sw = av_frame_alloc();
                d->sw->width = cfg.width;
                d->sw->height = cfg.height;
                d->sw->format = AV_PIX_FMT_NV12;
                d->hw = av_frame_alloc();
                qCInfo(lcEnc).noquote() << QStringLiteral("encoder hevc_vaapi_native %1x%2 @%3 fps %4 kbps CBR, VBV 1 frame%5")
                                               .arg(cfg.width)
                                               .arg(cfg.height)
                                               .arg(cfg.fps)
                                               .arg(cfg.bitrateKbps)
                                               .arg(cfg.colour == Colour::Bt601Full ? QStringLiteral(", BT.601 full range") : QString());
                return true;
            }
            qCWarning(lcEnc).noquote() << "native HEVC encoder unavailable, using FFmpeg's:" << why;
        }
        c->pix_fmt = AV_PIX_FMT_VAAPI;
        c->hw_frames_ctx = av_buffer_ref(d->frames);
        av_opt_set(c->priv_data, "rc_mode", "CBR", 0);
        av_opt_set_int(c->priv_data, "async_depth", 1, 0); // no pipelining: one in, one out
        av_opt_set_int(c->priv_data, "aud", 0, 0);
#endif
    } else if (cfg.codec == media::Codec::HEVC) {
        c->pix_fmt = AV_PIX_FMT_YUV420P;
        c->thread_count = 0;
        av_opt_set(c->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
        av_opt_set_int(c->priv_data, "forced-idr", 1, 0);
        av_opt_set(c->priv_data, "x265-params", "repeat-headers=1:log-level=warning", 0);
    } else {
        c->pix_fmt = AV_PIX_FMT_YUV420P;
        av_opt_set_int(c->priv_data, "preset", 10, 0);
        // Low-delay prediction structure is required for CBR in SVT-AV1.
        av_opt_set(c->priv_data, "svtav1-params", "pred-struct=1:lookahead=0:scm=2", 0);
    }

    int e = avcodec_open2(c, d->codec, nullptr);
    if (e < 0) return fail(QStringLiteral("open %1").arg(QString::fromLatin1(name)), e);

    d->sw = av_frame_alloc();
    d->sw->width = cfg.width;
    d->sw->height = cfg.height;
    d->sw->format = cfg.hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    if (!cfg.hardware && (e = av_frame_get_buffer(d->sw, 0)) < 0) return fail(QStringLiteral("frame alloc"), e);
    d->hw = av_frame_alloc();
    d->pkt = av_packet_alloc();
    qCInfo(lcEnc).noquote() << QStringLiteral("encoder %1 %2x%3 @%4 fps %5 kbps CBR, VBV %6 kbit%7")
                                   .arg(QString::fromLatin1(name))
                                   .arg(cfg.width)
                                   .arg(cfg.height)
                                   .arg(cfg.fps)
                                   .arg(cfg.bitrateKbps)
                                   .arg(c->rc_buffer_size / 1000)
                                   .arg(cfg.colour == Colour::Bt601Full ? QStringLiteral(", BT.601 full range") : QString());
    return true;
}

bool VideoEncoder::encode(const VideoFrame& f, bool forceKeyframe, const Output& out, std::optional<uint32_t> frameId)
{
    const uint32_t id = frameId.value_or(d->nextId);
    d->nextId = id + 1;
#ifdef __APPLE__
    if (d->vt) return d->vt->encode(f, id, forceKeyframe, out);
    if (!d->ctx) return false;
#else
    if (!d->ctx && !d->native) return false;
#endif
    AVFrame* input = nullptr;
    int e = 0;
#ifndef __APPLE__
    const bool zeroCopy = d->cfg.hardware && f.gpu;
#else
    const bool zeroCopy = false;
#endif
    if (!zeroCopy && (f.width != d->cfg.width || f.height != d->cfg.height || !f.ensureCpu())) return false;

    if (zeroCopy) {
        auto* s = static_cast<AVFrame*>(f.gpu->nv12Surface(d->cfg.width, d->cfg.height));
        av_frame_unref(d->hw);
        if (!s || (e = av_frame_ref(d->hw, s)) < 0) {
            qCWarning(lcEnc) << "no GPU surface for the frame";
            return false;
        }
        input = d->hw;
    } else if (d->cfg.hardware) {
        // Wrap the NV12 buffer (no copy) and upload it to a GPU surface.
        d->sw->data[0] = const_cast<uint8_t*>(f.y());
        d->sw->data[1] = const_cast<uint8_t*>(f.uv());
        d->sw->linesize[0] = d->sw->linesize[1] = f.width;
#ifdef __APPLE__
        input = d->sw;
#else
        av_frame_unref(d->hw);
        if ((e = av_hwframe_get_buffer(d->frames, d->hw, 0)) < 0 ||
            (e = av_hwframe_transfer_data(d->hw, d->sw, 0)) < 0) {
            qCWarning(lcEnc) << "upload failed:" << averr(e);
            return false;
        }
        input = d->hw;
#endif
    } else {
        // NV12 -> I420 for the software encoders.
        if ((e = av_frame_make_writable(d->sw)) < 0) return false;
        for (int r = 0; r < f.height; ++r)
            std::memcpy(d->sw->data[0] + r * d->sw->linesize[0], f.y() + size_t(r) * size_t(f.width), size_t(f.width));
        for (int r = 0; r < f.height / 2; ++r) {
            const uint8_t* uv = f.uv() + size_t(r) * size_t(f.width);
            uint8_t* u = d->sw->data[1] + r * d->sw->linesize[1];
            uint8_t* v = d->sw->data[2] + r * d->sw->linesize[2];
            for (int c = 0; c < f.width / 2; ++c) {
                u[c] = uv[2 * c];
                v[c] = uv[2 * c + 1];
            }
        }
        input = d->sw;
    }

#ifndef __APPLE__
    if (d->native) {
        if (!zeroCopy) d->sw->data[0] = d->sw->data[1] = nullptr; // the upload has the planes
        const auto surface = unsigned(reinterpret_cast<uintptr_t>(input->data[3]));
        return d->native->encode(surface, id, forceKeyframe, out);
    }
#endif
    input->pts = f.captureUs;
    input->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    e = avcodec_send_frame(d->ctx, input);
    if (d->cfg.hardware && !zeroCopy) {
        // The wrapper frame doesn't own its planes.
        d->sw->data[0] = d->sw->data[1] = nullptr;
    }
    if (e < 0) {
        qCWarning(lcEnc) << "send_frame failed:" << averr(e);
        return false;
    }
    while ((e = avcodec_receive_packet(d->ctx, d->pkt)) == 0) {
        d->lastKey = d->pkt->flags & AV_PKT_FLAG_KEY;
        out(d->pkt->data, size_t(d->pkt->size), d->lastKey);
        av_packet_unref(d->pkt);
    }
    return e == AVERROR(EAGAIN) || e == AVERROR_EOF;
}

} // namespace p2p::video
