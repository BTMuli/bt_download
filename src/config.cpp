#include "bt_download/config.hpp"

#include "tracker_config.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace bt {
namespace {

template <typename Type>
Type integer_field(const nlohmann::json& patch, std::string_view name, Type current) {
    if (!patch.contains(name)) return current;
    const auto& value = patch.at(name);
    if (!value.is_number_integer()) throw std::invalid_argument(std::string(name) + " must be an integer");
    return value.get<Type>();
}

std::int64_t rate_field(const nlohmann::json& patch, std::string_view name, std::int64_t current) {
    return integer_field(patch, name, current);
}

} // namespace

EngineConfig apply_config_patch(const EngineConfig& current, const nlohmann::json& patch) {
    if (!patch.is_object()) throw std::invalid_argument("config must be an object");
    auto next = current;
    next.active_downloads = integer_field(patch, "activeDownloads", next.active_downloads);
    next.download_rate_limit = rate_field(patch, "downloadRateLimit", next.download_rate_limit);
    next.upload_rate_limit = rate_field(patch, "uploadRateLimit", next.upload_rate_limit);
    next.connections_limit = integer_field(patch, "connectionsLimit", next.connections_limit);
    next.connections_per_task = integer_field(patch, "connectionsPerTask", next.connections_per_task);
    next.metadata_timeout_seconds = integer_field(patch, "metadataTimeoutSeconds", next.metadata_timeout_seconds);
    next.seed_time_limit_minutes = integer_field(patch, "seedTimeLimitMinutes", next.seed_time_limit_minutes);

    if (patch.contains("seedingEnabled")) {
        if (!patch.at("seedingEnabled").is_boolean()) {
            throw std::invalid_argument("seedingEnabled must be a boolean");
        }
        next.seeding_enabled = patch.at("seedingEnabled").get<bool>();
    }
    if (patch.contains("seedRatioLimit")) {
        if (!patch.at("seedRatioLimit").is_number()) {
            throw std::invalid_argument("seedRatioLimit must be a number");
        }
        next.seed_ratio_limit = patch.at("seedRatioLimit").get<double>();
    }
    if (patch.contains("additionalTrackers")) {
        next.additional_trackers = normalize_tracker_urls(patch.at("additionalTrackers"));
    }

    if (next.active_downloads < 1 || next.active_downloads > 64) {
        throw std::invalid_argument("activeDownloads must be between 1 and 64");
    }
    if (next.connections_limit < 1 || next.connections_limit > 10000
        || next.connections_per_task < 1 || next.connections_per_task > next.connections_limit) {
        throw std::invalid_argument("connection limits are invalid");
    }
    if (next.download_rate_limit < 0 || next.upload_rate_limit < 0
        || next.download_rate_limit > std::numeric_limits<int>::max()
        || next.upload_rate_limit > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("rate limits must fit a non-negative 32-bit integer");
    }
    if (next.metadata_timeout_seconds < 1 || next.metadata_timeout_seconds > 86400) {
        throw std::invalid_argument("metadataTimeoutSeconds must be between 1 and 86400");
    }
    if (!std::isfinite(next.seed_ratio_limit)
        || (next.seed_ratio_limit != 0.0
            && (next.seed_ratio_limit < 0.1 || next.seed_ratio_limit > 100.0))) {
        throw std::invalid_argument("seedRatioLimit must be 0 or between 0.1 and 100.0");
    }
    if (next.seed_time_limit_minutes < 0 || next.seed_time_limit_minutes > 525600) {
        throw std::invalid_argument("seedTimeLimitMinutes must be between 0 and 525600");
    }
    if (next.seeding_enabled && next.seed_ratio_limit == 0.0 && next.seed_time_limit_minutes == 0) {
        throw std::invalid_argument("enabled seeding requires at least one stop condition");
    }
    return next;
}

nlohmann::json config_json(const EngineConfig& config) {
    return {
        {"activeDownloads", config.active_downloads},
        {"downloadRateLimit", config.download_rate_limit},
        {"uploadRateLimit", config.upload_rate_limit},
        {"connectionsLimit", config.connections_limit},
        {"connectionsPerTask", config.connections_per_task},
        {"metadataTimeoutSeconds", config.metadata_timeout_seconds},
        {"additionalTrackers", config.additional_trackers},
        {"seedingEnabled", config.seeding_enabled},
        {"seedRatioLimit", config.seed_ratio_limit},
        {"seedTimeLimitMinutes", config.seed_time_limit_minutes},
    };
}

} // namespace bt
