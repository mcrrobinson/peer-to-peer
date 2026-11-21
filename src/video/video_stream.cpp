#include "video/video_stream.h"

#include "core/clock.h"
#include "net/media_transport.h"

#include <QLoggingCategory>

#include <algorithm>
#include <vector>

Q_LOGGING_CATEGORY(lcVideo, "p2p.video")

namespace p2p::video {

namespace {
constexpr int64_t kPliIntervalUs = 100'000;
constexpr size_t kLatencyWindow = 240;

// Updates |kbps| / |fps| over >= 1 s windows from running totals.
void updateRate(int64_t now, uint64_t bytes, uint64_t frames, uint64_t& lastBytes, uint64_t& lastFrames,
                int64_t& lastAt, double& kbps, double& fps)
{
    if (lastAt == 0) {
        lastAt = now;
        lastBytes = bytes;
        lastFrames = frames;
        return;
    }
    const int64_t dt = now - lastAt;
    if (dt < 1'000'000) return;
    kbps = double(bytes - lastBytes) * 8.0 / (double(dt) / 1e6) / 1000.0;
    fps = double(frames - lastFrames) / (double(dt) / 1e6);
    lastAt = now;
    lastBytes = bytes;
    lastFrames = frames;
}
} // namespace

// ============================================================ send

VideoSendStream::VideoSendStream(VideoStreamConfig cfg, media::StreamSelection sel, net::MediaTransport& t,
                                 uint8_t streamId, QString previewName)
    : cfg_(std::move(cfg)), sel_(sel), transport_(t), streamId_(streamId), previewName_(std::move(previewName))
{
}

VideoSendStream::~VideoSendStream()
{
    stop();
}

void VideoSendStream::attach()
{
    transport_.setFeedbackHandler(streamId_, [this](const uint8_t* d, size_t n, int64_t) {
        if (n >= 1 && d[0] == uint8_t(VideoFeedback::PictureLoss)) {
            forceKeyframe_ = true;
            std::lock_guard l(statsMutex_);
            ++st_.pliReceived;
        }
    });
}

bool VideoSendStream::start(QString* error)
{
    const auto& c = cfg_.capture;
    if (cfg_.source == VideoStreamConfig::Source::Camera) {
        source_ = createCameraSource(c);
        QString err;
        if (!source_->start([this](std::shared_ptr<VideoFrame> f) { onFrame(std::move(f)); }, &err)) {
            if (!cfg_.fallbackToPattern) {
                if (error) *error = err;
                return false;
            }
            qCWarning(lcVideo).noquote() << "camera unavailable (" << err << "), sending test pattern instead";
            cfg_.source = VideoStreamConfig::Source::Pattern;
        }
    }
    if (cfg_.source == VideoStreamConfig::Source::Pattern) {
        source_ = std::make_unique<TestPatternSource>(c.width, c.height, c.fps);
        if (!source_->start([this](std::shared_ptr<VideoFrame> f) { onFrame(std::move(f)); }, error)) return false;
    }
    sendingPattern_ = cfg_.source == VideoStreamConfig::Source::Pattern;
    if (source_) {
        std::lock_guard l(statsMutex_);
        st_.source = source_->description();
    }
    return true;
}

void VideoSendStream::stop()
{
    if (source_) source_->stop();
    source_.reset();
    FrameHub::instance().clear(previewName_);
}

void VideoSendStream::onFrame(std::shared_ptr<VideoFrame> f)
{
    FrameHub::instance().publish(previewName_, f);
    {
        std::lock_guard l(statsMutex_);
        ++st_.captured;
    }
    if (encoderFailed_) return;

    if (!encoder_) {
        VideoEncoder::Config ec;
        ec.codec = sel_.codec;
        ec.hardware = sel_.encoderHw;
        ec.width = f->width;
        ec.height = f->height;
        ec.fps = source_ ? std::max(1, source_->fps()) : 60;
        ec.bitrateKbps = cfg_.bitrateKbps;
        encoder_ = std::make_unique<VideoEncoder>();
        QString err;
        if (!encoder_->init(ec, &err)) {
            qCWarning(lcVideo).noquote() << "video encoder failed:" << err;
            encoder_.reset();
            encoderFailed_ = true;
            return;
        }
        std::lock_guard l(statsMutex_);
        st_.encoder = encoder_->name();
        st_.width = f->width;
        st_.height = f->height;
    }

    const bool key = forceKeyframe_.exchange(false);
    const int64_t t0 = nowUs();
    size_t bytes = 0;
    bool wasKey = false;
    const uint8_t codecId = uint8_t(sel_.codec);
    const bool ok = encoder_->encode(*f, key, [&](const uint8_t* data, size_t size, bool isKey) {
        wasKey |= isKey;
        bytes += size;
        packetizeFrame(frameId_++, f->captureUs, isKey, codecId, data, size, net::wire::kMaxPayload,
                       [&](const uint8_t* p, size_t n) { transport_.send(net::wire::kMedia, streamId_, p, n); },
                       sendingPattern_ ? FragmentHeader::kTestPattern : 0, uint16_t(f->width),
                       uint16_t(f->height));
    });
    const double ms = double(nowUs() - t0) / 1000.0;

    std::lock_guard l(statsMutex_);
    if (!ok) {
        ++st_.encodeErrors;
        return;
    }
    if (bytes == 0) return;
    ++st_.encoded;
    if (wasKey) ++st_.keyframes;
    st_.bytes += bytes;
    encodeSumMs_ += ms;
    st_.encodeAvgMs = encodeSumMs_ / double(st_.encoded);
    st_.encodeMaxMs = std::max(st_.encodeMaxMs, ms);
}

VideoSendStream::Stats VideoSendStream::stats() const
{
    std::lock_guard l(statsMutex_);
    Stats s = st_;
    updateRate(nowUs(), st_.bytes, st_.encoded, rateBytes_, rateFrames_, rateAtUs_, kbps_, fps_);
    s.kbps = kbps_;
    s.fps = fps_;
    return s;
}

// ============================================================ receive

VideoReceiveStream::VideoReceiveStream(media::StreamSelection sel, net::MediaTransport& t, uint8_t streamId,
                                       QString hubName)
    : sel_(sel), transport_(t), streamId_(streamId), hubName_(std::move(hubName))
{
}

VideoReceiveStream::~VideoReceiveStream()
{
    stop();
}

void VideoReceiveStream::attach()
{
    transport_.setHandler(streamId_, [this](const uint8_t* d, size_t n, int64_t t) { onFragment(d, n, t); });
}

bool VideoReceiveStream::start(QString* error)
{
    if (!decoder_.init(sel_.codec, sel_.decoderHw, error)) return false;
    {
        std::lock_guard l(statsMutex_);
        st_.decoder = decoder_.name();
    }
    running_ = true;
    thread_ = std::thread([this] { decodeLoop(); });
    return true;
}

void VideoReceiveStream::stop()
{
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    FrameHub::instance().clear(hubName_);
}

void VideoReceiveStream::onFragment(const uint8_t* data, size_t len, int64_t arrivalUs)
{
    auto h = FragmentHeader::read(data, len);
    if (!h) return;
    bytesIn_.fetch_add(len, std::memory_order_relaxed);
    {
        std::lock_guard l(m_);
        assembler_.insert(*h, data + FragmentHeader::kSize, len - FragmentHeader::kSize, arrivalUs);
        wake_ = true;
    }
    cv_.notify_one();
}

void VideoReceiveStream::requestKeyframe(int64_t nowUs)
{
    if (nowUs - lastPliUs_ < kPliIntervalUs) return;
    lastPliUs_ = nowUs;
    const uint8_t msg[1] = {uint8_t(VideoFeedback::PictureLoss)};
    transport_.send(net::wire::kFeedback, streamId_, msg, sizeof(msg));
    std::lock_guard l(statsMutex_);
    ++st_.pliSent;
}

void VideoReceiveStream::decodeLoop()
{
    while (running_.load(std::memory_order_relaxed)) {
        std::vector<AssembledFrame> frames;
        bool needKey = false;
        bool started = false;
        {
            std::unique_lock l(m_);
            cv_.wait_for(l, std::chrono::milliseconds(10), [&] { return wake_ || !running_; });
            wake_ = false;
            frames = assembler_.poll(nowUs());
            needKey = assembler_.needKeyframe();
            started = assembler_.stats().fragments > 0;
            std::lock_guard sl(statsMutex_);
            st_.assembler = assembler_.stats();
        }
        // Only ask for a keyframe once the stream has actually started
        // (the sender opens with one) or after a loss.
        if (needKey && started) requestKeyframe(nowUs());

        for (auto& f : frames) {
            const int64_t t0 = nowUs();
            bool err = false;
            auto out = decoder_.decode(f.data.data(), f.data.size(), &err, f.width, f.height);
            const int64_t t1 = nowUs();
            if (err) {
                {
                    std::lock_guard sl(statsMutex_);
                    ++st_.decodeErrors;
                }
                requestKeyframe(t1);
                continue;
            }
            if (!out) continue;
            out->id = f.id;
            out->captureUs = f.captureUs - clockOffsetUs_.load(); // into our clock
            const double latency = double(t1 - out->captureUs) / 1000.0;

            std::optional<uint32_t> code;
            if (f.flags & FragmentHeader::kTestPattern) code = TestPatternSource::readCode(*out);
            FrameHub::instance().publish(hubName_, out);

            std::lock_guard sl(statsMutex_);
            ++st_.decoded;
            decodeSumMs_ += double(t1 - t0) / 1000.0;
            st_.decodeAvgMs = decodeSumMs_ / double(st_.decoded);
            st_.width = out->width;
            st_.height = out->height;
            latencies_.push_back(latency);
            while (latencies_.size() > kLatencyWindow) latencies_.pop_front();
            if (code) {
                if (!havePattern_ || *code > lastPattern_) ++st_.patternOk;
                else ++st_.patternBad;
                havePattern_ = true;
                lastPattern_ = *code;
            }
        }
    }
}

VideoReceiveStream::Stats VideoReceiveStream::stats() const
{
    std::lock_guard l(statsMutex_);
    Stats s = st_;
    updateRate(nowUs(), bytesIn_.load(), st_.decoded, rateBytes_, rateFrames_, rateAtUs_, kbps_, fps_);
    s.kbps = kbps_;
    s.fps = fps_;
    if (!latencies_.empty()) {
        std::vector<double> v(latencies_.begin(), latencies_.end());
        double sum = 0;
        for (double x : v) sum += x;
        s.latencyAvgMs = sum / double(v.size());
        const size_t k = std::min(v.size() - 1, v.size() * 95 / 100);
        std::nth_element(v.begin(), v.begin() + long(k), v.end());
        s.latencyP95Ms = v[k];
    }
    return s;
}

} // namespace p2p::video
