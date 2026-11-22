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

#include <algorithm>
#include <cerrno>
#include <cstring>

Q_LOGGING_CATEGORY(lcMedia, "p2p.transport")

namespace p2p::net {

using namespace wire;

namespace {
constexpr uint8_t kDscpEf = 0xB8;
constexpr uint8_t kEct1 = 1;
constexpr uint8_t kCe = 3;
constexpr size_t kIpUdpOverhead = 28;
constexpr int64_t kFeedbackIntervalUs = 10'000;
constexpr size_t kFeedbackEarlyPackets = 200;
constexpr int64_t kPacerBurstUs = 1'000; // credit a late pacer may catch up on
constexpr int64_t kPacerMaxAgeUs = 200'000;

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
    // DSCP EF + ECT(1) (L4S) on everything; we read the ECN field of what
    // arrives for the peer's congestion controller. Failures are harmless.
    int tos = kDscpEf | kEct1;
    ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
    int on = 1;
    ::setsockopt(fd_, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on));
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof(on));

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
        remoteV4_ = true;
    } else if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR a = addr.toIPv6Address();
        std::memcpy(sa.sin6_addr.s6_addr, a.c, 16);
        sa.sin6_scope_id = addr.scopeId().toUInt();
        remoteV4_ = false;
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

void MediaTransport::setCongestionControl(const cc::CongestionController::Config& cfg, bool enabled)
{
    std::lock_guard l(ccMutex_);
    cc_ = cc::CongestionController(cfg);
    ccEnabled_ = enabled;
}

bool MediaTransport::start()
{
    if (fd_ < 0 || running_) return false;
    running_ = true;
    thread_ = std::thread([this] { recvLoop(); });
    pacerThread_ = std::thread([this] { pacerLoop(); });
    if (linkSim_.active()) simThread_ = std::thread([this] { simLoop(); });
    return true;
}

void MediaTransport::stop()
{
    running_ = false;
    pacerCv_.notify_all();
    simCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (pacerThread_.joinable()) pacerThread_.join();
    if (simThread_.joinable()) simThread_.join();
}

bool MediaTransport::send(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len)
{
    return transmit(type, streamId, payload, len);
}

bool MediaTransport::sendPaced(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len)
{
    if (len > kMaxPayload) return false;
    if (!ccEnabled_) return transmit(type, streamId, payload, len);
    {
        std::unique_lock l(pacerMutex_);
        // Keep order: once something is queued, everything queues behind it.
        if (pacerQueue_.empty() && pacingBps_.load(std::memory_order_relaxed) == 0) {
            l.unlock();
            return transmit(type, streamId, payload, len);
        }
        Queued& q = pacerQueue_.emplace_back();
        q.queuedUs = nowUs();
        q.type = type;
        q.stream = streamId;
        q.len = uint16_t(len);
        std::memcpy(q.data, payload, len);
        pacerBytes_ += len;
    }
    pacerCv_.notify_one();
    return true;
}

void MediaTransport::pacerLoop()
{
    Queued q;
    while (running_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock l(pacerMutex_);
            pacerCv_.wait_for(l, std::chrono::milliseconds(50), [&] { return !pacerQueue_.empty() || !running_; });
            if (pacerQueue_.empty()) continue;
            q = pacerQueue_.front();
            pacerQueue_.pop_front();
            pacerBytes_ -= q.len;
        }
        if (nowUs() - q.queuedUs > kPacerMaxAgeUs) {
            st_.pacerDropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const int64_t rate = pacingBps_.load(std::memory_order_relaxed);
        if (rate > 0) {
            const int64_t now = nowUs();
            if (pacerNextUs_ > now) std::this_thread::sleep_for(std::chrono::microseconds(pacerNextUs_ - now));
            pacerNextUs_ = std::max(pacerNextUs_, now - kPacerBurstUs) +
                           int64_t(double(q.len + kHeaderSize + kTagSize + kIpUdpOverhead) * 8e6 / double(rate));
        }
        transmit(q.type, q.stream, q.data, q.len);
    }
}

int64_t MediaTransport::pacerQueueUs() const
{
    const int64_t rate = pacingBps_.load(std::memory_order_relaxed);
    std::lock_guard l(pacerMutex_);
    if (rate <= 0 || pacerBytes_ == 0) return 0;
    return int64_t(double(pacerBytes_) * 8e6 / double(rate));
}

