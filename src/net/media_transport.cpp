#include "net/media_transport.h"

#include "core/clock.h"

#include <QLoggingCategory>

#include <sodium.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

Q_LOGGING_CATEGORY(lcMedia, "p2p.transport")

namespace p2p::net {

using namespace wire;

namespace {
void putU64(uint8_t* p, uint64_t v)
{
    for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v); v >>= 8; }
}
uint64_t getU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
void makeNonce(uint64_t pn, unsigned char n[crypto_aead_chacha20poly1305_IETF_NPUBBYTES])
{
    std::memset(n, 0, crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    putU64(n, pn);
}
} // namespace

// ---------------------------------------------------------------- ReplayWindow

bool ReplayWindow::check(uint64_t pn) const
{
    if (pn == 0) return false;
    if (!any_ || pn > highest_) return true;
    if (highest_ - pn >= kSize) return false;
    return !seen_.test(pn % kSize);
}

void ReplayWindow::mark(uint64_t pn)
{
    if (!any_) {
        any_ = true;
        highest_ = pn;
        seen_.reset();
    } else if (pn > highest_) {
        const uint64_t advance = pn - highest_;
        if (advance >= kSize) {
            seen_.reset();
        } else {
            for (uint64_t i = highest_ + 1; i <= pn; ++i) seen_.reset(i % kSize);
        }
        highest_ = pn;
    }
    seen_.set(pn % kSize);
}

// -------------------------------------------------------------- MediaTransport

MediaTransport::MediaTransport() = default;

MediaTransport::~MediaTransport()
{
    stop();
    if (fd_ >= 0) ::close(fd_);
    sodium_memzero(tx_.data(), tx_.size());
    sodium_memzero(rx_.data(), rx_.size());
}

bool MediaTransport::open(uint16_t port, QString* error)
{
    auto fail = [&](const char* what) {
        if (error) *error = QStringLiteral("%1: %2").arg(what, QString::fromLocal8Bit(std::strerror(errno)));
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        return false;
    };

    fd_ = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd_ < 0) return fail("socket");
    ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);

    int off = 0;
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)); // dual-stack
    int buf = 4 << 20;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    // DSCP EF for the whole socket for now; per-stream marking comes with
    // the congestion controller. Failures are harmless.
    int tos = 0xB8;
    ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));

    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_addr = in6addr_any;
    sa.sin6_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) return fail("bind");

    socklen_t len = sizeof(sa);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
    localPort_ = ntohs(sa.sin6_port);
    return true;
}

void MediaTransport::setKeys(const crypto::Key& tx, const crypto::Key& rx)
{
    tx_ = tx;
    rx_ = rx;
}

bool MediaTransport::setRemote(const QHostAddress& addr, uint16_t port)
{
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    bool isV4 = false;
    const quint32 v4 = addr.toIPv4Address(&isV4);
    if (isV4) {
        // IPv4-mapped IPv6 address ::ffff:a.b.c.d
        sa.sin6_addr.s6_addr[10] = 0xff;
        sa.sin6_addr.s6_addr[11] = 0xff;
        sa.sin6_addr.s6_addr[12] = uint8_t(v4 >> 24);
        sa.sin6_addr.s6_addr[13] = uint8_t(v4 >> 16);
        sa.sin6_addr.s6_addr[14] = uint8_t(v4 >> 8);
        sa.sin6_addr.s6_addr[15] = uint8_t(v4);
    } else if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR a = addr.toIPv6Address();
        std::memcpy(sa.sin6_addr.s6_addr, a.c, 16);
        sa.sin6_scope_id = addr.scopeId().toUInt();
    } else {
        return false;
    }
    std::memcpy(remote_, &sa, sizeof(sa));
    remoteLen_ = sizeof(sa);
    return true;
}

void MediaTransport::setHandler(uint8_t streamId, Handler h)
{
    handlers_[streamId] = std::move(h);
}

void MediaTransport::setFeedbackHandler(uint8_t streamId, Handler h)
{
    feedbackHandlers_[streamId] = std::move(h);
}

bool MediaTransport::start()
{
    if (fd_ < 0 || running_) return false;
    running_ = true;
    thread_ = std::thread([this] { recvLoop(); });
    return true;
}

