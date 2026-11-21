#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>

// Thin wrappers over libsodium.
//
// Handshake (commit/reveal, ZRTP-style, so a MITM cannot grind keys to make
// the short authentication strings match):
//   initiator -> responder : commit = H(pk_i)
//   responder -> initiator : pk_r
//   initiator -> responder : pk_i          (responder checks H(pk_i) == commit)
// Both sides then derive directional session keys with X25519 (crypto_kx)
// and a 6-digit SAS over (pk_i || pk_r) that users can compare verbally.
namespace p2p::crypto {

using Key = std::array<uint8_t, 32>;

bool init();

struct KeyPair {
    Key pk{};
    Key sk{};
    static KeyPair generate();
    QByteArray publicKey() const;
};

QByteArray commitment(const QByteArray& publicKey);

struct SessionKeys {
    Key rx{};   // key for data we receive
    Key tx{};   // key for data we send
    QString sas;
};

std::optional<SessionKeys> deriveSession(bool isInitiator, const KeyPair& local,
                                         const QByteArray& remotePublicKey);

// Derive an independent subkey (e.g. for the UDP media transport).
Key deriveSubkey(const Key& master, uint64_t id, const char context[8]);

// Ordered AEAD stream (ChaCha20-Poly1305 IETF) with implicit counter nonces.
// Suitable for a reliable, in-order transport (the TCP control channel).
class AeadStream {
public:
    AeadStream() = default;
    AeadStream(const Key& tx, const Key& rx) : tx_(tx), rx_(rx) {}

    QByteArray seal(const QByteArray& plaintext);
    std::optional<QByteArray> open(const QByteArray& ciphertext);

private:
    Key tx_{}, rx_{};
    uint64_t txCounter_ = 0;
    uint64_t rxCounter_ = 0;
};

} // namespace p2p::crypto
