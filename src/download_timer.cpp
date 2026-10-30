#include "bt_download/download_timer.hpp"

namespace bt {

std::uint64_t DownloadTimer::sample(bool running, Clock::time_point now) {
    if (sampled_ && now < *sampled_) return 0;
    if (sampled_ && running_) remainder_ += now - *sampled_;
    sampled_ = now;
    running_ = running;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(remainder_);
    remainder_ -= elapsed;
    return static_cast<std::uint64_t>(elapsed.count());
}

} // namespace bt
