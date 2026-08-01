#include "bt_download/path_safety.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

void run_path_safety_tests() {
    expect(bt::is_safe_relative_torrent_path("folder/file.mkv"), "normal torrent path rejected");
    expect(!bt::is_safe_relative_torrent_path("../file.mkv"), "parent traversal accepted");
    expect(!bt::is_safe_relative_torrent_path("C:\\file.mkv"), "absolute torrent path accepted");
    const auto root = std::filesystem::temp_directory_path();
    expect(bt::is_within(root, root / "bt_download" / "file"), "child path rejected");
    expect(!bt::is_within(root / "bt_download", root / "other"), "sibling path accepted");
    expect(!bt::validate_save_path("relative").valid, "relative save path accepted");
    expect(bt::validate_save_path(root).valid, "writable temp path rejected");

#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = getpid();
#endif
    const auto probe_root = root / ("bt_download_path_safety_" + std::to_string(process_id) + "_"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(probe_root);
    const auto sentinel = probe_root / (".bt_download_write_probe_" + std::to_string(process_id));
    constexpr char sentinel_contents[] = "existing user data";
    {
        std::ofstream output(sentinel, std::ios::binary | std::ios::trunc);
        output << sentinel_contents;
    }
    expect(bt::validate_save_path(probe_root).valid, "writable path with an old-style probe sentinel rejected");
    expect(std::filesystem::exists(sentinel), "existing probe-named file was deleted");
    std::ifstream input(sentinel, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    expect(contents == sentinel_contents, "existing probe-named file was truncated");
    std::error_code cleanup_error;
    std::filesystem::remove_all(probe_root, cleanup_error);
}
