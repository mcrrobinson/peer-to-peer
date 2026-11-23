#pragma once

struct AVBufferRef;

namespace p2p::video {

// One VA-API device for the process (Linux), so camera surfaces can go to
// the encoder without a copy: surfaces only work on the display that made
// them. Returns a new reference (caller unrefs), or nullptr without VA-API.
AVBufferRef* sharedVaapiDevice();

} // namespace p2p::video
