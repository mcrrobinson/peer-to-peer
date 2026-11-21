#include "net/crypto.h"

#include <sodium.h>

#include <cstring>

namespace p2p::crypto {

namespace {

void makeNonce(uint64_t counter, unsigned char nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES])
{
    std::memset(nonce, 0, crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    for (int i = 0; i < 8; ++i)
        nonce[i] = static_cast<unsigned char>(counter >> (8 * i));
}

const unsigned char* bytes(const QByteArray& b)
{
    return reinterpret_cast<const unsigned char*>(b.constData());
}

} // namespace

bool init()
{
    return sodium_init() >= 0;
}

KeyPair KeyPair::generate()
{
    KeyPair kp;
    crypto_kx_keypair(kp.pk.data(), kp.sk.data());
    return kp;
}

QByteArray KeyPair::publicKey() const
{
    return QByteArray(reinterpret_cast<const char*>(pk.data()), int(pk.size()));
}

QByteArray commitment(const QByteArray& publicKey)
{
    static constexpr char kCtx[] = "p2pcall-commit-v1";
    QByteArray out(crypto_generichash_BYTES, '\0');
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, size_t(out.size()));
    crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kCtx), sizeof(kCtx) - 1);
    crypto_generichash_update(&st, bytes(publicKey), size_t(publicKey.size()));
    crypto_generichash_final(&st, reinterpret_cast<unsigned char*>(out.data()), size_t(out.size()));
    return out;
}

std::optional<SessionKeys> deriveSession(bool isInitiator, const KeyPair& local,
                                         const QByteArray& remotePublicKey)
{
    if (remotePublicKey.size() != crypto_kx_PUBLICKEYBYTES)
        return std::nullopt;

    SessionKeys keys;
    const unsigned char* rpk = bytes(remotePublicKey);
    const int rc = isInitiator
        ? crypto_kx_client_session_keys(keys.rx.data(), keys.tx.data(), local.pk.data(), local.sk.data(), rpk)
        : crypto_kx_server_session_keys(keys.rx.data(), keys.tx.data(), local.pk.data(), local.sk.data(), rpk);
    if (rc != 0)
        return std::nullopt;

    // SAS over initiator_pk || responder_pk (same order on both sides).
    const unsigned char* ipk = isInitiator ? local.pk.data() : rpk;
    const unsigned char* rspk = isInitiator ? rpk : local.pk.data();
    static constexpr char kCtx[] = "p2pcall-sas-v1";
    unsigned char h[8];
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, sizeof(h));
    crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kCtx), sizeof(kCtx) - 1);
    crypto_generichash_update(&st, ipk, crypto_kx_PUBLICKEYBYTES);
    crypto_generichash_update(&st, rspk, crypto_kx_PUBLICKEYBYTES);
    crypto_generichash_final(&st, h, sizeof(h));
    uint64_t v = 0;
    std::memcpy(&v, h, sizeof(v));
    v %= 1000000;
    keys.sas = QStringLiteral("%1 %2")
                   .arg(uint(v / 1000), 3, 10, QLatin1Char('0'))
                   .arg(uint(v % 1000), 3, 10, QLatin1Char('0'));
    return keys;
}

Key deriveSubkey(const Key& master, uint64_t id, const char context[8])
{
    static_assert(crypto_kdf_KEYBYTES == 32);
    Key out{};
    crypto_kdf_derive_from_key(out.data(), out.size(), id, context, master.data());
    return out;
}

QByteArray AeadStream::seal(const QByteArray& plaintext)
{
    unsigned char nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];
    makeNonce(txCounter_++, nonce);
    QByteArray out(plaintext.size() + int(crypto_aead_chacha20poly1305_IETF_ABYTES), '\0');
    unsigned long long outLen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(out.data()), &outLen,
                                              bytes(plaintext), size_t(plaintext.size()),
                                              nullptr, 0, nullptr, nonce, tx_.data());
    out.resize(int(outLen));
    return out;
}

std::optional<QByteArray> AeadStream::open(const QByteArray& ciphertext)
{
    if (ciphertext.size() < int(crypto_aead_chacha20poly1305_IETF_ABYTES))
        return std::nullopt;
    unsigned char nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];
    makeNonce(rxCounter_, nonce);
    QByteArray out(ciphertext.size() - int(crypto_aead_chacha20poly1305_IETF_ABYTES), '\0');
    unsigned long long outLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(out.data()), &outLen,
                                                  nullptr, bytes(ciphertext), size_t(ciphertext.size()),
                                                  nullptr, 0, nonce, rx_.data()) != 0)
        return std::nullopt;
    ++rxCounter_;
    out.resize(int(outLen));
    return out;
}

} // namespace p2p::crypto
