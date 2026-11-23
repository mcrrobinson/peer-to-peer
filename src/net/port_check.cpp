#include "net/port_check.h"

#ifdef __APPLE__
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace p2p::net {

bool loopbackPortShadowed(uint16_t port, bool udp)
{
#ifdef __APPLE__
    const int fd = ::socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) return false;
    // SO_REUSEADDR: coexist with our own wildcard socket, not with another
    // socket on exactly 127.0.0.1:port.
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool taken = ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 && errno == EADDRINUSE;
    ::close(fd);
    return taken;
#else
    (void)port;
    (void)udp;
    return false;
#endif
}

} // namespace p2p::net
