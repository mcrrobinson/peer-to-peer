#include "video/video_packetizer.h"

#include <QTest>

#include <algorithm>
#include <random>

using namespace p2p::video;

namespace {
struct Frag {
    FragmentHeader h;
    std::vector<uint8_t> data;
};

std::vector<uint8_t> payload(size_t n, uint8_t seed)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7);
    return v;
}

std::vector<Frag> split(uint32_t id, const std::vector<uint8_t>& data, bool key, size_t mtu = 1400,
                        double fecRate = 0, int fecBlock = 32)
{
    std::vector<Frag> out;
    PacketizeOptions opt;
    opt.fecRate = fecRate;
    opt.fecBlockSize = fecBlock;
    packetizeFrame(
        id, int64_t(id) * 1000, key, 1, data.data(), data.size(), mtu,
        [&](const uint8_t* p, size_t n) {
            auto h = FragmentHeader::read(p, n);
            QVERIFY(h);
            out.push_back({*h, std::vector<uint8_t>(p + FragmentHeader::kSize, p + n)});
        },
        opt);
    return out;
}

std::vector<Frag> dataOnly(const std::vector<Frag>& f)
{
    std::vector<Frag> out;
    for (const auto& x : f)
        if (!x.h.isParity()) out.push_back(x);
    return out;
}

void feed(FrameAssembler& a, const std::vector<Frag>& frags, int64_t t = 0)
{
    for (const auto& f : frags) a.insert(f.h, f.data.data(), f.data.size(), t);
}
} // namespace

class TestVideoPacketizer : public QObject {
    Q_OBJECT
private slots:
    void headerRoundTrip()
    {
        FragmentHeader h{0xdeadbeef, 3, 9, 123456789012, FragmentHeader::kKeyframe, 2, 1920, 1080};
        uint8_t b[FragmentHeader::kSize];
        h.write(b);
        auto r = FragmentHeader::read(b, sizeof(b));
        QVERIFY(r);
        QCOMPARE(r->frameId, h.frameId);
        QCOMPARE(r->index, h.index);
        QCOMPARE(r->count, h.count);
        QCOMPARE(r->captureUs, h.captureUs);
        QCOMPARE(r->flags, h.flags);
        QCOMPARE(r->codec, h.codec);
        QCOMPARE(r->width, h.width);
        QCOMPARE(r->height, h.height);
        b[5] = 9; // index == count
        QVERIFY(!FragmentHeader::read(b, sizeof(b)));
        QVERIFY(!FragmentHeader::read(b, 5));
    }

    void fragmentsFitAndAreBalanced()
    {
        const auto data = payload(10000, 1);
        const auto frags = split(1, data, true);
        QCOMPARE(frags.size(), size_t(8)); // ceil(10000 / 1380)
        size_t lo = SIZE_MAX, hi = 0;
        for (const auto& f : frags) {
            QVERIFY(f.data.size() + FragmentHeader::kSize <= 1400);
            lo = std::min(lo, f.data.size());
            hi = std::max(hi, f.data.size());
        }
        QVERIFY(hi - lo <= 1);
    }

