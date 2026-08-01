#include "bt_download/path_safety.hpp"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bt {
namespace {

std::filesystem::path normalize_absolute(const std::filesystem::path& path, std::error_code& error) {
    auto absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    return std::filesystem::weakly_canonical(absolute, error);
}

} // namespace

PathValidation validate_save_path(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute()) {
        return {false, {}, "SAVE_PATH_INVALID", "savePath must be an absolute path"};
    }
    std::error_code error;
    const auto normalized = normalize_absolute(path, error);
    if (error) return {false, {}, "SAVE_PATH_UNAVAILABLE", "savePath cannot be normalized: " + error.message()};
    if (!std::filesystem::exists(normalized, error) || !std::filesystem::is_directory(normalized, error)) {
        return {false, normalized, "SAVE_PATH_UNAVAILABLE", "savePath must be an existing directory"};
    }
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = getpid();
#endif
    const auto probe = normalized / (".bt_download_write_probe_" + std::to_string(process_id));
    {
        std::ofstream output(probe, std::ios::binary | std::ios::trunc);
        if (!output) return {false, normalized, "SAVE_PATH_NOT_WRITABLE", "savePath is not writable"};
    }
    std::filesystem::remove(probe, error);
    return {true, normalized, {}, {}};
}

bool is_safe_relative_torrent_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& component : path) {
        if (component == ".." || component == ".") return false;
    }
    return true;
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::error_code error;
    const auto normalized_root = normalize_absolute(root, error);
    if (error) return false;
    const auto normalized_candidate = normalize_absolute(candidate, error);
    if (error) return false;
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized_candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(root_it->c_str(), candidate_it->c_str()) != 0) return false;
#else
        if (*root_it != *candidate_it) return false;
#endif
    }
    return true;
}

} // namespace bt
