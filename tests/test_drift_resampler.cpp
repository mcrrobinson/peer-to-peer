#include "audio/drift_resampler.h"

#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using p2p::audio::DriftResampler;

namespace {
std::vector<int16_t> sine(int n, double hz, int startSample = 0)
{
    std::vector<int16_t> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v[size_t(i)] = int16_t(10000.0 * std::sin(2.0 * std::numbers::pi * hz * (startSample + i) / 48000.0));
    return v;
}

// Run |seconds| of 10 ms pulls at a fixed ratio, feeding 10 ms input blocks on demand.
struct Run {
    std::vector<int16_t> out;
    int inputBlocks = 0;
};
Run run(double ratio, double seconds, double hz = 500)
{
    DriftResampler r;
    Run res;
    int fed = 0;
    const int pulls = int(seconds * 100);
    for (int p = 0; p < pulls; ++p) {
        while (r.size() < r.required(480, ratio)) {
            auto blk = sine(480, hz, fed);
            r.push(blk.data(), 480);
            fed += 480;
            ++res.inputBlocks;
        }
        int16_t out[480];
        r.pull(out, 480, ratio);
        res.out.insert(res.out.end(), out, out + 480);
    }
    return res;
}
} // namespace

class TestDriftResampler : public QObject {
    Q_OBJECT
private slots:
    void unityRatioIsIdentity()
    {
        DriftResampler r;
        auto in = sine(4800, 500);
        r.push(in.data(), int(in.size()));
        std::vector<int16_t> out(4000);
        QVERIFY(r.size() >= r.required(4000, 1.0));
        r.pull(out.data(), 4000, 1.0);
        // The history sample only feeds interpolation: at ratio 1 output == input.
        for (int i = 0; i < 4000; ++i) QCOMPARE(out[size_t(i)], in[size_t(i)]);
    }

    void consumptionTracksRatio()
    {
        // 10 s of output at +1 % / -1 % should consume ~1 % more / less input.
        const auto fast = run(1.01, 10.0);
        const auto slow = run(0.99, 10.0);
        QVERIFY2(std::abs(fast.inputBlocks - 1010) <= 2, qPrintable(QString::number(fast.inputBlocks)));
        QVERIFY2(std::abs(slow.inputBlocks - 990) <= 2, qPrintable(QString::number(slow.inputBlocks)));
    }

    void outputIsSmooth()
    {
        // At a 2 % correction the output must still be a clean sine: no
        // sample-to-sample jump larger than the sine's own max slope (+margin).
        const auto res = run(1.02, 2.0);
        const double maxSlope = 10000.0 * 2.0 * std::numbers::pi * 500.0 * 1.02 / 48000.0;
        int worst = 0;
        for (size_t i = 1; i < res.out.size(); ++i)
            worst = std::max(worst, std::abs(int(res.out[i]) - int(res.out[i - 1])));
        QVERIFY2(worst < maxSlope * 1.1, qPrintable(QString("%1 vs %2").arg(worst).arg(maxSlope)));
    }

    void bufferedAccounting()
    {
        DriftResampler r;
        auto in = sine(960, 500);
        r.push(in.data(), 960);
        QCOMPARE(r.buffered(), 960.0); // 961 samples incl. history, pos 1
        int16_t out[480];
        r.pull(out, 480, 1.0);
        QCOMPARE(r.buffered(), 480.0);
    }
};

QTEST_GUILESS_MAIN(TestDriftResampler)
#include "test_drift_resampler.moc"
