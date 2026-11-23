#include "video/latency_stages.h"

#include <algorithm>

namespace p2p::video {

std::vector<StageStats::Stage> StageStats::summary() const
{
    std::vector<Stage> out;
    std::vector<double> v;
    for (size_t i = 0; i < names_.size(); ++i) {
        Stage s;
        s.name = names_[i];
        v.clear();
        for (const auto& r : rows_)
            if (!std::isnan(r[i])) v.push_back(r[i]);
        if (!v.empty()) {
            double sum = 0;
            for (double x : v) sum += x;
            s.avg = sum / double(v.size());
            std::sort(v.begin(), v.end());
            s.p50 = v[v.size() / 2];
            s.p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
        }
        out.push_back(s);
    }
    return out;
}

QString StageStats::format() const
{
    QString s;
    for (const auto& st : summary()) {
        if (std::isnan(st.avg)) continue;
        if (!s.isEmpty()) s += QStringLiteral(" | ");
        s += QStringLiteral("%1 %2/%3").arg(QLatin1String(st.name)).arg(st.avg, 0, 'f', 1).arg(st.p95, 0, 'f', 1);
    }
    return s;
}

} // namespace p2p::video
