#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace bt {

inline constexpr std::size_t max_additional_trackers = 512;
inline constexpr std::size_t max_tracker_url_bytes = 2048;

std::string normalize_tracker_url(std::string_view url);
std::vector<std::string> normalize_tracker_urls(const nlohmann::json& value);

} // namespace bt
