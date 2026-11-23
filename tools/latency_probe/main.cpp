// Glass-to-glass latency probe.
//
// A window flashes black / white at random intervals. Point a call's camera
// at it. The probe captures the whole screen (xdg-desktop-portal +
// PipeWire, like screen share), splits each capture into tiles and finds
// every tile that follows the flashes: the probe window itself, and the
// flash as it reappears in video windows (the remote view of the call, a
// self view). Both are read from the same screen captures, so display,
// compositor and capture delays cancel: for each video window,
//     glass-to-glass = when it switched - when the probe window switched.
// Works for any app (e.g. a WebRTC call in a browser, for an A/B).
//
// Usage: latency_probe [--seconds 60] [--csv out.csv]
#include "core/clock.h"
#include "video/video_source.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QRasterWindow>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

using namespace p2p;

namespace {

constexpr int kTile = 16;                // capture pixels per tile side
constexpr int64_t kMinGapUs = 500'000;   // between flashes (random in [min, max])
constexpr int64_t kMaxGapUs = 900'000;
constexpr int64_t kMaxLagUs = 800'000;   // longest latency searched for
constexpr int64_t kLagStepUs = 4'000;
constexpr double kMinStd = 12.0;         // tiles that barely change aren't candidates
constexpr double kMinCorr = 0.6;         // correlation with the flash pattern to count as following it
constexpr int kMinTiles = 4;             // smallest region reported

struct Capture {
    int64_t t = 0;
    std::vector<uint8_t> tiles; // mean luma per tile, row-major
};

struct Toggle {
    int64_t t = 0;
    bool white = false;
};

// ------------------------------------------------------------------ window

class FlashWindow : public QRasterWindow {
public:
    bool white = false;
    std::function<void(int64_t, bool)> onPainted;
    bool pendingReport = false;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(QRect(0, 0, width(), height()), white ? Qt::white : Qt::black);
        if (pendingReport && onPainted) {
            pendingReport = false;
            onPainted(nowUs(), white);
        }
    }
};

// ---------------------------------------------------------------- analysis

struct Region {
    std::vector<int> tiles;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; // tile bounds
    double lagUs = 0;                                  // median tile lag (coarse)
    std::vector<double> perFlashUs;                    // NaN when not found for that flash
    double medianUs = NAN;
};