    void reassemblesOutOfOrder()
    {
        FrameAssembler a;
        const auto data = payload(50000, 3);
        auto frags = split(10, data, true);
        std::mt19937 rng(42);
        std::shuffle(frags.begin(), frags.end(), rng);
        feed(a, frags);
        auto out = a.poll(0);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].data, data);
        QVERIFY(out[0].keyframe);
        QCOMPARE(out[0].captureUs, int64_t(10000));
    }

    void waitsForKeyframeAtStart()
    {
        FrameAssembler a;
        feed(a, split(5, payload(100, 0), false));
        QVERIFY(a.poll(0).empty());
        QVERIFY(a.needKeyframe());
        feed(a, split(6, payload(100, 0), true));
        auto out = a.poll(0);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].id, 6u);
        QVERIFY(!a.needKeyframe());
    }

    void releasesInDecodeOrder()
    {
        FrameAssembler a;
        feed(a, split(1, payload(3000, 1), true));
        feed(a, split(3, payload(3000, 3), false)); // arrives before 2
        auto out = a.poll(0);
        QCOMPARE(out.size(), size_t(1));
        feed(a, split(2, payload(3000, 2), false));
        out = a.poll(1000);
        QCOMPARE(out.size(), size_t(2));
        QCOMPARE(out[0].id, 2u);
        QCOMPARE(out[1].id, 3u);
    }

    void lostFrameRequestsKeyframeAfterGrace()
    {
        FrameAssembler a;
        feed(a, split(1, payload(3000, 1), true));
        QCOMPARE(a.poll(0).size(), size_t(1));
        auto f2 = split(2, payload(3000, 2), false);
        f2.pop_back(); // frame 2 incomplete for good
        feed(a, f2);
        feed(a, split(3, payload(3000, 3), false), 1000);
        QVERIFY(a.poll(1000).empty());   // inside grace period
        QVERIFY(!a.needKeyframe());
        QVERIFY(a.poll(25'000).empty()); // grace expired: frame 2 lost, 3 undecodable
        QVERIFY(a.needKeyframe());
        QCOMPARE(a.stats().framesLost, 1u);

        feed(a, split(4, payload(3000, 4), true), 30'000);
        auto out = a.poll(30'000);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].id, 4u);
    }

    void neverResyncsOnOldFramesAfterLoss()
    {
        // Frames 1 (key) .. 3 shown, 4 lost: while waiting for a keyframe, a
        // late copy of keyframe 1 (a slow resend) must not be shown again,
        // nor NACKed.
        FrameAssembler a;
        auto f1 = split(1, payload(3000, 1), true);
        feed(a, f1);
        feed(a, split(2, payload(3000, 2), false));
        feed(a, split(3, payload(3000, 3), false));
        QCOMPARE(a.poll(0).size(), size_t(3));
        feed(a, split(5, payload(3000, 5), false), 1000);
        a.poll(1000);
        QVERIFY(a.poll(100'000).empty()); // 4 declared lost
        QVERIFY(a.needKeyframe());
        feed(a, {f1[0]}, 100'000);
        feed(a, f1, 100'000);
        QVERIFY(a.poll(100'000).empty());
        QVERIFY(a.collectNacks(150'000, {}).empty());

        feed(a, split(6, payload(3000, 6), true), 200'000);
        auto out = a.poll(200'000);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].id, 6u);
    }

    void laterKeyframeSkipsGapImmediately()
    {
        FrameAssembler a;
        feed(a, split(1, payload(100, 1), true));
        a.poll(0);
        feed(a, split(5, payload(100, 5), true)); // 2..4 never arrive
        auto out = a.poll(0);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].id, 5u);
        QCOMPARE(a.stats().framesLost, 3u);
    }

    void lateFragmentsIgnored()
    {
        FrameAssembler a;
        auto f1 = split(1, payload(100, 1), true);
        feed(a, f1);
        a.poll(0);
        feed(a, f1);
        QCOMPARE(a.stats().lateFragments, f1.size());
        QVERIFY(a.poll(0).empty());
    }

    void fecRebuildsLostFragments()
    {
        // 100 kB keyframe, 25% parity in blocks of 16: lose 3 fragments in
        // one block and 1 in another (including the short last fragment).
        const auto data = payload(100'000, 9);
        auto frags = split(1, data, true, 1400, 0.25, 16);
        const size_t nData = dataOnly(frags).size();
        QCOMPARE(nData, size_t(73));
        QCOMPARE(frags.size() - nData, size_t(5 * 4)); // 5 blocks x ceil(16 * .25)
        std::vector<Frag> kept;
        for (const auto& f : frags)
            if (f.h.index != 0 && f.h.index != 5 && f.h.index != 15 && f.h.index != nData - 1) kept.push_back(f);
        FrameAssembler a;
        feed(a, kept);
        auto out = a.poll(0);
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].data, data);
        QVERIFY(out[0].recovered);
        QCOMPARE(a.stats().fecRecoveredFragments, 4u);
        QCOMPARE(a.stats().fecRecoveredFrames, 1u);
    }

    void fecGivesUpBeyondParity()
    {
        auto frags = split(1, payload(20'000, 1), true, 1400, 0.1, 32); // 15 data + 2 parity
        std::vector<Frag> kept;
        for (const auto& f : frags)
            if (f.h.index > 2) kept.push_back(f); // 3 data lost, 2 parity
        FrameAssembler a;
        feed(a, kept);
        QVERIFY(a.poll(0).empty());
        // Only the shortfall is requested: 3 missing - 2 parity = 1.
        auto n = a.collectNacks(5'000, {});
        QCOMPARE(n.size(), size_t(1));
        QCOMPARE(n[0].index, uint16_t(0));
    }

    void nackRequestsHolesThenRetries()
    {
        FrameAssembler a;
        feed(a, split(1, payload(3000, 1), true));
        QCOMPARE(a.poll(0).size(), size_t(1));
        auto f2 = split(2, payload(5000, 2), false); // 4 fragments
        f2.erase(f2.begin() + 1);
        feed(a, f2, 1000);
        FrameAssembler::NackPolicy pol;
        QVERIFY(a.collectNacks(1500, pol).empty()); // still inside reorder window
        auto n = a.collectNacks(3500, pol);
        QCOMPARE(n.size(), size_t(1));
        QCOMPARE(n[0].frameId, 2u);
        QCOMPARE(n[0].index, uint16_t(1));
        QVERIFY(a.collectNacks(5000, pol).empty());  // retry interval not over
        QCOMPARE(a.collectNacks(14000, pol).size(), size_t(1));
        QCOMPARE(a.collectNacks(25000, pol).size(), size_t(1));
        QVERIFY(a.collectNacks(40000, pol).empty()); // max tries reached

        // The resend completes the frame.
        auto again = split(2, payload(5000, 2), false);
        again[1].h.flags |= FragmentHeader::kRetransmit;
        a.insert(again[1].h, again[1].data.data(), again[1].data.size(), 41000);
        auto out = a.poll(41000);
        QCOMPARE(out.size(), size_t(1));
        QVERIFY(out[0].recovered);
        QCOMPARE(a.stats().retransmitsUseful, 1u);
    }

    void nackWholeMissingFrame()
    {
        FrameAssembler a;
        feed(a, split(1, payload(3000, 1), true));
        a.poll(0);
        feed(a, split(3, payload(3000, 3), false), 1000); // 2 vanished entirely
        QVERIFY(a.poll(1000).empty());
        QVERIFY(a.collectNacks(1000, {}).empty());
        auto n = a.collectNacks(4000, {});
        QCOMPARE(n.size(), size_t(1));
        QCOMPARE(n[0].frameId, 2u);
        QCOMPARE(n[0].index, NackItem::kWholeFrame);
        feed(a, split(2, payload(3000, 2), false), 4500);
        auto out = a.poll(4500);
        QCOMPARE(out.size(), size_t(2));
        QCOMPARE(out[0].id, 2u);
        QCOMPARE(out[1].id, 3u);
    }

    void frameIdWrapAround()
    {
        FrameAssembler a;
        feed(a, split(0xffffffffu, payload(100, 1), true));
        feed(a, split(0u, payload(100, 2), false));
        auto out = a.poll(0);
        QCOMPARE(out.size(), size_t(2));
        QCOMPARE(out[1].id, 0u);
    }
};

QTEST_GUILESS_MAIN(TestVideoPacketizer)
#include "test_video_packetizer.moc"
