#include "video/video_stream.h"

#include "core/clock.h"
#include "net/media_transport.h"

#include <QLoggingCategory>

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <vector>

Q_LOGGING_CATEGORY(lcVideo, "p2p.video")

namespace p2p::video {

namespace {
constexpr int64_t kPliIntervalUs = 100'000;
constexpr size_t kLatencyWindow = 240;
constexpr int64_t kPathLossIntervalUs = 200'000;
constexpr int64_t kHistoryUs = 1'000'000;
constexpr size_t kHistoryMaxBytes = 32u << 20;
constexpr size_t kNackEntry = 6;
constexpr int64_t kResendMaxAgeUs = 500'000; // receiver grace is <= 300 ms + RTT

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
    sws_freeContext(sws_);
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
        {
            // A screen may be idle (no new frames): ask for one to carry the keyframe.
            std::lock_guard sl(sourceMutex_);
            if (source_) source_->requestFrame();
        }
        std::lock_guard l(statsMutex_);
        ++st_.pliReceived;
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
                // History is pruned only when a frame is encoded, so while
                // frames are skipped check the age here too.
                if (it == history_.end() || nowUs() - it->sentUs > kResendMaxAgeUs) {
                    ++missed;
                    continue;
                }
                // Resends never push the pacer past the point where new
                // frames get skipped: fresh video has priority.
                const size_t bytes = index == NackItem::kWholeFrame ? it->bytes
                                     : index < it->fragments.size() ? it->fragments[index].size() : 0;
                if (bytes && !resendFits(bytes)) {
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

bool VideoSendStream::resendFits(size_t bytes) const
{
    const int64_t rate = transport_.ccStats().pacingBps;
    if (rate <= 0) return true; // not pacing: goes out right away
    return transport_.pacerQueueUs() + int64_t(double(bytes) * 8e6 / double(rate)) <= skipQueueUs_.load();
}

void VideoSendStream::resend(const std::vector<uint8_t>& payload)
{
    uint8_t buf[2048];
    std::memcpy(buf, payload.data(), payload.size());
    buf[16] |= FragmentHeader::kRetransmit; // flags byte
    transport_.sendPaced(net::wire::kMedia, streamId_, buf, payload.size());
}

bool VideoSendStream::start(QString* error)
{
    if (running_) return true;
    std::lock_guard sl(sourceMutex_);
    forceKeyframe_ = true;
    const auto& c = cfg_.capture;
    auto onFrame = [this](std::shared_ptr<VideoFrame> f) { this->onFrame(std::move(f)); };
    if (cfg_.source == VideoStreamConfig::Source::Screen) {
        source_ = createScreenSource(cfg_.screen, cfg_.onSourceEnded);
        if (!source_->start(onFrame, error)) {
            source_.reset();
            return false;
        }
    }
    if (cfg_.source == VideoStreamConfig::Source::Camera) {
        source_ = createCameraSource(c);
        QString err;
        if (!source_->start(onFrame, &err)) {
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
        if (!source_->start(onFrame, error)) return false;
    }
    sendingPattern_ = cfg_.source == VideoStreamConfig::Source::Pattern;
    if (source_) {
        std::lock_guard l(statsMutex_);
        st_.source = source_->description();
    }
    running_ = true;
    return true;
}

void VideoSendStream::stop()
{
    {
        std::lock_guard sl(sourceMutex_);
        if (source_) source_->stop();
        source_.reset();
    }
    running_ = false;
    FrameHub::instance().clear(previewName_);
}

bool VideoSendStream::openEncoder(const Rung& r, int fps)
{
    const int64_t t0 = nowUs();
    VideoEncoder::Config ec;
    ec.codec = sel_.codec;
    ec.hardware = sel_.encoderHw;
    ec.width = r.width;
    ec.height = r.height;
    ec.fps = fps;
    ec.bitrateKbps = r.kbps;
    auto enc = std::make_unique<VideoEncoder>();
    QString err;
    if (!enc->init(ec, &err)) {
        qCWarning(lcVideo).noquote() << "video encoder failed:" << err;
        return false;
    }
    encoder_ = std::move(enc);
    const double ms = double(nowUs() - t0) / 1000.0;
    std::lock_guard l(statsMutex_);
    st_.encoder = encoder_->name();
    st_.width = r.width;
    st_.height = r.height;
    st_.rungKbps = r.kbps;
    if (ladder_) st_.rungIndex = ladder_->index();
    st_.reopenMaxMs = std::max(st_.reopenMaxMs, ms);
    return true;
}

const VideoFrame* VideoSendStream::scaled(const VideoFrame& f, int w, int h)
{
    if (f.width == w && f.height == h) return &f;
    sws_ = sws_getCachedContext(sws_, f.width, f.height, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                nullptr, nullptr, nullptr);
    if (!sws_) return nullptr;
    if (scaledFrame_.width != w || scaledFrame_.height != h) scaledFrame_.allocate(w, h);
    const uint8_t* src[2] = {f.y(), f.uv()};
    const int srcStride[2] = {f.width, f.width};
    uint8_t* dst[2] = {scaledFrame_.y(), scaledFrame_.uv()};
    const int dstStride[2] = {w, w};
    sws_scale(sws_, src, srcStride, 0, f.height, dst, dstStride);
    scaledFrame_.captureUs = f.captureUs;
    scaledFrame_.id = f.id;
    return &scaledFrame_;
}

void VideoSendStream::onFrame(std::shared_ptr<VideoFrame> f)
{
    FrameHub::instance().publish(previewName_, f);
    {
        std::lock_guard l(statsMutex_);
        ++st_.captured;
    }
    if (encoderFailed_) return;

    const int fps = source_ ? std::max(1, source_->fps()) : 60;
    const bool cc = cfg_.congestionControl && transport_.congestionControlEnabled();
    const int64_t now = nowUs();
    lossEstimate_ = transport_.ccStats().lossRate;

    // (Re)build the ladder for this source size: first frame, or a shared
    // window / screen that changed size.
    if (!ladder_ || f->width != ladderSrcW_ || f->height != ladderSrcH_) {
        // Text-mode screen share keeps resolution and gives up frame rate:
        // size the rungs as if for ~10 fps.
        const bool text = cfg_.source == VideoStreamConfig::Source::Screen &&
                          cfg_.screen.mode == ScreenConfig::Mode::Text;
        auto rungs = cc ? buildLadder(cfg_.bitrateKbps, cfg_.minBitrateKbps, f->width, f->height, text ? 10 : fps)
                        : std::vector<Rung>{{cfg_.bitrateKbps, f->width, f->height}};
        ladder_ = std::make_unique<RungSelector>(std::move(rungs));
        encoder_.reset();
        ladderSrcW_ = f->width;
        ladderSrcH_ = f->height;
        std::lock_guard l(statsMutex_);
        st_.rungCount = int(ladder_->ladder().size());
        if (source_) st_.source = source_->description();
    }
    if (cc) {
        // What the controller leaves for this encoder: minus audio, minus FEC overhead.
        const double avail = double(transport_.targetBps() - transport_.streamBps(net::wire::kAudio));
        const double budget = std::max(0.0, avail) * share_.load() / (1.0 + lastFecRate_) / 1000.0;
        if (ladder_->update(budget, now)) {
            const auto& r = ladder_->rung();
            qCInfo(lcVideo).noquote() << QStringLiteral("rate ladder -> %1 kbps %2x%3 (budget %4 kbps)")
                                             .arg(r.kbps)
                                             .arg(r.width)
                                             .arg(r.height)
                                             .arg(budget, 0, 'f', 0);
            encoder_.reset();
        }
        {
            std::lock_guard l(statsMutex_);
            st_.budgetKbps = budget;
            st_.rungSwitches = ladder_->switches();
        }
        // Don't stack frames behind a backed-up pacer: skip this one.
        skipQueueUs_ = 1'500'000 / fps;
        if (transport_.pacerQueueUs() > 1'500'000 / fps) {
            std::lock_guard l(statsMutex_);
            ++st_.framesSkipped;
            return;
        }
    }
    if (!encoder_ && !openEncoder(ladder_->rung(), fps)) {
        encoderFailed_ = true;
        return;
    }
    const VideoFrame* in = scaled(*f, ladder_->rung().width, ladder_->rung().height);
    if (!in) return;

    const bool key = forceKeyframe_.exchange(false);
    const int64_t t0 = nowUs();
    size_t bytes = 0, parity = 0;
    bool wasKey = false;
    double fecRate = 0;
    const uint8_t codecId = uint8_t(sel_.codec);
    const bool ok = encoder_->encode(*in, key, [&](const uint8_t* data, size_t size, bool isKey) {
        wasKey |= isKey;
        bytes += size;
        PacketizeOptions opt;
        opt.extraFlags = sendingPattern_ ? FragmentHeader::kTestPattern : 0;
        opt.width = uint16_t(in->width);
        opt.height = uint16_t(in->height);
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
                transport_.sendPaced(net::wire::kMedia, streamId_, p, n);
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
    st_.reportedLoss = lossEstimate_.load();
    lastFecRate_ = fecRate;
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

// Path loss before recovery, for display (the sender learns about loss
// from transport-wide feedback).
void VideoReceiveStream::updatePathLoss(int64_t now)
{
    if (now - lastReportUs_ < kPathLossIntervalUs) return;
    lastReportUs_ = now;
    const auto t = transport_.stats();
    const uint64_t exp = t.packetsExpected - reportExpected_, rec = t.packetsReceived - reportReceived_;
    reportExpected_ = t.packetsExpected;
    reportReceived_ = t.packetsReceived;
    if (exp == 0) return;
    const double loss = rec >= exp ? 0.0 : double(exp - rec) / double(exp);
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
        updatePathLoss(nowUs());
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
                if (resetPattern_.exchange(false)) havePattern_ = false;
                if (!havePattern_ || *code > lastPattern_) {
                    ++st_.patternOk;
                } else {
                    ++st_.patternBad;
                    qCWarning(lcVideo) << hubName_ << "pattern out of order: frame" << f.id << "code" << *code
                                       << "after" << lastPattern_ << (f.keyframe ? "(key)" : "")
                                       << (f.recovered ? "(recovered)" : "") << out->width << "x" << out->height;
                }
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
