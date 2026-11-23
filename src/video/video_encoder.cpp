#include "video/video_encoder.h"

#ifndef __APPLE__
#include "video/vaapi_device.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#include <QLoggingCategory>

#include <climits>
#include <cstring>

Q_LOGGING_CATEGORY(lcEnc, "p2p.encoder")

namespace p2p::video {

namespace {
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

    ~Impl()
    {
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

QString VideoEncoder::name() const
{
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

bool VideoEncoder::encode(const VideoFrame& f, bool forceKeyframe, const Output& out)
{
    if (!d->ctx) return false;
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
        out(d->pkt->data, size_t(d->pkt->size), d->pkt->flags & AV_PKT_FLAG_KEY);
        av_packet_unref(d->pkt);
    }
    return e == AVERROR(EAGAIN) || e == AVERROR_EOF;
}

} // namespace p2p::video
