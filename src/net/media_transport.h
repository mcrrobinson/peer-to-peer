#pragma once

#include "net/crypto.h"

#include <QHostAddress>
#include <QString>

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <thread>

namespace p2p::net {

// Wire format of every media datagram:
//
//   0      u8   (version << 6) | type
//   1      u8   stream id
//   2..3   u16  reserved (0)
//   4..11  u64  packet number, big-endian (transport-wide, monotonically increasing)
//   12..   ChaCha20-Poly1305 ciphertext of the stream payload; AD = bytes 0..11,
//          nonce = packet number. Keys are per direction, derived from the
//          control-channel session keys, so nonces never repeat.
//
// The packet number doubles as the transport-wide sequence number for
// congestion-control feedback (phase 6).
namespace wire {
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kMaxPayload = 1400; // keeps datagrams under a 1500-byte MTU over IPv6
enum Type : uint8_t { kMedia = 1, kFeedback = 2 };
enum Stream : uint8_t { kAudio = 0, kCamera = 1, kScreen = 2 };
} // namespace wire

// Sliding anti-replay window (accepts reordering up to kSize packets).
class ReplayWindow {
public:
    static constexpr uint64_t kSize = 1024;
    bool check(uint64_t pn) const;   // true if pn is acceptable (not seen, not too old)
    void mark(uint64_t pn);          // call only after the packet authenticated
private:
    bool any_ = false;
    uint64_t highest_ = 0;
    std::bitset<kSize> seen_;
};

// Encrypted UDP transport for media. A dedicated receive thread delivers
// packets to per-stream handlers; send() is thread-safe and non-blocking.
// Uses plain BSD sockets so audio/video threads can send without touching
// the Qt event loop.
class MediaTransport {
public:
    using Handler = std::function<void(const uint8_t* payload, size_t len, int64_t arrivalUs)>;

    struct Stats {
        uint64_t packetsSent = 0, bytesSent = 0, sendErrors = 0, simDropped = 0;
        uint64_t packetsReceived = 0, bytesReceived = 0, authFailures = 0, replays = 0, malformed = 0;
    };

    MediaTransport();
    ~MediaTransport();
    MediaTransport(const MediaTransport&) = delete;
    MediaTransport& operator=(const MediaTransport&) = delete;

    bool open(uint16_t port, QString* error);
    uint16_t localPort() const { return localPort_; }

    // Configuration — call before start().
    void setKeys(const crypto::Key& tx, const crypto::Key& rx);
    bool setRemote(const QHostAddress& addr, uint16_t port);
    void setHandler(uint8_t streamId, Handler h);
    // Feedback packets (picture-loss indications, later NACK/CC reports).
    void setFeedbackHandler(uint8_t streamId, Handler h);
    void setSimulatedLoss(double fraction) { simLoss_ = fraction; }

    bool start();
    void stop();

    bool send(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len);
    Stats stats() const;

private:
    void recvLoop();

    int fd_ = -1;
    uint16_t localPort_ = 0;
    crypto::Key tx_{}, rx_{};
    alignas(8) unsigned char remote_[128]{}; // sockaddr_in6
    unsigned remoteLen_ = 0;
    std::array<Handler, 256> handlers_;
    std::array<Handler, 256> feedbackHandlers_;
    std::atomic<uint64_t> nextPn_{1};
    ReplayWindow replay_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    double simLoss_ = 0.0;

    struct AtomicStats {
        std::atomic<uint64_t> packetsSent{0}, bytesSent{0}, sendErrors{0}, simDropped{0};
        std::atomic<uint64_t> packetsReceived{0}, bytesReceived{0}, authFailures{0}, replays{0}, malformed{0};
    } st_;
};

} // namespace p2p::net