void MediaTransport::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool MediaTransport::send(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len)
{
    if (fd_ < 0 || remoteLen_ == 0 || len > kMaxPayload) return false;

    uint8_t pkt[kHeaderSize + kMaxPayload + kTagSize];
    const uint64_t pn = nextPn_.fetch_add(1, std::memory_order_relaxed);
    pkt[0] = uint8_t((kVersion << 6) | (type & 0x3f));
    pkt[1] = streamId;
    pkt[2] = pkt[3] = 0;
    putU64(pkt + 4, pn);

    unsigned char nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];
    makeNonce(pn, nonce);
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(pkt + kHeaderSize, &clen, payload, len, pkt, kHeaderSize,
                                              nullptr, nonce, tx_.data());
    const size_t total = kHeaderSize + size_t(clen);

    if (simLoss_ > 0 && simulateDrop()) {
        st_.simDropped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const ssize_t n = ::sendto(fd_, pkt, total, 0, reinterpret_cast<const sockaddr*>(remote_), remoteLen_);
    if (n != ssize_t(total)) {
        st_.sendErrors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    st_.packetsSent.fetch_add(1, std::memory_order_relaxed);
    st_.bytesSent.fetch_add(total, std::memory_order_relaxed);
    return true;
}

void MediaTransport::recvLoop()
{
    uint8_t pkt[2048];
    uint8_t plain[2048];
    pollfd pfd{fd_, POLLIN, 0};
    while (running_.load(std::memory_order_relaxed)) {
        if (::poll(&pfd, 1, 50) <= 0) continue;
        for (;;) {
            const ssize_t n = ::recv(fd_, pkt, sizeof(pkt), 0);
            if (n < 0) break; // EAGAIN or transient error
            const int64_t arrival = nowUs();
            if (size_t(n) < kHeaderSize + kTagSize || (pkt[0] >> 6) != kVersion) {
                st_.malformed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const uint64_t pn = getU64(pkt + 4);
            if (!replay_.check(pn)) {
                st_.replays.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            unsigned char nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];
            makeNonce(pn, nonce);
            unsigned long long plen = 0;
            if (crypto_aead_chacha20poly1305_ietf_decrypt(plain, &plen, nullptr, pkt + kHeaderSize,
                                                          size_t(n) - kHeaderSize, pkt, kHeaderSize, nonce,
                                                          rx_.data()) != 0) {
                st_.authFailures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            replay_.mark(pn);
            if (rxFirstPn_.load(std::memory_order_relaxed) == 0) rxFirstPn_.store(pn, std::memory_order_relaxed);
            if (pn > rxHighestPn_.load(std::memory_order_relaxed)) rxHighestPn_.store(pn, std::memory_order_relaxed);
            st_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
            st_.bytesReceived.fetch_add(uint64_t(n), std::memory_order_relaxed);
            const uint8_t type = pkt[0] & 0x3f;
            const uint8_t stream = pkt[1];
            if (type == kMedia && handlers_[stream])
                handlers_[stream](plain, size_t(plen), arrival);
            else if (type == kFeedback && feedbackHandlers_[stream])
                feedbackHandlers_[stream](plain, size_t(plen), arrival);
        }
    }
}

MediaTransport::Stats MediaTransport::stats() const
{
    auto r = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    const uint64_t first = r(rxFirstPn_), highest = r(rxHighestPn_);
    return {r(st_.packetsSent),     r(st_.bytesSent),    r(st_.sendErrors), r(st_.simDropped),
            r(st_.packetsReceived), r(st_.bytesReceived), r(st_.authFailures), r(st_.replays),
            r(st_.malformed),       first ? highest - first + 1 : 0};
}

bool MediaTransport::simulateDrop()
{
    auto uniform = [] { return double(randombytes_uniform(1'000'000)) / 1e6; };
    if (simBurst_ <= 1.0) return uniform() < simLoss_;
    // Bad state drops everything and lasts |simBurst_| packets on average;
    // the good->bad rate is chosen so the long-run loss equals simLoss_.
    std::lock_guard l(simMutex_);
    const double toGood = 1.0 / simBurst_;
    const double toBad = simLoss_ >= 1.0 ? 1.0 : toGood * simLoss_ / (1.0 - simLoss_);
    simBad_ = simBad_ ? uniform() >= toGood : uniform() < toBad;
    return simBad_;
}

} // namespace p2p::net
