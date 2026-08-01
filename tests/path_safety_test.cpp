#include "bt_download/path_safety.hpp"

#include <filesystem>
#include <stdexcept>

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
}
