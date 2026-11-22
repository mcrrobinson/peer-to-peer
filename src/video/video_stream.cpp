#include "video/video_stream.h"

#include "core/clock.h"
#include "net/media_transport.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cstring>
#include <vector>

Q_LOGGING_CATEGORY(lcVideo, "p2p.video")

namespace p2p::video {

namespace {
constexpr int64_t kPliIntervalUs = 100'000;
constexpr size_t kLatencyWindow = 240;
constexpr int64_t kLossReportIntervalUs = 200'000;
constexpr int64_t kHistoryUs = 1'000'000;
constexpr size_t kHistoryMaxBytes = 32u << 20;
constexpr size_t kNackEntry = 6;

void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void putU32(uint8_t* p, uint32_t v) { putU16(p, uint16_t(v >> 16)); putU16(p + 2, uint16_t(v)); }
uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t getU32(const uint8_t* p) { return uint32_t(getU16(p)) << 16 | getU16(p + 2); }

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

double autoFecRate(double loss, bool keyframe)
{
    if (loss < 0.002) return 0.0;
    const double base = std::min(0.5, 2.0 * loss + 0.03);
    return keyframe ? std::min(0.6, base * 1.5) : base;
}

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
    transport_.setFeedbackHandler(streamId_, [this](const uint8_t* d, size_t n, int64_t) { onFeedback(d, n); });
}

void VideoSendStream::onFeedback(const uint8_t* d, size_t n)
{
    if (n < 1) return;
    switch (VideoFeedback(d[0])) {
    case VideoFeedback::PictureLoss: {
        forceKeyframe_ = true;
        std::lock_guard l(statsMutex_);
        ++st_.pliReceived;
        return;
    }
    case VideoFeedback::LossReport: {
        if (n < 3) return;
        const double loss = std::min(1.0, getU16(d + 1) / 10000.0);
        // Fast attack, slow decay.
        const double prev = lossEstimate_.load();
        lossEstimate_ = loss > prev ? loss : prev * 0.8 + loss * 0.2;
        std::lock_guard l(statsMutex_);
        st_.reportedLoss = loss;
        return;
    }
    case VideoFeedback::Nack: {
        uint64_t items = 0, sent = 0, missed = 0;
        {
            std::lock_guard l(historyMutex_);
            for (size_t off = 1; off + kNackEntry <= n; off += kNackEntry) {
                const uint32_t id = getU32(d + off);
                const uint16_t index = getU16(d + off + 4);
                ++items;
                auto it = std::find_if(history_.begin(), history_.end(), [&](const SentFrame& f) { return f.id == id; });
                if (it == history_.end()) {
                    ++missed;
                    continue;
                }
                if (index == NackItem::kWholeFrame) {
                    for (const auto& frag : it->fragments) resend(frag);
                    sent += it->fragments.size();
                } else if (index < it->fragments.size()) {
                    resend(it->fragments[index]);
                    ++sent;
                } else {
                    ++missed;
                }
            }
        }
        std::lock_guard l(statsMutex_);
        st_.nackItems += items;
        st_.retransmitted += sent;
        st_.retransmitMisses += missed;
        return;
    }
    }
}

void VideoSendStream::resend(const std::vector<uint8_t>& payload)
{
    uint8_t buf[2048];
    std::memcpy(buf, payload.data(), payload.size());
    buf[16] |= FragmentHeader::kRetransmit; // flags byte
    transport_.send(net::wire::kMedia, streamId_, buf, payload.size());
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
    size_t bytes = 0, parity = 0;
    bool wasKey = false;
    double fecRate = 0;
    const uint8_t codecId = uint8_t(sel_.codec);
    const bool ok = encoder_->encode(*f, key, [&](const uint8_t* data, size_t size, bool isKey) {
        wasKey |= isKey;
        bytes += size;
        PacketizeOptions opt;
        opt.extraFlags = sendingPattern_ ? FragmentHeader::kTestPattern : 0;
        opt.width = uint16_t(f->width);
        opt.height = uint16_t(f->height);
        switch (cfg_.fec) {
        case VideoStreamConfig::Fec::Off: break;
        case VideoStreamConfig::Fec::Auto: opt.fecRate = autoFecRate(lossEstimate_.load(), isKey); break;
        case VideoStreamConfig::Fec::Fixed: opt.fecRate = cfg_.fecFixedRate; break;
        }
        fecRate = opt.fecRate;

        SentFrame sent;
        sent.id = frameId_++;
        sent.sentUs = t0;
        packetizeFrame(
            sent.id, f->captureUs, isKey, codecId, data, size, net::wire::kMaxPayload,
            [&](const uint8_t* p, size_t n) {
                transport_.send(net::wire::kMedia, streamId_, p, n);
                auto h = FragmentHeader::read(p, n);
                if (h && h->isParity()) {
                    ++parity;
                } else if (cfg_.nack) {
                    sent.fragments.emplace_back(p, p + n);
                    sent.bytes += n;
                }
            },
            opt);
        if (cfg_.nack) {
            std::lock_guard l(historyMutex_);
            historyBytes_ += sent.bytes;
            history_.push_back(std::move(sent));
            while (!history_.empty() &&
                   (t0 - history_.front().sentUs > kHistoryUs || historyBytes_ > kHistoryMaxBytes)) {
                historyBytes_ -= history_.front().bytes;
                history_.pop_front();
            }
        }
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
    st_.parityFragments += parity;
    st_.fecRate = fecRate;
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
                                       QString hubName, bool nack)
    : sel_(sel), transport_(t), streamId_(streamId), hubName_(std::move(hubName)), nack_(nack)
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

void VideoReceiveStream::sendNacks(const std::vector<NackItem>& items)
{
    uint8_t buf[net::wire::kMaxPayload];
    const size_t perPacket = (sizeof(buf) - 1) / kNackEntry;
    for (size_t i = 0; i < items.size(); i += perPacket) {
        buf[0] = uint8_t(VideoFeedback::Nack);
        size_t off = 1;
        for (size_t j = i; j < std::min(items.size(), i + perPacket); ++j, off += kNackEntry) {
            putU32(buf + off, items[j].frameId);
            putU16(buf + off + 4, items[j].index);
        }
        transport_.send(net::wire::kFeedback, streamId_, buf, off);
    }
}

void VideoReceiveStream::maybeReportLoss(int64_t now)
{
    if (now - lastReportUs_ < kLossReportIntervalUs) return;
    lastReportUs_ = now;
    const auto t = transport_.stats();
    const uint64_t exp = t.packetsExpected - reportExpected_, rec = t.packetsReceived - reportReceived_;
    reportExpected_ = t.packetsExpected;
    reportReceived_ = t.packetsReceived;
    if (exp == 0) return;
    const double loss = rec >= exp ? 0.0 : double(exp - rec) / double(exp);
    uint8_t msg[3] = {uint8_t(VideoFeedback::LossReport)};
    putU16(msg + 1, uint16_t(std::min(10000.0, loss * 10000.0 + 0.5)));
    transport_.send(net::wire::kFeedback, streamId_, msg, sizeof(msg));
    std::lock_guard l(statsMutex_);
    st_.pathLoss = loss;
}

void VideoReceiveStream::decodeLoop()
{
    while (running_.load(std::memory_order_relaxed)) {
        std::vector<AssembledFrame> frames;
        std::vector<NackItem> nacks;
        bool needKey = false;
        bool synced = false;
        int64_t grace = 0;
        const int64_t rtt = std::max<int64_t>(rttUs_.load(), 0);
        FrameAssembler::NackPolicy policy;
        policy.retryUs = std::max<int64_t>(5'000, rtt * 3 / 2 + 2'000);
        {
            std::unique_lock l(m_);
            // Short timeout: the NACK and grace timers run off this loop.
            cv_.wait_for(l, std::chrono::milliseconds(2), [&] { return wake_ || !running_; });
            wake_ = false;
            const int64_t now = nowUs();
            if (nack_) {
                // Long enough for every retry to come back, but bounded.
                assembler_.setReorderGraceUs(std::clamp<int64_t>(
                    policy.reorderUs + policy.maxTries * policy.retryUs + rtt / 2, 20'000, 300'000));
                nacks = assembler_.collectNacks(now, policy);
            }
            frames = assembler_.poll(now);
            needKey = assembler_.needKeyframe();
            synced = assembler_.stats().framesReleased > 0;
            grace = assembler_.reorderGraceUs();
            if (firstFragmentUs_ == 0 && assembler_.stats().fragments > 0) firstFragmentUs_ = now;
            std::lock_guard sl(statsMutex_);
            st_.assembler = assembler_.stats();
        }
        if (!nacks.empty()) sendNacks(nacks);
        maybeReportLoss(nowUs());
        // The sender opens with a keyframe, so before the first decode give
        // FEC / NACK the same grace a later loss gets; after that the
        // assembler has already waited when it reports a loss.
        if (needKey && firstFragmentUs_ != 0 && (synced || nowUs() - firstFragmentUs_ >= grace))
            requestKeyframe(nowUs());

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
            if (f.recovered) ++st_.recoveredDecoded;
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
