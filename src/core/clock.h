#pragma once

#include <chrono>
#include <cstdint>

namespace p2p {

// Monotonic clock in microseconds. All latency/RTT bookkeeping uses this
// clock; cross-peer comparisons go through the clock-offset estimate kept
// by ControlChannel.
inline int64_t nowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace p2p
