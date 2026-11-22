#include "net/fec.h"

#include <QtTest>

#include <random>

using namespace p2p::net;

class TestFec : public QObject {
    Q_OBJECT

    static std::vector<std::vector<uint8_t>> randomShards(int k, size_t len, std::mt19937& rng)
    {
        std::vector<std::vector<uint8_t>> d(static_cast<size_t>(k), std::vector<uint8_t>(len));
        for (auto& s : d)
            for (auto& b : s) b = uint8_t(rng());
        return d;
    }

    // Encodes, erases |lost| shard indices, decodes and checks the data.
    static bool roundTrip(int k, int m, size_t len, const std::vector<int>& lost, std::mt19937& rng)
    {
        auto data = randomShards(k, len, rng);
        std::vector<const uint8_t*> ptrs;
        for (auto& s : data) ptrs.push_back(s.data());
        std::vector<std::vector<uint8_t>> parity;
        fec::encode(ptrs, len, m, parity);
        std::vector<std::vector<uint8_t>> shards = data;
        shards.insert(shards.end(), parity.begin(), parity.end());
        for (int i : lost) shards[size_t(i)].clear();
        if (!fec::decode(k, m, len, shards)) return false;
        for (int i = 0; i < k; ++i)
            if (shards[size_t(i)] != data[size_t(i)]) return false;
        return true;
    }

private slots:
    void noLossIsTrivial()
    {
        std::mt19937 rng(1);
        QVERIFY(roundTrip(10, 2, 100, {}, rng));
    }

    void recoversUpToParityCount()
    {
        std::mt19937 rng(2);
        QVERIFY(roundTrip(10, 3, 64, {0, 5, 9}, rng));     // data only
        QVERIFY(roundTrip(10, 3, 64, {1, 10, 12}, rng));   // data + parity
        QVERIFY(roundTrip(1, 1, 64, {0}, rng));            // single shard
        QVERIFY(roundTrip(48, 8, 1378, {0, 1, 2, 3, 4, 5, 6, 47}, rng));
    }

    void failsBeyondParityCount()
    {
        std::mt19937 rng(3);
        QVERIFY(!roundTrip(10, 2, 64, {0, 1, 2}, rng));
    }

    void randomErasurePatterns()
    {
        std::mt19937 rng(4);
        for (int iter = 0; iter < 300; ++iter) {
            const int k = 1 + int(rng() % 60), m = 1 + int(rng() % 12);
            std::vector<int> idx(size_t(k + m));
            std::iota(idx.begin(), idx.end(), 0);
            std::shuffle(idx.begin(), idx.end(), rng);
            idx.resize(size_t(rng() % (m + 1)));
            QVERIFY2(roundTrip(k, m, 1 + rng() % 200, idx, rng), qPrintable(QStringLiteral("k=%1 m=%2").arg(k).arg(m)));
        }
    }
};

QTEST_GUILESS_MAIN(TestFec)
#include "test_fec.moc"
