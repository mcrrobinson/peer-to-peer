#include "video/hevc_vt_encoder.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <QLoggingCategory>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

Q_LOGGING_CATEGORY(lcVt, "p2p.encoder.vt", QtInfoMsg)

namespace p2p::video {

namespace {

// Owns one CF reference.
template <typename T> struct CfRef {
    T ref = nullptr;
    CfRef() = default;
    explicit CfRef(T r) : ref(r) {}
    ~CfRef()
    {
        if (ref) CFRelease(ref);
    }
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    operator T() const { return ref; }
};

CFNumberRef cfInt(int64_t v) { return CFNumberCreate(nullptr, kCFNumberSInt64Type, &v); }

CFMutableDictionaryRef cfDict()
{
    return CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

void appendStartCode(std::vector<uint8_t>& v) { v.insert(v.end(), {0, 0, 0, 1}); }

} // namespace

struct HevcVtEncoder::Impl {
    Config cfg;
    VTCompressionSessionRef session = nullptr;

    // Filled by the output callback (VideoToolbox's thread) for |waitingSeq|.
    std::mutex m;
    std::condition_variable cv;
    int64_t waitingSeq = -1;
    bool done = false;
    bool dropped = false;
    OSStatus status = noErr;
    std::vector<uint8_t> annexB;
    bool key = false;
    std::optional<int64_t> ltrToken; // set if the frame is an LTR

    int64_t seq = 0;            // presentation index fed to VideoToolbox
    int refBack = 0;
    uint64_t drops = 0;
    // Recent LTRs the receiver may still have, oldest first: our frame id,
    // VideoToolbox's token. Cleared on a keyframe (it empties the DPB).
    static constexpr size_t kMaxLtrs = 16;
    std::deque<std::pair<uint32_t, int64_t>> ltrs;
    // Pending recovery: tokens to acknowledge and the newest of their frames.
    std::vector<int64_t> refreshTokens;
    std::optional<uint32_t> refreshBase;

    ~Impl()
    {
        if (session) {
            VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }

    static void onOutput(void* ref, void*, OSStatus st, VTEncodeInfoFlags flags, CMSampleBufferRef sb)
    {
        auto* self = static_cast<Impl*>(ref);
        std::lock_guard l(self->m);
        self->status = st;
        self->dropped = st != noErr || !sb || (flags & kVTEncodeInfo_FrameDropped);
        if (!self->dropped) self->collect(sb);
        self->done = true;
        self->cv.notify_all();
    }

    // Sample buffer (length-prefixed NAL units, parameter sets in the format
    // description) -> Annex B, parameter sets first on a keyframe.
    void collect(CMSampleBufferRef sb)
    {
        annexB.clear();
        key = true;
        ltrToken.reset();
        if (CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false); att && CFArrayGetCount(att) > 0) {
            auto* a = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(att, 0));
            key = !CFDictionaryContainsKey(a, kCMSampleAttachmentKey_NotSync);
            if (auto* t = static_cast<CFNumberRef>(CFDictionaryGetValue(a, kVTSampleAttachmentKey_RequireLTRAcknowledgementToken))) {
                int64_t v = 0;
                if (CFNumberGetValue(t, kCFNumberSInt64Type, &v)) ltrToken = v;
            }
        }
        CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
        size_t sets = 0;
        int lenSize = 4;
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, nullptr, nullptr, &sets, &lenSize);
        if (key) {
            for (size_t i = 0; i < sets; ++i) {
                const uint8_t* p = nullptr;
                size_t n = 0;
                if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, i, &p, &n, nullptr, nullptr) != noErr) continue;
                appendStartCode(annexB);
                annexB.insert(annexB.end(), p, p + n);
            }
        }
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        const size_t total = bb ? CMBlockBufferGetDataLength(bb) : 0;
        std::vector<uint8_t> data(total);
        if (total) CMBlockBufferCopyDataBytes(bb, 0, total, data.data());
        for (size_t off = 0; off + size_t(lenSize) <= total;) {
            size_t len = 0;
            for (int i = 0; i < lenSize; ++i) len = (len << 8) | data[off + size_t(i)];
            off += size_t(lenSize);
            if (off + len > total) break;
            appendStartCode(annexB);
            annexB.insert(annexB.end(), data.begin() + std::ptrdiff_t(off), data.begin() + std::ptrdiff_t(off + len));
            off += len;
        }
    }

    bool setProp(CFStringRef key, CFTypeRef value, const char* name, QString* why)
    {
        const OSStatus st = VTSessionSetProperty(session, key, value);
        if (st == noErr) return true;
        if (why) *why = QStringLiteral("VideoToolbox %1: %2").arg(QLatin1String(name)).arg(st);
        return false;
    }
};

