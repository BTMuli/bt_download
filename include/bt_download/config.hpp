#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bt {

struct EngineConfig {
    int active_downloads{4};
    std::int64_t download_rate_limit{0};
    std::int64_t upload_rate_limit{0};
    int connections_limit{256};
    int connections_per_task{64};
    int metadata_timeout_seconds{300};
    std::vector<std::string> additional_trackers;
    bool seeding_enabled{false};
    double seed_ratio_limit{2.0};
    int seed_time_limit_minutes{60};
    bool conserve_on_metered_or_low_power{false};
    std::int64_t constrained_upload_rate_limit{4 * 1024 * 1024};
    std::int64_t constrained_upload_total_limit{200 * 1024 * 1024};
};

EngineConfig apply_config_patch(const EngineConfig& current, const nlohmann::json& patch);
nlohmann::json config_json(const EngineConfig& config);

} // namespace bt
