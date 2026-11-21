// V4L2 camera capture (Linux).
#include "core/clock.h"
#include "video/video_source.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <QLoggingCategory>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

Q_LOGGING_CATEGORY(lcCam, "p2p.camera")

namespace p2p::video {

namespace {

int xioctl(int fd, unsigned long req, void* arg)
{
    int r;
    do r = ::ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

QString fourccName(uint32_t f)
{
    char s[5] = {char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff), char((f >> 24) & 0xff), 0};
    return QString::fromLatin1(s);
}

class V4l2Camera : public VideoSource {
public:
    explicit V4l2Camera(CaptureConfig cfg) : cfg_(std::move(cfg)) {}
    ~V4l2Camera() override
    {
        stop();
        closeDevice();
    }

    bool start(Callback cb, QString* error) override
    {
        if (!openDevice(error)) {
            closeDevice();
            return false;
        }
        cb_ = std::move(cb);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
    }

    QString description() const override
    {
        return QStringLiteral("%1 \"%2\" %3x%4 %5 @ %6 fps")
            .arg(cfg_.device, name_)
            .arg(width_)
            .arg(height_)
            .arg(fourccName(pixfmt_))
            .arg(fps_);
    }
    int fps() const override { return fps_; }

private:
    bool supportsFps(uint32_t fmt, int w, int h, int fps)
    {
        v4l2_frmivalenum iv{};
        iv.pixel_format = fmt;
        iv.width = uint32_t(w);
        iv.height = uint32_t(h);
        for (iv.index = 0; xioctl(fd_, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == 0; ++iv.index) {
            if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE && iv.discrete.numerator > 0 &&
                int(iv.discrete.denominator / iv.discrete.numerator) >= fps)
                return true;
        }
        return false;
    }

    bool openDevice(QString* error)
    {
        auto fail = [&](const QString& what) {
            if (error) *error = QStringLiteral("%1: %2 (%3)").arg(cfg_.device, what, QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        };
        fd_ = ::open(cfg_.device.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) return fail(QStringLiteral("cannot open"));

        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) return fail(QStringLiteral("not a V4L2 device"));
        const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING))
            return fail(QStringLiteral("no streaming video capture"));
        name_ = QString::fromUtf8(reinterpret_cast<const char*>(cap.card));

        uint32_t want = V4L2_PIX_FMT_MJPEG;
        if (cfg_.format == CaptureConfig::Format::Nv12 ||
            (cfg_.format == CaptureConfig::Format::Auto &&
             supportsFps(V4L2_PIX_FMT_NV12, cfg_.width, cfg_.height, cfg_.fps)))
            want = V4L2_PIX_FMT_NV12;

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = uint32_t(cfg_.width);
        fmt.fmt.pix.height = uint32_t(cfg_.height);
        fmt.fmt.pix.pixelformat = want;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) return fail(QStringLiteral("VIDIOC_S_FMT"));
        pixfmt_ = fmt.fmt.pix.pixelformat;
        width_ = int(fmt.fmt.pix.width) & ~1;
        height_ = int(fmt.fmt.pix.height) & ~1;
        stride_ = int(fmt.fmt.pix.bytesperline);
        if (pixfmt_ != V4L2_PIX_FMT_NV12 && pixfmt_ != V4L2_PIX_FMT_MJPEG) {
            errno = EINVAL;
            return fail(QStringLiteral("camera offered unsupported format %1").arg(fourccName(pixfmt_)));
        }

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = uint32_t(cfg_.fps);
        xioctl(fd_, VIDIOC_S_PARM, &parm);
        fps_ = parm.parm.capture.timeperframe.numerator
                   ? int(parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator)
                   : cfg_.fps;

