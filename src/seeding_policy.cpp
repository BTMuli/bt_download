#include "seeding_policy.hpp"

namespace bt {

std::optional<SeedStopReason> evaluate_seed_stop(bool enabled, double ratio_limit,
    int time_limit_minutes, std::uint64_t uploaded_bytes, std::uint64_t total_wanted_bytes,
    std::uint64_t seeding_seconds) {
    if (!enabled) return SeedStopReason::disabled;
    if (ratio_limit > 0.0 && total_wanted_bytes > 0) {
        const auto uploaded = static_cast<long double>(uploaded_bytes);
        const auto target = static_cast<long double>(total_wanted_bytes)
            * static_cast<long double>(ratio_limit);
        if (uploaded >= target) return SeedStopReason::ratio;
    }
    if (time_limit_minutes > 0
        && seeding_seconds >= static_cast<std::uint64_t>(time_limit_minutes) * 60U) {
        return SeedStopReason::time;
    }
    return std::nullopt;
}

} // namespace bt
