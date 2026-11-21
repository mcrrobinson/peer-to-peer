#include "video/codec_probe.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace p2p::video {

// Placeholder until the macOS (VideoToolbox) probe lands: software only.
media::MediaCaps probeLocalCaps()
{
    using media::Codec;
    media::MediaCaps caps;
    if (avcodec_find_encoder_by_name("libsvtav1")) caps.encoders.append({Codec::AV1, false, 3840, 2160});
    if (avcodec_find_encoder_by_name("libx265")) caps.encoders.append({Codec::HEVC, false, 3840, 2160});
    if (avcodec_find_decoder_by_name("libdav1d")) caps.decoders.append({Codec::AV1, false, 3840, 2160});
    if (avcodec_find_decoder_by_name("hevc")) caps.decoders.append({Codec::HEVC, false, 3840, 2160});
    return caps;
}

} // namespace p2p::video