bool MediaTransport::transmit(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len)
{
    if (fd_ < 0 || remoteLen_ == 0 || len > kMaxPayload) return false;

    uint8_t pkt[kMaxDatagram];
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

    if (type == kMedia) {
        std::lock_guard l(ccMutex_);
        cc_.onPacketSent(pn, nowUs(), total + kIpUdpOverhead, streamId);
    }

    if (simLoss_ > 0 && simulateDrop()) {
        st_.simDropped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (linkSim_.active()) {
        {
            std::lock_guard l(simLinkMutex_);
            const int64_t now = nowUs();
            const int64_t start = std::max(now, simLinkFreeUs_);
            const int64_t queued = start - now;
            if (linkSim_.rateBps > 0 && queued > linkSim_.queueUs) {
                st_.simQueueDrops.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (linkSim_.rateBps > 0)
                simLinkFreeUs_ = start + int64_t(double(total + kIpUdpOverhead) * 8e6 / double(linkSim_.rateBps));
            else
                simLinkFreeUs_ = now;
            SimPacket& sp = simQueue_.emplace_back();
            sp.atUs = simLinkFreeUs_ + linkSim_.delayUs;
            // Like DualPI2's step marking, never mark below ~2 packets of queue.
            const int64_t twoPackets =
                linkSim_.rateBps > 0 ? int64_t(2 * double(kMaxDatagram + kIpUdpOverhead) * 8e6 / double(linkSim_.rateBps)) : 0;
            sp.ecn = linkSim_.ceThresholdUs > 0 && queued > std::max(linkSim_.ceThresholdUs, twoPackets) ? kCe : kEct1;
            sp.len = uint16_t(total);
            std::memcpy(sp.data, pkt, total);
        }
        simCv_.notify_one();
        st_.packetsSent.fetch_add(1, std::memory_order_relaxed);
        st_.bytesSent.fetch_add(total, std::memory_order_relaxed);
        return true;
    }

    if (!sendDatagram(pkt, total, -1)) {
        st_.sendErrors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    st_.packetsSent.fetch_add(1, std::memory_order_relaxed);
    st_.bytesSent.fetch_add(total, std::memory_order_relaxed);
    return true;
}

bool MediaTransport::sendDatagram(const uint8_t* pkt, size_t len, int ecn)
{
    if (ecn < 0)
        return ::sendto(fd_, pkt, len, 0, reinterpret_cast<const sockaddr*>(remote_), remoteLen_) == ssize_t(len);
    iovec iov{const_cast<uint8_t*>(pkt), len};
    msghdr m{};
    m.msg_name = remote_;
    m.msg_namelen = remoteLen_;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    alignas(cmsghdr) unsigned char cbuf[CMSG_SPACE(sizeof(int))];
    m.msg_control = cbuf;
    m.msg_controllen = sizeof(cbuf);
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = remoteV4_ ? IPPROTO_IP : IPPROTO_IPV6;
    c->cmsg_type = remoteV4_ ? IP_TOS : IPV6_TCLASS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    const int tos = kDscpEf | (ecn & 3);
    std::memcpy(CMSG_DATA(c), &tos, sizeof(tos));
    return ::sendmsg(fd_, &m, 0) == ssize_t(len);
}

void MediaTransport::simLoop()
{
    SimPacket p;
    while (running_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock l(simLinkMutex_);
            simCv_.wait_for(l, std::chrono::milliseconds(50), [&] { return !simQueue_.empty() || !running_; });
            if (simQueue_.empty()) continue;
            p = simQueue_.front();
            simQueue_.pop_front();
        }
        const int64_t wait = p.atUs - nowUs();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds(wait));
        if (!sendDatagram(p.data, p.len, p.ecn)) st_.sendErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void MediaTransport::refreshPacing(int64_t now)
{
    // ccMutex_ held.
    pacingBps_.store(ccEnabled_ ? cc_.pacingBps(now) : 0, std::memory_order_relaxed);
}

void MediaTransport::maybeSendFeedback(int64_t now)
{
    if (!feedback_.hasNew()) return;
    if (now - lastFeedbackUs_ < kFeedbackIntervalUs && feedback_.newCount() < kFeedbackEarlyPackets) return;
    auto fb = feedback_.build(now, cc::TransportFeedback::maxEntries(kMaxPayload));
    lastFeedbackUs_ = now;
    if (!fb) return;
    uint8_t buf[kMaxPayload];
    const size_t n = fb->serialize(buf, sizeof(buf));
    if (n && transmit(kTransportFeedback, 0, buf, n)) st_.feedbackSent.fetch_add(1, std::memory_order_relaxed);
}

int64_t MediaTransport::targetBps() const
{
    std::lock_guard l(ccMutex_);
    return cc_.targetBps();
}

int64_t MediaTransport::streamBps(uint8_t streamId) const
{
    std::lock_guard l(ccMutex_);
    return cc_.streamBps(streamId);
}

cc::CongestionController::Stats MediaTransport::ccStats() const
{
    std::lock_guard l(ccMutex_);
    return cc_.stats(nowUs());
}

void MediaTransport::recvLoop()
{
    uint8_t pkt[2048];
    uint8_t plain[2048];
    alignas(cmsghdr) unsigned char cbuf[256];
    pollfd pfd{fd_, POLLIN, 0};
    while (running_.load(std::memory_order_relaxed)) {
        int timeoutMs = 50;
        if (feedback_.hasNew())
            timeoutMs = int(std::clamp<int64_t>((lastFeedbackUs_ + kFeedbackIntervalUs - nowUs() + 999) / 1000, 0, 50));
        if (::poll(&pfd, 1, timeoutMs) <= 0) {
            maybeSendFeedback(nowUs());
            continue;
        }
        for (;;) {
            iovec iov{pkt, sizeof(pkt)};
            msghdr m{};
            m.msg_iov = &iov;
            m.msg_iovlen = 1;
            m.msg_control = cbuf;
            m.msg_controllen = sizeof(cbuf);
            const ssize_t n = ::recvmsg(fd_, &m, 0);
            if (n < 0) break; // EAGAIN or transient error
            const int64_t arrival = nowUs();
            uint8_t ecn = 0;
            for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                // IP_TOS arrives as one byte, IPV6_TCLASS as an int.
                if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS) {
                    ecn = *CMSG_DATA(c) & 3;
                } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS) {
                    int v = 0;
                    std::memcpy(&v, CMSG_DATA(c), sizeof(v));
                    ecn = uint8_t(v & 3);
                }
            }
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
            if (ecn == kCe) st_.ceReceived.fetch_add(1, std::memory_order_relaxed);
            const uint8_t type = pkt[0] & 0x3f;
            const uint8_t stream = pkt[1];
            if (type == kTransportFeedback) {
                st_.feedbackReceived.fetch_add(1, std::memory_order_relaxed);
                if (auto fb = cc::TransportFeedback::parse(plain, size_t(plen))) {
                    std::lock_guard l(ccMutex_);
                    cc_.onFeedback(*fb, arrival);
                    refreshPacing(arrival);
                }
                continue;
            }
            feedback_.onPacket(pn, arrival, ecn);
            if (type == kMedia && handlers_[stream])
                handlers_[stream](plain, size_t(plen), arrival);
            else if (type == kFeedback && feedbackHandlers_[stream])
                feedbackHandlers_[stream](plain, size_t(plen), arrival);
        }
        maybeSendFeedback(nowUs());
        // The pacing hold expires with time, not only with feedback.
        std::lock_guard l(ccMutex_);
        refreshPacing(nowUs());
    }
}

MediaTransport::Stats MediaTransport::stats() const
{
    auto r = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    const uint64_t first = r(rxFirstPn_), highest = r(rxHighestPn_);
    return {r(st_.packetsSent),     r(st_.bytesSent),    r(st_.sendErrors), r(st_.simDropped),
            r(st_.packetsReceived), r(st_.bytesReceived), r(st_.authFailures), r(st_.replays),
            r(st_.malformed),       first ? highest - first + 1 : 0,
            r(st_.feedbackSent),    r(st_.feedbackReceived), r(st_.ceReceived), r(st_.simQueueDrops),
            r(st_.pacerDropped)};
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
