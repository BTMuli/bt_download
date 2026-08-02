#pragma once

#include "bt_download/task.hpp"

#include <cstdint>
#include <optional>

namespace bt {

std::optional<SeedStopReason> evaluate_seed_stop(bool enabled, double ratio_limit,
    int time_limit_minutes, std::uint64_t uploaded_bytes, std::uint64_t total_wanted_bytes,
    std::uint64_t seeding_seconds);

} // namespace bt
