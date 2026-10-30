#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace bt {

// Monotonic, process-local anchor. Only the returned milliseconds are persisted.
// Every call consumes a disjoint interval, including repeated reads/checkpoints.
class DownloadTimer {
public:
    using Clock = std::chrono::steady_clock;
    std::uint64_t sample(bool running, Clock::time_point now = Clock::now());

private:
    std::optional<Clock::time_point> sampled_;
    Clock::duration remainder_{};
    bool running_{false};
};

} // namespace bt
