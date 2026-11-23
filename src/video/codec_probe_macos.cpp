#include "video/codec_probe.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcProbe, "p2p.probe")

namespace p2p::video {

namespace {

// A hardware encoder for |codec| in VideoToolbox's encoder list.
bool vtHardwareEncoder(CMVideoCodecType codec)
{
    CFArrayRef list = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &list) != noErr || !list) return false;
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(list) && !found; ++i) {
        auto* e = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        auto* type = static_cast<CFNumberRef>(CFDictionaryGetValue(e, kVTVideoEncoderList_CodecType));
        auto* hw = static_cast<CFBooleanRef>(CFDictionaryGetValue(e, kVTVideoEncoderList_IsHardwareAccelerated));
        int32_t t = 0;
        if (type && CFNumberGetValue(type, kCFNumberSInt32Type, &t) && CMVideoCodecType(t) == codec && hw &&
            CFBooleanGetValue(hw))
            found = true;
    }
    CFRelease(list);
    return found;
}

bool haveEncoder(const char* name) { return avcodec_find_encoder_by_name(name) != nullptr; }
bool haveDecoder(const char* name) { return avcodec_find_decoder_by_name(name) != nullptr; }

constexpr int kHwMaxW = 4096, kHwMaxH = 2304;
constexpr int kSwMaxW = 3840, kSwMaxH = 2160;

} // namespace

media::MediaCaps probeLocalCaps()
{
    using media::Codec;
    const bool hevcEnc = vtHardwareEncoder(kCMVideoCodecType_HEVC);
    const bool hevcDec = VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC);
    const bool av1Dec = VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1);
    qCInfo(lcProbe) << "VideoToolbox: HEVC enc/dec" << hevcEnc << hevcDec << "AV1 dec" << av1Dec;

    media::MediaCaps caps;
    // Hardware encode: our VideoToolbox encoder (hevc_vt_encoder.h), FFmpeg's
    // hevc_videotoolbox as the fallback.
    if (hevcEnc && haveEncoder("hevc_videotoolbox")) caps.encoders.append({Codec::HEVC, true, kHwMaxW, kHwMaxH});
    if (haveEncoder("libsvtav1")) caps.encoders.append({Codec::AV1, false, kSwMaxW, kSwMaxH});
    if (haveEncoder("libx265")) caps.encoders.append({Codec::HEVC, false, kSwMaxW, kSwMaxH});

    // HEVC decode: FFmpeg's hevc decoder with the VideoToolbox hwaccel.
    // AV1 stays software (the hwaccel path isn't wired up for AV1).
    const bool hevcDecoder = haveDecoder("hevc");
    if (hevcDec && hevcDecoder) caps.decoders.append({Codec::HEVC, true, kHwMaxW, kHwMaxH});
    if (haveDecoder("libdav1d")) caps.decoders.append({Codec::AV1, false, kSwMaxW, kSwMaxH});
    if (hevcDecoder) caps.decoders.append({Codec::HEVC, false, kSwMaxW, kSwMaxH});

    qCInfo(lcProbe).noquote() << "local caps:" << caps.summary();
    return caps;
}

} // namespace p2p::video
