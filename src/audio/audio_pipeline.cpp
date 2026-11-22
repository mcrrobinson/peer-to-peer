#include "audio/audio_pipeline.h"

#include "core/clock.h"
#include "net/media_transport.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

Q_LOGGING_CATEGORY(lcAudio, "p2p.audio")

namespace p2p::audio {

namespace {
constexpr size_t kAudioHeader = 16;
constexpr size_t kLatencyWindow = 300;
constexpr size_t kIpUdpOverhead = 28;  // IPv4 + UDP, as the congestion controller counts it
constexpr int kRateCheckFrames = 10;   // adapt the bitrate every 100 ms

void putU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint32_t getU32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void putU64(uint8_t* p, uint64_t v) { putU32(p, uint32_t(v >> 32)); putU32(p + 4, uint32_t(v)); }
uint64_t getU64(const uint8_t* p) { return uint64_t(getU32(p)) << 32 | getU32(p + 4); }
} // namespace

AudioPipeline::AudioPipeline(Config cfg, net::MediaTransport& transport)
    : cfg_(std::move(cfg)), transport_(transport)
{
}

AudioPipeline::~AudioPipeline()
{
    stop();
}

void AudioPipeline::attach()
{
    transport_.setHandler(net::wire::kAudio, [this](const uint8_t* d, size_t n, int64_t t) { onPacket(d, n, t); });
}

bool AudioPipeline::start(QString* error)
{
    cfg_.opus.sampleRate = kSampleRate;
    cfg_.opus.channels = 1;
    if (!enc_.init(cfg_.opus) || !dec_.init(kSampleRate, 1)) {
        if (error) *error = QStringLiteral("opus init failed");
        return false;
    }
    bitrate_ = cfg_.opus.bitrate;
    if (cfg_.adaptiveBitrate) {
        AudioRateController::Config rc;
        rc.maxBps = cfg_.opus.bitrate;
        rc.minBps = cfg_.minBitrate;
        rc.overheadBps = int64_t(kAudioHeader + net::wire::kHeaderSize + net::wire::kTagSize + kIpUdpOverhead) * 8 *
                         (kSampleRate / kFrameSamples);
        rate_ = std::make_unique<AudioRateController>(rc);
    }

    io_ = cfg_.useDevice ? createDeviceAudioIO(cfg_.periodMs) : std::make_unique<SimulatedAudioIO>(cfg_.periodMs);
    // APM only makes sense for a real microphone. It is created even with
    // every stage off so stages can be switched on mid-call.
    if (cfg_.source == Source::Mic && cfg_.useDevice)
        apm_.init(kSampleRate, cfg_.echoCancel, cfg_.noiseSuppress, cfg_.autoGain, cfg_.noiseLevel);

    if (!io_->start([this](const int16_t* in, int16_t* out, int n) { process(in, out, n); }, error)) {
        io_.reset();
        return false;
    }
    apm_.setStreamDelayMs(io_->deviceLatencyMs());
    return true;
}

void AudioPipeline::stop()
{
    if (io_) {
        io_->stop();
        io_.reset();
    }
}

// --------------------------------------------------------------- receive

void AudioPipeline::onPacket(const uint8_t* data, size_t len, int64_t arrivalUs)
{
    if (len <= kAudioHeader) return;
    EncodedFrame f;
    f.seq = getU32(data);
    f.timestamp = getU32(data + 4);
    f.captureUs = int64_t(getU64(data + 8));
    f.arrivalUs = arrivalUs;
    f.data.assign(data + kAudioHeader, data + len);
    std::lock_guard lock(jbMutex_);
    jb_.insert(std::move(f));
}

// --------------------------------------------------------------- audio thread

void AudioPipeline::process(const int16_t* in, int16_t* out, int frames)
{
    const int64_t now = nowUs();

    callbacks_.fetch_add(1, std::memory_order_relaxed);
    if (frames < minCbFrames_.load(std::memory_order_relaxed)) minCbFrames_.store(frames, std::memory_order_relaxed);
    if (frames > maxCbFrames_.load(std::memory_order_relaxed)) maxCbFrames_.store(frames, std::memory_order_relaxed);
    if (lastCallbackUs_ != 0) {
        const int64_t gap = now - lastCallbackUs_;
        if (gap > maxGapUs_.load(std::memory_order_relaxed)) maxGapUs_.store(gap, std::memory_order_relaxed);
        if (gap * 2 > int64_t(cfg_.periodMs) * 3000) lateCallbacks_.fetch_add(1, std::memory_order_relaxed);
    }
    lastCallbackUs_ = now;

    // Playout first, so the echo source can loop it straight back.
    const double ratio = playoutRatio();
    while (playout_.size() < playout_.required(frames, ratio)) decodeOne(now);
    playout_.pull(out, frames, ratio);

    // Echo-canceller reference: exactly what goes to the speaker, after the
    // drift resampler has stretched it, in 10 ms frames.
    if (apm_.active()) {
        for (int i = 0; i < frames; ++i) {
            renderFrame_[renderCount_++] = out[i];
            if (renderCount_ == kFrameSamples) {
                apm_.processRender(renderFrame_);
                renderCount_ = 0;
            }
        }
    } else {
        renderCount_ = 0;
    }

    for (int i = 0; i < frames; ++i) {
        int16_t s = 0;
        switch (cfg_.source) {
        case Source::Mic: s = in ? in[i] : 0; break;
        case Source::Tone:
            s = int16_t(8000.0 * std::sin(2.0 * std::numbers::pi * kToneHz * tonePhase_ / kSampleRate));
            tonePhase_ = (tonePhase_ + 1) % kSampleRate;
            break;
        case Source::Silence: s = 0; break;
        case Source::Echo: s = out[i]; break;
        }
        if (capCount_ == 0)
            capStartUs_ = now - int64_t(frames - i) * 1'000'000 / kSampleRate;
        capFrame_[capCount_++] = s;
        if (capCount_ == kFrameSamples) {
            encodeAndSend();
            capCount_ = 0;
        }
    }
}

void AudioPipeline::decodeOne(int64_t nowUs)
{
    JitterBuffer::Result r;
    {
        std::lock_guard lock(jbMutex_);
        r = jb_.pull();
    }

    int16_t pcm[5760]; // up to 120 ms
    int n = 0;
    switch (r.action) {
    case JitterBuffer::Action::Silence:
        std::memset(pcm, 0, sizeof(int16_t) * kFrameSamples);
        n = kFrameSamples;
        break;
    case JitterBuffer::Action::Decode: {
        n = dec_.decode(r.frame.data.data(), int(r.frame.data.size()), pcm, 5760);
        if (n <= 0) n = dec_.plc(pcm, kFrameSamples);
        const int64_t playAt = nowUs + int64_t(playout_.buffered()) * 1'000'000 / kSampleRate;
        const double latMs = double(playAt - (r.frame.captureUs - clockOffsetUs_.load())) / 1000.0;
        std::lock_guard lock(statsMutex_);
        latencies_.push_back(latMs);
        while (latencies_.size() > kLatencyWindow) latencies_.pop_front();
        break;
    }
    case JitterBuffer::Action::Conceal: {
        OpusDecoderWrapper::Method m{};
        n = dec_.conceal(r.frame.data.data(), int(r.frame.data.size()), r.distance, pcm, kFrameSamples, &m);
        (m == OpusDecoderWrapper::Method::Dred ? concealDred_ : m == OpusDecoderWrapper::Method::Fec ? concealFec_
                                                                                                        : concealPlc_)
            .fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case JitterBuffer::Action::Underrun:
        n = dec_.plc(pcm, kFrameSamples);
        break;
    }
    if (n <= 0) {
        std::memset(pcm, 0, sizeof(int16_t) * kFrameSamples);
        n = kFrameSamples;
    }

    if (r.action != JitterBuffer::Action::Silence) analyse(pcm, n);

    playout_.push(pcm, n);
}

double AudioPipeline::playoutRatio()
{
    // Steer playout speed so that buffered audio (jitter buffer + decoded
    // backlog) tracks the jitter-buffer target. A proportional controller on
    // a ~200 ms smoothed level absorbs sound-card clock drift (hundreds of
    // ppm) and drains bursts without audible frame drops. Clamped to +-2 %.
    int level = 0, target = 1;
    {
        std::lock_guard lock(jbMutex_);
        level = jb_.levelFrames();
        target = jb_.targetFrames();
    }
    const double buffered = double(level) * kFrameSamples + playout_.buffered();
    smoothedBufferedSamples_ += 0.05 * (buffered - smoothedBufferedSamples_);
    const double desired = double(target) * kFrameSamples + kFrameSamples / 2.0;
    const double ratio = std::clamp(1.0 + 0.5 * (smoothedBufferedSamples_ - desired) / kSampleRate, 0.98, 1.02);
    ratio_.store(ratio, std::memory_order_relaxed);
    return ratio;
}

void AudioPipeline::encodeAndSend()
{
    if (apm_.active()) apm_.processCapture(capFrame_);
    if (muted_.load(std::memory_order_relaxed)) std::memset(capFrame_, 0, sizeof(capFrame_));

    uint8_t pkt[net::wire::kMaxPayload];
    putU32(pkt, seq_);
    putU32(pkt + 4, ts_);
    putU64(pkt + 8, uint64_t(capStartUs_));
    if (rate_ && seq_ % kRateCheckFrames == 0) adaptBitrate(capStartUs_);
    const int n = enc_.encode(capFrame_, kFrameSamples, pkt + kAudioHeader, int(sizeof(pkt) - kAudioHeader));
    ++seq_;
    ts_ += kFrameSamples;
    if (n <= 0) return;
    if (transport_.send(net::wire::kMedia, net::wire::kAudio, pkt, kAudioHeader + size_t(n))) {
        framesSent_.fetch_add(1, std::memory_order_relaxed);
        bytesSent_.fetch_add(kAudioHeader + size_t(n), std::memory_order_relaxed);
    }
}

void AudioPipeline::adaptBitrate(int64_t nowUs)
{
    if (!transport_.congestionControlEnabled()) return;
    if (!rate_->update(transport_.targetBps(), reservedBps_.load(std::memory_order_relaxed), nowUs)) return;
    const int bps = rate_->bitrate();
    enc_.setBitrate(bps);
    bitrate_.store(bps, std::memory_order_relaxed);
    bitrateChanges_.fetch_add(1, std::memory_order_relaxed);
    qCInfo(lcAudio) << "opus bitrate ->" << bps / 1000 << "kbps (cc target" << transport_.targetBps() / 1000
                    << "kbps)";
}

void AudioPipeline::analyse(const int16_t* pcm, int n)
{
    // Output level, plus Goertzel energy at the test-tone frequency per 10 ms block.
    for (int off = 0; off + kFrameSamples <= n; off += kFrameSamples) {
        const double coeff = 2.0 * std::cos(2.0 * std::numbers::pi * kToneHz / kSampleRate);
        double s1 = 0, s2 = 0, sq = 0;
        for (int i = 0; i < kFrameSamples; ++i) {
            const double x = pcm[off + i] / 32768.0;
            sq += x * x;
            const double s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;    // |X|^2
        toneSumSq_ += 2.0 * power / kFrameSamples;                   // energy at the bin
        sumSq_ += sq;
        analysedSamples_ += kFrameSamples;
    }
    if (analysedSamples_ >= kSampleRate) { // publish once per second
        const double ms = sumSq_ / analysedSamples_;
        std::lock_guard lock(statsMutex_);
        outputRmsDb_ = ms > 0 ? 10.0 * std::log10(ms) : -120.0;
        toneFraction_ = sumSq_ > 0 ? std::min(1.0, toneSumSq_ / sumSq_) : 0.0;
        sumSq_ = toneSumSq_ = 0;
        analysedSamples_ = 0;
    }
}

// --------------------------------------------------------------- stats

AudioPipeline::Stats AudioPipeline::stats() const
{
    Stats s;
    s.framesSent = framesSent_.load();
    s.bytesSent = bytesSent_.load();
    {
        std::lock_guard lock(jbMutex_);
        s.jb = jb_.stats();
        s.targetMs = jb_.targetFrames() * 10;
        s.levelMs = jb_.levelFrames() * 10;
        s.jitterP95Ms = jb_.jitterP95Ms();
    }
    s.concealDred = concealDred_.load();
    s.concealFec = concealFec_.load();
    s.concealPlc = concealPlc_.load();
    {
        std::lock_guard lock(statsMutex_);
        if (!latencies_.empty()) {
            std::vector<double> v(latencies_.begin(), latencies_.end());
            double sum = 0;
            for (double x : v) sum += x;
            s.latencyAvgMs = sum / double(v.size());
            const size_t k = std::min(v.size() - 1, (v.size() * 95) / 100);
            std::nth_element(v.begin(), v.begin() + long(k), v.end());
            s.latencyP95Ms = v[k];
        }
        s.outputRmsDb = outputRmsDb_;
        s.toneFraction = toneFraction_;
    }
    s.callbacks = callbacks_.load();
    s.minCallbackFrames = s.callbacks ? minCbFrames_.load() : 0;
    s.maxCallbackFrames = maxCbFrames_.load();
    s.maxCallbackGapMs = double(maxGapUs_.load()) / 1000.0;
    s.lateCallbacks = lateCallbacks_.load();
    s.driftPpm = (ratio_.load() - 1.0) * 1e6;
    s.processingActive = apm_.active();
    s.echoCancel = apm_.echoCancel();
    s.noiseSuppress = apm_.noiseSuppress();
    s.autoGain = apm_.autoGain();
    s.dred = enc_.dredEnabled();
    s.lbrr = enc_.lbrrEnabled();
    s.bitrateKbps = bitrate_.load() / 1000;
    s.bitrateChanges = bitrateChanges_.load();
    if (io_) {
        s.deviceLatencyMs = io_->deviceLatencyMs();
        s.device = io_->description();
    }
    return s;
}

QString AudioPipeline::format(const Stats& s)
{
    return QStringLiteral("audio tx %1 frames | rx %2 dec %3 conceal %4 (dred %5 fec %6 plc %7) underrun %8 late %9 drop %10\n"
                          "jb target %11 ms level %12 ms jitter p95 %13 ms | latency avg %14 ms p95 %15 ms (+%16 ms device)\n"
                          "out %17 dBFS tone %18% | %19 | opus %28 kbps (changes %29) fec %20\n"
                          "device cb %21 frames %22-%23 max gap %24 ms late %25 | late-after-wait %26 | drift-comp %27 ppm")
        .arg(s.framesSent)
        .arg(s.jb.received)
        .arg(s.jb.decoded)
        .arg(s.jb.concealed)
        .arg(s.concealDred)
        .arg(s.concealFec)
        .arg(s.concealPlc)
        .arg(s.jb.underruns)
        .arg(s.jb.late)
        .arg(s.jb.dropped)
        .arg(s.targetMs)
        .arg(s.levelMs)
        .arg(s.jitterP95Ms, 0, 'f', 2)
        .arg(s.latencyAvgMs, 0, 'f', 1)
        .arg(s.latencyP95Ms, 0, 'f', 1)
        .arg(s.deviceLatencyMs)
        .arg(s.outputRmsDb, 0, 'f', 1)
        .arg(s.toneFraction * 100.0, 0, 'f', 0)
        .arg(QStringLiteral("AEC %1 NS %2 AGC %3")
                 .arg(s.echoCancel ? "on" : "off", s.noiseSuppress ? "on" : "off", s.autoGain ? "on" : "off"))
        .arg(s.dred ? "dred" : s.lbrr ? "lbrr" : "off")
        .arg(s.callbacks)
        .arg(s.minCallbackFrames)
        .arg(s.maxCallbackFrames)
        .arg(s.maxCallbackGapMs, 0, 'f', 1)
        .arg(s.lateCallbacks)
        .arg(s.jb.lateAfterWait)
        .arg(qRound(s.driftPpm))
        .arg(s.bitrateKbps)
        .arg(s.bitrateChanges);
}

} // namespace p2p::audio
