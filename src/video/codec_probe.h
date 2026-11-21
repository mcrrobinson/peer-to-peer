#pragma once

#include "media/codec_caps.h"

namespace p2p::video {

// Probes which HEVC/AV1 encoders and decoders this host can actually run:
// hardware via VA-API (Linux) / VideoToolbox (macOS, later), software via the
// FFmpeg wrappers that are compiled in (libx265, libsvtav1, hevc, libdav1d).
media::MediaCaps probeLocalCaps();

} // namespace p2p::video
