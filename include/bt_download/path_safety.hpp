#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

// Returns the Windows extended-length (`\\?\`) form of the path; other
// platforms and relative paths are returned unchanged.
//
// Win32 calls behind std::filesystem cannot reach paths longer than MAX_PATH
// unless the machine enables long path support, while libtorrent always reads
// and writes payload files through the extended-length form. Payload checks
// must use the same form, otherwise existing files under deep save paths are
// reported as missing.
std::filesystem::path extended_length_path(const std::filesystem::path& path);

// Returns the size of a regular file, or std::nullopt when the path is missing,
// is not a regular file, or cannot be queried.
std::optional<std::uintmax_t> regular_file_size(const std::filesystem::path& path);

} // namespace bt