double median(std::vector<double> v)
{
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return std::isnan(x); }), v.end());
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Analysis {
    const std::vector<Capture>& caps;
    const std::vector<Toggle>& toggles;
    int cols, rows;
    std::vector<std::vector<int8_t>> stateByLag; // [lag step][capture]: stateAt(t - lag)

    void precompute()
    {
        for (int64_t d = 0; d <= kMaxLagUs; d += kLagStepUs) {
            std::vector<int8_t> v(caps.size());
            for (size_t k = 0; k < caps.size(); ++k) v[k] = int8_t(stateAt(caps[k].t - d));
            stateByLag.push_back(std::move(v));
        }
    }

    // Flash state (1 white / 0 black) shown at time t, or -1 before the first.
    int stateAt(int64_t t) const
    {
        auto it = std::upper_bound(toggles.begin(), toggles.end(), t,
                                   [](int64_t v, const Toggle& g) { return v < g.t; });
        if (it == toggles.begin()) return -1;
        return std::prev(it)->white ? 1 : 0;
    }

    // Best lag and its correlation for one tile.
    std::pair<int64_t, double> bestLag(int tile) const
    {
        int64_t best = 0;
        double bestR = -1;
        std::vector<double> y(caps.size());
        for (size_t k = 0; k < caps.size(); ++k) y[k] = caps[k].tiles[size_t(tile)];
        for (size_t li = 0; li < stateByLag.size(); ++li) {
            const int64_t d = int64_t(li) * kLagStepUs;
            const auto& st = stateByLag[li];
            double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
            int n = 0;
            for (size_t k = 0; k < caps.size(); ++k) {
                if (st[k] < 0) continue;
                const double x = st[k];
                sx += x, sy += y[k], sxx += x, syy += y[k] * y[k], sxy += x * y[k];
                ++n;
            }
            if (n < 20) continue;
            const double vx = sxx - sx * sx / n, vy = syy - sy * sy / n;
            if (vx <= 0 || vy <= 0) continue;
            const double r = (sxy - sx * sy / n) / std::sqrt(vx * vy);
            if (r > bestR) {
                bestR = r;
                best = d;
            }
        }
        return {best, bestR};
    }

    // When the tile crossed half-way between its black and white levels
    // after flash |j|, interpolated between captures; NaN if not found.
    double crossing(int tile, size_t j, int64_t lag, double lo, double hi) const
    {
        const auto& g = toggles[j];
        const int64_t from = std::max(g.t, g.t + lag - 150'000);
        int64_t to = g.t + lag + 150'000;
        if (j + 1 < toggles.size()) to = std::min(to, toggles[j + 1].t + lag - 30'000);
        const double mid = (lo + hi) / 2;
        auto it = std::lower_bound(caps.begin(), caps.end(), from,
                                   [](const Capture& c, int64_t v) { return c.t < v; });
        if (it == caps.begin() || it == caps.end()) return NAN;
        for (auto prev = std::prev(it); it != caps.end() && it->t <= to; prev = it, ++it) {
            const double a = prev->tiles[size_t(tile)], b = it->tiles[size_t(tile)];
            const bool crossed = g.white ? (a < mid && b >= mid) : (a > mid && b <= mid);
            if (!crossed) continue;
            const double f = (mid - a) / (b - a);
            return double(prev->t) + f * double(it->t - prev->t) - double(g.t);
        }
        return NAN;
    }

    std::vector<Region> run()
    {
        precompute();
        const int n = cols * rows;
        std::vector<int64_t> lag(size_t(n), -1);
        std::vector<double> lo(static_cast<size_t>(n)), hi(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            double s = 0, ss = 0;
            for (const auto& c : caps) {
                s += c.tiles[size_t(i)];
                ss += double(c.tiles[size_t(i)]) * c.tiles[size_t(i)];
            }
            const double m = s / double(caps.size());
            if (std::sqrt(std::max(0.0, ss / double(caps.size()) - m * m)) < kMinStd) continue;
            const auto [d, r] = bestLag(i);
            if (r < kMinCorr) continue;
            // Black / white levels away from the switches.
            double sl = 0, sh = 0;
            int nl = 0, nh = 0;
            for (const auto& c : caps) {
                const int s0 = stateAt(c.t - d - 60'000), s1 = stateAt(c.t - d + 60'000);
                if (s0 < 0 || s0 != s1) continue;
                if (s0) sh += c.tiles[size_t(i)], ++nh;
                else sl += c.tiles[size_t(i)], ++nl;
            }
            if (!nl || !nh) continue;
            lag[size_t(i)] = d;
            lo[size_t(i)] = sl / nl;
            hi[size_t(i)] = sh / nh;
        }

        // Connected regions of following tiles (4-neighbour).
        std::vector<int> seen(size_t(n), 0);
        std::vector<Region> regions;
        for (int i = 0; i < n; ++i) {
            if (lag[size_t(i)] < 0 || seen[size_t(i)]) continue;
            Region rg;
            std::vector<int> stack{i};
            seen[size_t(i)] = 1;
            while (!stack.empty()) {
                const int t = stack.back();
                stack.pop_back();
                rg.tiles.push_back(t);
                const int x = t % cols, y = t / cols;
                rg.x0 = std::min(rg.x0, x), rg.x1 = std::max(rg.x1, x);
                rg.y0 = std::min(rg.y0, y), rg.y1 = std::max(rg.y1, y);
                for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
                    const int k = ny * cols + nx;
                    // Same region only if the lag is close (a video window
                    // next to the probe window stays separate).
                    if (lag[size_t(k)] < 0 || seen[size_t(k)] ||
                        std::abs(lag[size_t(k)] - lag[size_t(t)]) > 40'000)
                        continue;
                    seen[size_t(k)] = 1;
                    stack.push_back(k);
                }
            }
            if (int(rg.tiles.size()) < kMinTiles) continue;
            std::vector<double> lags;
            for (int t : rg.tiles) lags.push_back(double(lag[size_t(t)]));
            rg.lagUs = median(lags);
            // Per flash: the median crossing over the region's tiles.
            for (size_t j = 0; j < toggles.size(); ++j) {
                std::vector<double> v;
                for (int t : rg.tiles) v.push_back(crossing(t, j, lag[size_t(t)], lo[size_t(t)], hi[size_t(t)]));
                rg.perFlashUs.push_back(median(v));
            }
            rg.medianUs = median(rg.perFlashUs);
            regions.push_back(std::move(rg));
        }
        std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) {
            return a.medianUs < b.medianUs;
        });
        return regions;
    }
};

