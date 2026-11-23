#pragma once

#include <QString>

#include <cmath>
#include <cstddef>
#include <deque>
#include <vector>

namespace p2p::video {

// Rolling per-stage latency over the last |window| frames, for the latency
// breakdown. A stage value may be NaN (unknown for that frame); it is then
// left out of that stage's numbers. Not thread-safe: callers lock.
class StageStats {
public:
    StageStats(std::vector<const char*> names, size_t window = 240) : names_(std::move(names)), window_(window) {}

    void add(std::vector<double> ms)
    {
        ms.resize(names_.size(), NAN);
        rows_.push_back(std::move(ms));
        while (rows_.size() > window_) rows_.pop_front();
    }
    void clear() { rows_.clear(); }
    size_t frames() const { return rows_.size(); }

    struct Stage {
        const char* name = "";
        double avg = NAN, p50 = NAN, p95 = NAN;
    };
    std::vector<Stage> summary() const;
    // "encode 12.1/14.0 | send 0.3/0.9 | ..." (avg/p95 ms); empty with no data.
    QString format() const;

private:
    std::vector<const char*> names_;
    size_t window_;
    std::deque<std::vector<double>> rows_;
};

} // namespace p2p::video
