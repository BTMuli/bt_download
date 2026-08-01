#pragma once

#include <filesystem>
#include <string>

namespace bt {

struct PathValidation {
    bool valid;
    std::filesystem::path normalized;
    std::string error_code;
    std::string message;
};

PathValidation validate_save_path(const std::filesystem::path& path);
bool is_safe_relative_torrent_path(const std::filesystem::path& path);
bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate);

} // namespace bt