        // Don't let auto-exposure lower the frame rate in dim light.
        v4l2_control ctrl{V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0};
        xioctl(fd_, VIDIOC_S_CTRL, &ctrl);

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) return fail(QStringLiteral("VIDIOC_REQBUFS"));
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) return fail(QStringLiteral("VIDIOC_QUERYBUF"));
            void* p = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
            if (p == MAP_FAILED) return fail(QStringLiteral("mmap"));
            buffers_.push_back({p, b.length});
            if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) return fail(QStringLiteral("VIDIOC_QBUF"));
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return fail(QStringLiteral("VIDIOC_STREAMON"));

        if (pixfmt_ == V4L2_PIX_FMT_MJPEG) {
            const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            jpeg_ = c ? avcodec_alloc_context3(c) : nullptr;
            if (!jpeg_ || avcodec_open2(jpeg_, c, nullptr) < 0) {
                errno = EINVAL;
                return fail(QStringLiteral("MJPEG decoder unavailable"));
            }
            jpegFrame_ = av_frame_alloc();
        }
        qCInfo(lcCam).noquote() << "camera:" << description();
        return true;
    }

    void closeDevice()
    {
        for (auto& b : buffers_) ::munmap(b.ptr, b.len);
        buffers_.clear();
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        if (sws_) sws_freeContext(sws_);
        sws_ = nullptr;
        av_frame_free(&jpegFrame_);
        avcodec_free_context(&jpeg_);
    }

    void loop()
    {
        pollfd pfd{fd_, POLLIN, 0};
        while (running_.load(std::memory_order_relaxed)) {
            if (::poll(&pfd, 1, 200) <= 0) continue;
            // Drain: keep only the newest buffer, hand older ones straight back.
            v4l2_buffer last{};
            bool have = false;
            for (;;) {
                v4l2_buffer b{};
                b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                b.memory = V4L2_MEMORY_MMAP;
                if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) break;
                if (have) {
                    ++dropped_;
                    xioctl(fd_, VIDIOC_QBUF, &last);
                }
                last = b;
                have = true;
            }
            if (!have) continue;

            auto frame = std::make_shared<VideoFrame>();
            frame->allocate(width_, height_);
            frame->id = seq_++;
            const bool monotonic =
                (last.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
            frame->captureUs = monotonic ? int64_t(last.timestamp.tv_sec) * 1'000'000 + last.timestamp.tv_usec
                                         : nowUs();
            const int64_t t0 = nowUs();
            const bool ok = convert(static_cast<const uint8_t*>(buffers_[last.index].ptr), last.bytesused, *frame);
            const int64_t t1 = nowUs();
            xioctl(fd_, VIDIOC_QBUF, &last);
            // Where camera latency goes: driver timestamp -> dequeue, then conversion.
            timingDeliver_ += double(t0 - frame->captureUs) / 1000.0;
            timingConvert_ += double(t1 - t0) / 1000.0;
            if (++timingN_ == 300) {
                qCInfo(lcCam, "camera timing over %d frames: timestamp->dequeue %.1f ms, convert %.1f ms, dropped %llu",
                       timingN_, timingDeliver_ / timingN_, timingConvert_ / timingN_,
                       (unsigned long long)dropped_);
                timingN_ = 0;
                timingDeliver_ = timingConvert_ = 0;
            }
            if (ok) cb_(std::move(frame));
        }
    }

    bool convert(const uint8_t* src, size_t len, VideoFrame& f)
    {
        if (pixfmt_ == V4L2_PIX_FMT_NV12) {
            const int stride = stride_ > 0 ? stride_ : width_;
            if (len < size_t(stride) * size_t(height_) * 3 / 2) return false;
            for (int r = 0; r < height_; ++r)
                std::memcpy(f.y() + size_t(r) * size_t(width_), src + size_t(r) * size_t(stride), size_t(width_));
            const uint8_t* uvSrc = src + size_t(stride) * size_t(height_);
            for (int r = 0; r < height_ / 2; ++r)
                std::memcpy(f.uv() + size_t(r) * size_t(width_), uvSrc + size_t(r) * size_t(stride), size_t(width_));
            return true;
        }

        // MJPEG: decode, then convert full-range BT.601 YUV 4:2:2 to limited BT.709 NV12.
        AVPacket* pkt = av_packet_alloc();
        pkt->data = const_cast<uint8_t*>(src);
        pkt->size = int(len);
        int rc = avcodec_send_packet(jpeg_, pkt);
        av_packet_free(&pkt);
        if (rc < 0 || avcodec_receive_frame(jpeg_, jpegFrame_) < 0) return false;

        sws_ = sws_getCachedContext(sws_, jpegFrame_->width, jpegFrame_->height, AVPixelFormat(jpegFrame_->format),
                                    width_, height_, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;
        const int* src601 = sws_getCoefficients(SWS_CS_ITU601);
        const int* dst709 = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(sws_, src601, 1 /*full*/, dst709, 0 /*limited*/, 0, 1 << 16, 1 << 16);
        uint8_t* dst[2] = {f.y(), f.uv()};
        const int dstStride[2] = {width_, width_};
        sws_scale(sws_, jpegFrame_->data, jpegFrame_->linesize, 0, jpegFrame_->height, dst, dstStride);
        return true;
    }

    struct Mapped {
        void* ptr;
        size_t len;
    };

    CaptureConfig cfg_;
    int fd_ = -1;
    QString name_;
    uint32_t pixfmt_ = 0;
    int width_ = 0, height_ = 0, stride_ = 0, fps_ = 0;
    std::vector<Mapped> buffers_;
    AVCodecContext* jpeg_ = nullptr;
    AVFrame* jpegFrame_ = nullptr;
    int timingN_ = 0;
    double timingDeliver_ = 0, timingConvert_ = 0;
    SwsContext* sws_ = nullptr;
    Callback cb_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint32_t seq_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace

std::unique_ptr<VideoSource> createCameraSource(const CaptureConfig& cfg)
{
    return std::make_unique<V4l2Camera>(cfg);
}

} // namespace p2p::video
