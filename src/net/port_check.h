#pragma once

#include <cstdint>

namespace p2p::net {

// macOS lets a dual-stack (::) socket bind a port another process already
// holds on 127.0.0.1 (Linux refuses with EADDRINUSE); connections and
// datagrams to 127.0.0.1:port then reach that process, not us. True if
// |port| is taken that way, checked with a probe bind on 127.0.0.1 while our
// own socket is bound. Always false off macOS. Only needed for sockets with
// SO_REUSEADDR (QTcpServer sets it); without it macOS refuses the bind.
bool loopbackPortShadowed(uint16_t port, bool udp);

} // namespace p2p::net
