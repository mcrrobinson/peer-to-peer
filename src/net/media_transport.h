#pragma once

#include "net/cc/congestion_controller.h"
#include "net/cc/transport_feedback.h"
#include "net/crypto.h"

#include <QHostAddress>
#include <QString>

#include <array>
#include <atomic>
#include <mutex>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <thread>

namespace p2p::net {

// Wire format of every media datagram:
//
//   0      u8   (version << 6) | type
//   1      u8   stream id (unused for transport feedback)
//   2..3   u16  reserved (0)
//   4..11  u64  packet number, big-endian (transport-wide, monotonically increasing)
//   12..   ChaCha20-Poly1305 ciphertext of the stream payload; AD = bytes 0..11,
//          nonce = packet number. Keys are per direction, derived from the
//          control-channel session keys, so nonces never repeat.
//
// The packet number doubles as the transport-wide sequence number for
// congestion control: the receiver reports arrival time and ECN of every
// packet in kTransportFeedback datagrams (see cc/transport_feedback.h),
// which are themselves not reported. Packets carry ECT(1) (L4S) and DSCP EF.
namespace wire {
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kMaxPayload = 1400; // keeps datagrams under a 1500-byte MTU over IPv6
enum Type : uint8_t { kMedia = 1, kFeedback = 2, kTransportFeedback = 3 };
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
// packets to per-stream handlers and sends transport-wide feedback every
// ~10 ms; send() is thread-safe and non-blocking. Uses plain BSD sockets so
// audio/video threads can send without touching the Qt event loop.
//
// Congestion control runs here (it sees every packet): media packets are
// logged on send, the peer's feedback updates the controller, and video
// reads the target rate. sendPaced() goes through a pacer that is only
// active while the controller sees the link as the bottleneck; otherwise
// it sends immediately like send(). Packets that waited longer than 200 ms
// in the pacer are dropped: the receiver has given up (or is about to give
// up) on their frame, and sending them would only add delay.
class MediaTransport {
public:
    using Handler = std::function<void(const uint8_t* payload, size_t len, int64_t arrivalUs)>;

    struct Stats {
        uint64_t packetsSent = 0, bytesSent = 0, sendErrors = 0, simDropped = 0;
        uint64_t packetsReceived = 0, bytesReceived = 0, authFailures = 0, replays = 0, malformed = 0;
        // Packets the peer has sent us so far, judging by packet numbers
        // (highest - first + 1); with packetsReceived this gives path loss.
        uint64_t packetsExpected = 0;
        uint64_t feedbackSent = 0, feedbackReceived = 0, ceReceived = 0, simQueueDrops = 0;
        uint64_t pacerDropped = 0; // waited too long in the pacer to be of use
    };
    // Testing: a bottleneck on our outgoing packets: drop-tail queue drained
    // at |rateBps| (0 = unlimited), then |delayUs| of propagation; packets
    // that waited longer than |ceThresholdUs| (and than 2 full-size packet
    // times) get marked CE (L4S step AQM).
    struct LinkSim {
        int64_t rateBps = 0;
        int64_t delayUs = 0;
        int64_t queueUs = 100'000;
        int64_t ceThresholdUs = 0;
        bool active() const { return rateBps > 0 || delayUs > 0; }
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
    // Testing: drop |fraction| of outgoing packets. |meanBurst| > 1 makes
    // the losses bursty (two-state Gilbert-Elliott model, same average).
    void setSimulatedLoss(double fraction, double meanBurst = 1.0)
    {
        simLoss_ = fraction;
        simBurst_ = meanBurst;
    }

    void setLinkSim(const LinkSim& sim) { linkSim_ = sim; }
    // |enabled| = false keeps measuring (loss for FEC, stats) but never paces.
    void setCongestionControl(const cc::CongestionController::Config& cfg, bool enabled);

    bool start();
    void stop();

    bool send(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len);
    bool sendPaced(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len);
    Stats stats() const;

    bool congestionControlEnabled() const { return ccEnabled_; }
    int64_t targetBps() const;
    int64_t streamBps(uint8_t streamId) const;
    cc::CongestionController::Stats ccStats() const;
    int64_t pacerQueueUs() const; // time to drain what the pacer holds

private:
    static constexpr size_t kMaxDatagram = wire::kHeaderSize + wire::kMaxPayload + wire::kTagSize;
    struct Queued {
        int64_t queuedUs = 0;
        uint8_t type = 0, stream = 0;
        uint16_t len = 0;
        uint8_t data[wire::kMaxPayload];
    };
    struct SimPacket {
        int64_t atUs = 0;
        uint8_t ecn = 0;
        uint16_t len = 0;
        uint8_t data[kMaxDatagram];
    };
    void recvLoop();
    void pacerLoop();
    void simLoop();
    bool transmit(uint8_t type, uint8_t streamId, const uint8_t* payload, size_t len);
    bool sendDatagram(const uint8_t* pkt, size_t len, int ecn); // ecn < 0: socket default
    void maybeSendFeedback(int64_t nowUs);
    void refreshPacing(int64_t nowUs);

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
    bool simulateDrop();
    double simLoss_ = 0.0;
    double simBurst_ = 1.0;
    std::mutex simMutex_;
    bool simBad_ = false;
    std::atomic<uint64_t> rxFirstPn_{0}, rxHighestPn_{0};
    bool remoteV4_ = false;

    // Congestion control (sender side) and feedback (receiver side).
    mutable std::mutex ccMutex_;
    cc::CongestionController cc_;
    bool ccEnabled_ = true;
    std::atomic<int64_t> pacingBps_{0};
    cc::FeedbackBuilder feedback_; // receive thread only
    int64_t lastFeedbackUs_ = 0;

    // Pacer.
    mutable std::mutex pacerMutex_;
    std::condition_variable pacerCv_;
    std::deque<Queued> pacerQueue_;
    size_t pacerBytes_ = 0;
    int64_t pacerNextUs_ = 0;
    std::thread pacerThread_;

    // Link simulation.
    LinkSim linkSim_;
    std::mutex simLinkMutex_;
    std::condition_variable simCv_;
    std::deque<SimPacket> simQueue_;
    int64_t simLinkFreeUs_ = 0;
    std::thread simThread_;

    struct AtomicStats {
        std::atomic<uint64_t> packetsSent{0}, bytesSent{0}, sendErrors{0}, simDropped{0};
        std::atomic<uint64_t> packetsReceived{0}, bytesReceived{0}, authFailures{0}, replays{0}, malformed{0};
        std::atomic<uint64_t> feedbackSent{0}, feedbackReceived{0}, ceReceived{0}, simQueueDrops{0};
        std::atomic<uint64_t> pacerDropped{0};
    } st_;
};

} // namespace p2p::net
