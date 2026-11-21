#include "net/crypto.h"

#include <QTest>

using namespace p2p::crypto;

class TestCrypto : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(init()); }

    void sessionKeysMatch()
    {
        const KeyPair a = KeyPair::generate();
        const KeyPair b = KeyPair::generate();
        auto ka = deriveSession(true, a, b.publicKey());
        auto kb = deriveSession(false, b, a.publicKey());
        QVERIFY(ka && kb);
        QCOMPARE(ka->tx, kb->rx);
        QCOMPARE(ka->rx, kb->tx);
        QVERIFY(ka->tx != ka->rx);
        QCOMPARE(ka->sas, kb->sas);
        QCOMPARE(ka->sas.size(), 7); // "123 456"
    }

    void sasDiffersForDifferentKeys()
    {
        const KeyPair a = KeyPair::generate();
        const KeyPair b = KeyPair::generate();
        const KeyPair m = KeyPair::generate();
        QVERIFY(deriveSession(true, a, b.publicKey())->sas != deriveSession(true, a, m.publicKey())->sas);
    }

    void rejectsBadPublicKey()
    {
        QVERIFY(!deriveSession(true, KeyPair::generate(), QByteArray(5, 'x')));
    }

    void commitmentBindsKey()
    {
        const KeyPair a = KeyPair::generate();
        const KeyPair b = KeyPair::generate();
        QCOMPARE(commitment(a.publicKey()), commitment(a.publicKey()));
        QVERIFY(commitment(a.publicKey()) != commitment(b.publicKey()));
    }

    void aeadRoundTripAndTamper()
    {
        const KeyPair a = KeyPair::generate();
        const KeyPair b = KeyPair::generate();
        auto ka = *deriveSession(true, a, b.publicKey());
        auto kb = *deriveSession(false, b, a.publicKey());
        AeadStream sa(ka.tx, ka.rx), sb(kb.tx, kb.rx);

        const QByteArray m1 = "hello", m2 = "world";
        const QByteArray c1 = sa.seal(m1), c2 = sa.seal(m2);
        QVERIFY(c1 != c2);
        QCOMPARE(*sb.open(c1), m1);
        QCOMPARE(*sb.open(c2), m2);

        QByteArray c3 = sa.seal("secret");
        c3[0] = char(c3[0] ^ 1);
        QVERIFY(!sb.open(c3));

        // Replay of an earlier frame fails (counter nonce has moved on).
        AeadStream sb2(kb.tx, kb.rx);
        QVERIFY(sb2.open(c1));
        QVERIFY(!sb2.open(c1));
    }

    void subkeysAreIndependent()
    {
        Key master{};
        master[0] = 1;
        QVERIFY(deriveSubkey(master, 1, "p2pmedia") != deriveSubkey(master, 2, "p2pmedia"));
        QVERIFY(deriveSubkey(master, 1, "p2pmedia") != deriveSubkey(master, 1, "p2pother"));
    }
};

QTEST_GUILESS_MAIN(TestCrypto)
#include "test_crypto.moc"