struct Dist {
    int n = 0;
    double mean = NAN, p50 = NAN, p95 = NAN, min = NAN, max = NAN;
};

Dist dist(std::vector<double> v)
{
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return std::isnan(x); }), v.end());
    Dist d;
    d.n = int(v.size());
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    double s = 0;
    for (double x : v) s += x;
    d.mean = s / double(v.size());
    d.p50 = v[v.size() / 2];
    d.p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
    d.min = v.front();
    d.max = v.back();
    return d;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("p2pcall");
    QCommandLineParser cli;
    cli.setApplicationDescription("Glass-to-glass latency probe: point the camera at the flashing window.");
    cli.addHelpOption();
    QCommandLineOption secondsOpt("seconds", "How long to measure.", "s", "60");
    QCommandLineOption csvOpt("csv", "Also write per-flash latencies to this CSV file.", "file");
    QCommandLineOption sourceLogOpt("source-log",
                                    "No camera: the call sends p2pcall's --video-source flash, whose switch times are "
                                    "in this file (its --flash-log). Measures frame created -> on screen.",
                                    "file");
    QCommandLineOption flashOnlyOpt("flash-only", "Just flash for --seconds and count repaints (debugging).");
    flashOnlyOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption selfTestOpt("self-test", "Check the analysis on synthetic captures (no window, no capture).");
    selfTestOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    cli.addOptions({secondsOpt, csvOpt, selfTestOpt, flashOnlyOpt, sourceLogOpt});
    cli.process(app);

    if (cli.isSet(selfTestOpt)) {
        // 40x30 tiles at 60 fps for 60 s with capture jitter and noise: a
        // "probe" block that switches 20 ms after each flash, a "video" block
        // that fades over 30 ms starting 90 ms after it. Expect ~70 ms.
        std::vector<Capture> caps;
        std::vector<Toggle> toggles;
        int64_t t = 1'000'000;
        bool white = false;
        auto* rng = QRandomGenerator::global();
        for (int64_t at = 1'300'000; at < 60'000'000; at += rng->bounded(500'000, 900'000)) {
            white = !white;
            toggles.push_back({at, white});
        }
        auto level = [&](int64_t when, int64_t delay, int64_t fade) {
            double v = 0;
            for (const auto& g : toggles) {
                const double x = std::clamp(double(when - g.t - delay) / double(std::max<int64_t>(1, fade)), 0.0, 1.0);
                if (x > 0) v = g.white ? x * 1.0 + (1 - x) * v : (1 - x) * v;
            }
            return v;
        };
        for (; t < 61'000'000; t += 16'667 + rng->bounded(-1500, 1500)) {
            Capture c;
            c.t = t;
            c.tiles.resize(40 * 30);
            for (int i = 0; i < 40 * 30; ++i) {
                const int x = i % 40, y = i / 40;
                double v = 30 + rng->bounded(8);                                       // static noise
                if (x < 10 && y < 10) v = 16 + 219 * level(t, 20'000, 1);               // probe window
                else if (x >= 20 && x < 30 && y >= 15 && y < 25)
                    v = 40 + 150 * level(t, 90'000, 30'000) + rng->bounded(10);         // camera view
                c.tiles[size_t(i)] = uint8_t(std::clamp(v, 0.0, 255.0));
            }
            caps.push_back(std::move(c));
        }
        Analysis a{caps, toggles, 40, 30, {}};
        const auto r = a.run();
        std::printf("regions %zu\n", r.size());
        for (const auto& g : r) std::printf("  %zu tiles at %d,%d: median %.1f ms\n", g.tiles.size(), g.x0, g.y0, g.medianUs / 1000);
        if (r.size() != 2) return 1;
        std::vector<double> d;
        for (size_t j = 0; j < toggles.size(); ++j) d.push_back((r[1].perFlashUs[j] - r[0].perFlashUs[j]) / 1000);
        const Dist dd = dist(d);
        std::printf("glass-to-glass mean %.1f p50 %.1f p95 %.1f (expect ~70 + half the 30 ms fade = ~85)\n", dd.mean,
                    dd.p50, dd.p95);
        return std::abs(dd.p50 - 85) < 5 ? 0 : 1;
    }
    const int64_t durationUs = int64_t(std::max(10, cli.value(secondsOpt).toInt())) * 1'000'000;

    FlashWindow win;
    win.setTitle("latency probe: point the camera here");
    win.resize(640, 640);
    win.show();

    std::mutex m;
    std::vector<Capture> caps;
    std::vector<Toggle> toggles;
    int cols = 0, rows = 0;
    int64_t startUs = 0;
    bool started = false, finished = false;

    win.onPainted = [&](int64_t t, bool white) {
        std::lock_guard l(m);
        if (started) toggles.push_back({t, white});
    };

    QTimer flashTimer;
    flashTimer.setSingleShot(true);
    flashTimer.setTimerType(Qt::PreciseTimer);
    int ticks = 0;
    QObject::connect(&flashTimer, &QTimer::timeout, [&] {
        ++ticks;
        win.white = !win.white;
        win.pendingReport = true;
        win.update();
        const auto gap = QRandomGenerator::global()->bounded(int(kMinGapUs / 1000), int(kMaxGapUs / 1000));
        flashTimer.start(gap);
    });

    if (cli.isSet(flashOnlyOpt)) {
        int paints = 0;
        win.onPainted = [&](int64_t, bool) { ++paints; };
        flashTimer.start(300);
        QTimer::singleShot(int(durationUs / 1000), &app, [&] {
            std::printf("timer ticks %d, flashes painted %d\n", ticks, paints);
            app.exit(0);
        });
        return app.exec();
    }

    video::ScreenConfig sc;
    sc.mode = video::ScreenConfig::Mode::Motion;
    sc.maxFps = 60;
    sc.maxWidth = 1920;
    sc.maxHeight = 1080;
    const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(cfgDir);
    sc.restoreTokenFile = cfgDir + "/screencast-token-latency-probe";
    auto source = video::createScreenSource(sc, [&](QString why) {
        QMetaObject::invokeMethod(&app, [&, why] {
            if (finished) return;
            std::fprintf(stderr, "screen capture ended: %s\n", qPrintable(why));
            app.exit(1);
        }, Qt::QueuedConnection);
    });

    auto finish = [&] {
        finished = true;
        flashTimer.stop();
        source->stop();
        std::lock_guard l(m);
        std::printf("\n%zu captures, %zu flashes (%d requested), %dx%d tiles\n", caps.size(), toggles.size(), ticks,
                    cols, rows);
        if (caps.size() < 100 || toggles.size() < 10) {
            std::printf("not enough data\n");
            app.exit(1);
            return;
        }
        Analysis a{caps, toggles, cols, rows};
        const auto regions = a.run();
        if (regions.empty()) {
            std::printf("nothing on screen follows the flashes (is the probe window visible to the capture?)\n");
            app.exit(1);
            return;
        }
        const Region& ref = regions.front();
        auto where = [&](const Region& r) {
            return QStringLiteral("x %1-%2% y %3-%4% of the screen, %5 tiles")
                .arg(100 * r.x0 / cols)
                .arg(100 * (r.x1 + 1) / cols)
                .arg(100 * r.y0 / rows)
                .arg(100 * (r.y1 + 1) / rows)
                .arg(r.tiles.size());
        };
        std::printf("\nreference: the probe window (%s)\n  flash -> seen in capture: median %.1f ms\n",
                    qPrintable(where(ref)), ref.medianUs / 1000.0);
        if (cli.isSet(sourceLogOpt)) {
            // The video windows follow the flash source's switches, not ours.
            std::vector<Toggle> src;
            QFile f(cli.value(sourceLogOpt));
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                while (!f.atEnd()) {
                    const auto parts = QString::fromUtf8(f.readLine()).trimmed().split(' ');
                    if (parts.size() != 2) continue;
                    const int64_t t = parts[0].toLongLong();
                    if (t > caps.front().t + 200'000 && t < caps.back().t - kMaxLagUs) src.push_back({t, parts[1] == "1"});
                }
            }
            std::printf("\nflash source: %zu switches in %s during the capture\n", src.size(),
                        qPrintable(f.fileName()));
            if (src.size() < 10) {
                std::printf("not enough (is the call sending --video-source flash with this --flash-log?)\n");
                app.exit(1);
                return;
            }
            Analysis b{caps, src, cols, rows, {}};
            const auto vids = b.run();
            if (vids.empty()) std::printf("no window on screen follows the flash source\n");
            // A switch is seen in the capture |ref.medianUs| after the probe
            // paints it; take that off so the numbers end at the screen.
            for (size_t i = 0; i < vids.size(); ++i) {
                std::vector<double> v;
                for (double x : vids[i].perFlashUs) v.push_back((x - ref.medianUs) / 1000.0);
                const Dist d = dist(v);
                std::printf("\nvideo window %zu (%s)\n  frame created -> on screen over %d switches: mean %.1f ms, "
                            "median %.1f, p95 %.1f, min %.1f, max %.1f\n",
                            i + 1, qPrintable(where(vids[i])), d.n, d.mean, d.p50, d.p95, d.min, d.max);
            }
            app.exit(0);
            return;
        }
        if (regions.size() == 1)
            std::printf("\nno video window follows the flashes: is the camera pointed at the probe window and the "
                        "call's video visible on this screen?\n");
        QFile csv(cli.value(csvOpt));
        const bool writeCsv = cli.isSet(csvOpt) && csv.open(QIODevice::WriteOnly | QIODevice::Truncate);
        if (writeCsv) csv.write("region,flash,glass_to_glass_ms\n");
        for (size_t i = 1; i < regions.size(); ++i) {
            const Region& r = regions[i];
            std::vector<double> g2g;
            for (size_t j = 0; j < toggles.size(); ++j) {
                const double v = r.perFlashUs[j] - ref.perFlashUs[j];
                g2g.push_back(v / 1000.0);
                if (writeCsv && !std::isnan(v))
                    csv.write(QStringLiteral("%1,%2,%3\n").arg(i).arg(j).arg(v / 1000.0, 0, 'f', 2).toUtf8());
            }
            const Dist d = dist(g2g);
            std::printf("\nvideo window %zu (%s)\n  glass-to-glass over %d flashes: mean %.1f ms, median %.1f, p95 "
                        "%.1f, min %.1f, max %.1f\n",
                        i, qPrintable(where(r)), d.n, d.mean, d.p50, d.p95, d.min, d.max);
        }
        app.exit(0);
    };

    QString err;
    const bool ok = source->start(
        [&](std::shared_ptr<video::VideoFrame> f) {
            if (f->repeated) return; // unchanged screen re-sent with a new timestamp
            Capture c;
            c.t = f->captureUs;
            const int cw = f->width / kTile, ch = f->height / kTile;
            c.tiles.resize(size_t(cw * ch));
            for (int ty = 0; ty < ch; ++ty)
                for (int tx = 0; tx < cw; ++tx) {
                    // Sample a 4x4 grid inside the tile (fast, plenty for flat areas).
                    int sum = 0;
                    for (int yy = 2; yy < kTile; yy += 4)
                        for (int xx = 2; xx < kTile; xx += 4)
                            sum += f->y()[size_t(ty * kTile + yy) * size_t(f->width) + size_t(tx * kTile + xx)];
                    c.tiles[size_t(ty * cw + tx)] = uint8_t(sum / 16);
                }
            std::lock_guard l(m);
            if (finished) return;
            if (!started) {
                started = true;
                startUs = c.t;
                cols = cw;
                rows = ch;
                QMetaObject::invokeMethod(&app, [&] {
                    std::printf("capturing; flashing for %lld s...\n", (long long)(durationUs / 1'000'000));
                    std::fflush(stdout);
                    flashTimer.start(300);
                    QTimer::singleShot(int(durationUs / 1000), &app, finish);
                }, Qt::QueuedConnection);
            }
            if (cw != cols || ch != rows) return; // capture size changed: ignore
            caps.push_back(std::move(c));
        },
        &err);
    if (!ok) {
        std::fprintf(stderr, "screen capture failed: %s\n", qPrintable(err));
        return 1;
    }
    std::printf("pick the screen to capture in the dialog (the whole monitor showing the probe and the call)\n");
    std::fflush(stdout);
    return app.exec();
}