HevcVtEncoder::HevcVtEncoder() : d(std::make_unique<Impl>()) {}
HevcVtEncoder::~HevcVtEncoder() = default;

bool HevcVtEncoder::init(const Config& cfg, QString* why)
{
    d->cfg = cfg;
    CfRef<CFMutableDictionaryRef> spec(cfDict());
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, kCFBooleanTrue);

    // Source buffers from the session's pool: NV12 in the frame's range.
    const bool full = cfg.colour == Colour::Bt601Full;
    CfRef<CFMutableDictionaryRef> src(cfDict());
    CfRef<CFNumberRef> fmt(cfInt(full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                      : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange));
    CfRef<CFNumberRef> w(cfInt(cfg.width)), h(cfInt(cfg.height));
    CfRef<CFMutableDictionaryRef> ioSurface(cfDict());
    CFDictionarySetValue(src, kCVPixelBufferPixelFormatTypeKey, fmt);
    CFDictionarySetValue(src, kCVPixelBufferWidthKey, w);
    CFDictionarySetValue(src, kCVPixelBufferHeightKey, h);
    CFDictionarySetValue(src, kCVPixelBufferIOSurfacePropertiesKey, ioSurface);

    OSStatus st = VTCompressionSessionCreate(nullptr, cfg.width, cfg.height, kCMVideoCodecType_HEVC, spec, src,
                                             nullptr, &Impl::onOutput, d.get(), &d->session);
    if (st != noErr) {
        if (why) *why = QStringLiteral("VTCompressionSessionCreate: %1").arg(st);
        d->session = nullptr;
        return false;
    }
    CfRef<CFNumberRef> fps(cfInt(cfg.fps)), maxKeyInterval(cfInt(1 << 30)), rate(cfInt(int64_t(cfg.bitrateKbps) * 1000));
    const bool ok =
        d->setProp(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue, "RealTime", why) &&
        d->setProp(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, "AllowFrameReordering", why) &&
        d->setProp(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel, "ProfileLevel", why) &&
        d->setProp(kVTCompressionPropertyKey_MaxKeyFrameInterval, maxKeyInterval, "MaxKeyFrameInterval", why) &&
        d->setProp(kVTCompressionPropertyKey_ExpectedFrameRate, fps, "ExpectedFrameRate", why) &&
        d->setProp(kVTCompressionPropertyKey_AverageBitRate, rate, "AverageBitRate", why) &&
        d->setProp(kVTCompressionPropertyKey_EnableLTR, kCFBooleanTrue, "EnableLTR", why) &&
        // Colours in the VUI (the range comes from the pixel format).
        d->setProp(kVTCompressionPropertyKey_ColorPrimaries,
                   full ? kCVImageBufferColorPrimaries_SMPTE_C : kCVImageBufferColorPrimaries_ITU_R_709_2,
                   "ColorPrimaries", why) &&
        d->setProp(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2,
                   "TransferFunction", why) &&
        d->setProp(kVTCompressionPropertyKey_YCbCrMatrix,
                   full ? kCVImageBufferYCbCrMatrix_ITU_R_601_4 : kCVImageBufferYCbCrMatrix_ITU_R_709_2, "YCbCrMatrix",
                   why);
    if (!ok) return false;
    if ((st = VTCompressionSessionPrepareToEncodeFrames(d->session)) != noErr) {
        if (why) *why = QStringLiteral("VTCompressionSessionPrepareToEncodeFrames: %1").arg(st);
        return false;
    }
    return true;
}

