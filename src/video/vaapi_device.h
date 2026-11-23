#pragma once

#include "video/video_frame.h"

struct AVBufferRef;
struct AVFrame;

namespace p2p::video {

// One VA-API device for the process (Linux), so camera surfaces can go to
// the encoder without a copy: surfaces only work on the display that made
// them. Returns a new reference (caller unrefs), or nullptr without VA-API.
AVBufferRef* sharedVaapiDevice();

// Exports an NV12 VA surface (AV_PIX_FMT_VAAPI frame) as dma-bufs for a
// w x h picture; false if the driver can't (or the layout isn't Y + UV).
bool exportVaapiNv12(const AVFrame* frame, int w, int h, DmaBufNv12& out);
// Closes the fds of an export.
void closeDmaBuf(DmaBufNv12& d);

} // namespace p2p::video
