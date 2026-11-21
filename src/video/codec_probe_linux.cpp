#include "video/codec_probe.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <va/va.h>
#include <va/va_drm.h>

#include <QLoggingCategory>

#include <fcntl.h>
#include <unistd.h>

#include <vector>

Q_LOGGING_CATEGORY(lcProbe, "p2p.probe")

namespace p2p::video {

namespace {

struct VaSupport {
    bool hevcEnc = false, hevcDec = false, av1Enc = false, av1Dec = false;
    int hevcEncMaxW = 0, hevcEncMaxH = 0;
    int av1EncMaxW = 0, av1EncMaxH = 0;
    QString driver;
};

void queryMaxSize(VADisplay d, VAProfile p, VAEntrypoint ep, int& w, int& h)
{
    VAConfigAttrib attrs[2] = {{VAConfigAttribMaxPictureWidth, 0}, {VAConfigAttribMaxPictureHeight, 0}};
    if (vaGetConfigAttributes(d, p, ep, attrs, 2) != VA_STATUS_SUCCESS)
        return;
    if (attrs[0].value != VA_ATTRIB_NOT_SUPPORTED) w = int(attrs[0].value);
    if (attrs[1].value != VA_ATTRIB_NOT_SUPPORTED) h = int(attrs[1].value);
}

VaSupport probeVaapi()
{
    VaSupport s;
    for (int node = 128; node < 136; ++node) {
        const std::string path = "/dev/dri/renderD" + std::to_string(node);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        VADisplay d = vaGetDisplayDRM(fd);
        int maj = 0, min = 0;
        if (!d || vaInitialize(d, &maj, &min) != VA_STATUS_SUCCESS) {
            if (d) vaTerminate(d);
            ::close(fd);
            continue;
        }
        s.driver = QString::fromUtf8(vaQueryVendorString(d));

        int nProfiles = vaMaxNumProfiles(d);
        std::vector<VAProfile> profiles(size_t(std::max(nProfiles, 0)));
        vaQueryConfigProfiles(d, profiles.data(), &nProfiles);
        profiles.resize(size_t(std::max(nProfiles, 0)));

        std::vector<VAEntrypoint> eps(size_t(std::max(vaMaxNumEntrypoints(d), 0)));
        for (VAProfile p : profiles) {
            const bool isHevc = p == VAProfileHEVCMain;
            const bool isAv1 = p == VAProfileAV1Profile0;
            if (!isHevc && !isAv1) continue;
            int nEps = int(eps.size());
            if (vaQueryConfigEntrypoints(d, p, eps.data(), &nEps) != VA_STATUS_SUCCESS) continue;
            for (int i = 0; i < nEps; ++i) {
                const VAEntrypoint ep = eps[size_t(i)];
                const bool enc = ep == VAEntrypointEncSlice || ep == VAEntrypointEncSliceLP;
                const bool dec = ep == VAEntrypointVLD;
                if (isHevc) {
                    if (dec) s.hevcDec = true;
                    if (enc && !s.hevcEnc) { s.hevcEnc = true; queryMaxSize(d, p, ep, s.hevcEncMaxW, s.hevcEncMaxH); }
                } else {
                    if (dec) s.av1Dec = true;
                    if (enc && !s.av1Enc) { s.av1Enc = true; queryMaxSize(d, p, ep, s.av1EncMaxW, s.av1EncMaxH); }
                }
            }
        }
        vaTerminate(d);
        ::close(fd);
        break; // first working render node
    }
    return s;
}

bool haveEncoder(const char* name) { return avcodec_find_encoder_by_name(name) != nullptr; }
bool haveDecoder(const char* name) { return avcodec_find_decoder_by_name(name) != nullptr; }

constexpr int kHwDefaultMaxW = 4096, kHwDefaultMaxH = 2304;
constexpr int kSwMaxW = 3840, kSwMaxH = 2160;

} // namespace

media::MediaCaps probeLocalCaps()
{
    using media::Codec;
    const VaSupport va = probeVaapi();
    qCInfo(lcProbe) << "VA-API driver:" << (va.driver.isEmpty() ? QStringLiteral("none") : va.driver)
                    << "HEVC enc/dec" << va.hevcEnc << va.hevcDec << "AV1 enc/dec" << va.av1Enc << va.av1Dec;

    media::MediaCaps caps;
    auto orDefault = [](int v, int d) { return v > 0 ? v : d; };

    // Hardware paths require both the VA-API entrypoint and the FFmpeg wrapper.
    if (va.hevcEnc && haveEncoder("hevc_vaapi"))
        caps.encoders.append({Codec::HEVC, true, orDefault(va.hevcEncMaxW, kHwDefaultMaxW), orDefault(va.hevcEncMaxH, kHwDefaultMaxH)});
    if (va.av1Enc && haveEncoder("av1_vaapi"))
        caps.encoders.append({Codec::AV1, true, orDefault(va.av1EncMaxW, kHwDefaultMaxW), orDefault(va.av1EncMaxH, kHwDefaultMaxH)});
    if (haveEncoder("libsvtav1"))
        caps.encoders.append({Codec::AV1, false, kSwMaxW, kSwMaxH});
    if (haveEncoder("libx265"))
        caps.encoders.append({Codec::HEVC, false, kSwMaxW, kSwMaxH});

    const bool hevcDecoder = haveDecoder("hevc");
    if (va.hevcDec && hevcDecoder)
        caps.decoders.append({Codec::HEVC, true, kHwDefaultMaxW, kHwDefaultMaxH});
    if (va.av1Dec && haveDecoder("av1"))
        caps.decoders.append({Codec::AV1, true, kHwDefaultMaxW, kHwDefaultMaxH});
    if (haveDecoder("libdav1d"))
        caps.decoders.append({Codec::AV1, false, kSwMaxW, kSwMaxH});
    if (hevcDecoder)
        caps.decoders.append({Codec::HEVC, false, kSwMaxW, kSwMaxH});

    qCInfo(lcProbe).noquote() << "local caps:" << caps.summary();
    return caps;
}

} // namespace p2p::video