bool HevcVtEncoder::encode(const VideoFrame& f, uint32_t id, bool forceKeyframe, const Output& out)
{
    if (!d->session || f.width != d->cfg.width || f.height != d->cfg.height || !f.ensureCpu()) return false;
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(d->session);
    CVPixelBufferRef pb = nullptr;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb) != kCVReturnSuccess) {
        qCWarning(lcVt) << "no pixel buffer";
        return false;
    }
    CfRef<CVPixelBufferRef> pbRef(pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    for (size_t p = 0; p < 2; ++p) {
        auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, p));
        const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        const uint8_t* src = p ? f.uv() : f.y();
        const int rows = p ? f.height / 2 : f.height;
        for (int r = 0; r < rows; ++r)
            std::memcpy(dst + size_t(r) * stride, src + size_t(r) * size_t(f.width), size_t(f.width));
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);

    // Per-frame options: a keyframe, or an LTR refresh after a loss.
    CfRef<CFMutableDictionaryRef> opts;
    std::optional<uint32_t> refreshBase;
    if (forceKeyframe) {
        opts.ref = cfDict();
        CFDictionarySetValue(opts, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        d->refreshTokens.clear();
        d->refreshBase.reset();
    } else if (d->refreshBase) {
        // Every LTR the receiver decoded: VideoToolbox keeps several and
        // refreshes from one it still has (acknowledging only the newest
        // fails when losses follow each other: it then sends keyframes).
        opts.ref = cfDict();
        std::vector<CFNumberRef> nums;
        for (int64_t t : d->refreshTokens) nums.push_back(cfInt(t));
        CfRef<CFArrayRef> acked(CFArrayCreate(nullptr, reinterpret_cast<const void**>(nums.data()), CFIndex(nums.size()),
                                              &kCFTypeArrayCallBacks));
        for (CFNumberRef n : nums) CFRelease(n);
        CFDictionarySetValue(opts, kVTEncodeFrameOptionKey_AcknowledgedLTRTokens, acked);
        CFDictionarySetValue(opts, kVTEncodeFrameOptionKey_ForceLTRRefresh, kCFBooleanTrue);
        refreshBase = d->refreshBase;
    }

    const int64_t seq = d->seq++;
    {
        std::lock_guard l(d->m);
        d->waitingSeq = seq;
        d->done = false;
    }
    const OSStatus st = VTCompressionSessionEncodeFrame(d->session, pb, CMTimeMake(seq, d->cfg.fps),
                                                        CMTimeMake(1, d->cfg.fps), opts, nullptr, nullptr);
    if (st != noErr) {
        qCWarning(lcVt) << "VTCompressionSessionEncodeFrame:" << st;
        return false;
    }
    // One frame in flight: the output (or the drop) comes back for this frame.
    std::unique_lock l(d->m);
    if (!d->cv.wait_for(l, std::chrono::milliseconds(500), [&] { return d->done; })) {
        l.unlock();
        VTCompressionSessionCompleteFrames(d->session, kCMTimeInvalid);
        l.lock();
        if (!d->done) {
            qCWarning(lcVt) << "no output for frame" << id;
            return false;
        }
    }
    if (d->status != noErr) {
        qCWarning(lcVt) << "encode failed:" << d->status;
        return false;
    }
    if (d->dropped) {
        ++d->drops;
        return true; // the refresh (if any) stays pending for the next frame
    }
    d->refreshTokens.clear();
    d->refreshBase.reset();
    if (refreshBase || d->key || d->ltrToken)
        qCDebug(lcVt) << "frame" << id << (d->key ? "key" : "P") << "refresh from" << (refreshBase ? int(*refreshBase) : -1)
                      << "ltr token" << (d->ltrToken ? *d->ltrToken : -1) << "bytes" << d->annexB.size();
    d->refBack = d->key ? 0 : refreshBase ? int(id - *refreshBase) : 1;
    if (d->key) d->ltrs.clear(); // a keyframe starts a new chain
    if (d->ltrToken) {
        d->ltrs.emplace_back(id, *d->ltrToken);
        if (d->ltrs.size() > Impl::kMaxLtrs) d->ltrs.pop_front();
    }
    std::vector<uint8_t> au;
    au.swap(d->annexB);
    const bool key = d->key;
    l.unlock();
    out(au.data(), au.size(), key);
    return true;
}

int HevcVtEncoder::lastRefBack() const { return d->refBack; }

bool HevcVtEncoder::recoverFrom(uint32_t goodId)
{
    // The receiver lost everything after |goodId|: an LTR after it is gone
    // for it, and VideoToolbox keeps no older one.
    // The receiver lost everything after |goodId|, LTRs included.
    while (!d->ltrs.empty() && int32_t(d->ltrs.back().first - goodId) > 0) d->ltrs.pop_back();
    qCDebug(lcVt) << "recoverFrom" << goodId << "LTRs" << d->ltrs.size() << "newest"
                  << (d->ltrs.empty() ? -1 : int64_t(d->ltrs.back().first));
    d->refreshTokens.clear();
    d->refreshBase.reset();
    if (d->ltrs.empty()) return false;
    for (const auto& l : d->ltrs) d->refreshTokens.push_back(l.second);
    d->refreshBase = d->ltrs.back().first;
    return true;
}

void HevcVtEncoder::setBitrateKbps(int kbps)
{
    if (kbps == d->cfg.bitrateKbps || !d->session) return;
    d->cfg.bitrateKbps = kbps;
    CfRef<CFNumberRef> rate(cfInt(int64_t(kbps) * 1000));
    if (OSStatus st = VTSessionSetProperty(d->session, kVTCompressionPropertyKey_AverageBitRate, rate); st != noErr)
        qCWarning(lcVt) << "AverageBitRate" << kbps << "kbps:" << st;
}

uint64_t HevcVtEncoder::droppedFrames() const { return d->drops; }

} // namespace p2p::video
