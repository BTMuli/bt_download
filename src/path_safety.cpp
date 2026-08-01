#include "bt_download/path_safety.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bt {
namespace {

std::filesystem::path normalize_absolute(const std::filesystem::path& path, std::error_code& error) {
    auto absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    return std::filesystem::weakly_canonical(absolute, error);
}

std::string random_probe_suffix() {
    std::array<std::uint32_t, 4> words{};
    std::random_device random;
    for (auto& word : words) word = random();
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : words) output << std::setw(8) << word;
    return output.str();
}

bool can_create_write_probe(const std::filesystem::path& directory) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto probe = directory / (".bt_download_write_probe_" + random_probe_suffix());
#ifdef _WIN32
        const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return true;
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
#else
        int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const int file = ::open(probe.c_str(), flags, 0600);
        if (file >= 0) {
            ::close(file);
            ::unlink(probe.c_str());
            return true;
        }
        if (errno != EEXIST) return false;
#endif
    }
    return false;
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
    if (!can_create_write_probe(normalized)) {
        return {false, normalized, "SAVE_PATH_NOT_WRITABLE", "savePath is not writable"};
    }
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
