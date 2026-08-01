#include "bt_download/engine.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string path_utf8(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.u8string();
#endif
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() / ("bt_download_magnet_test_" + suffix);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

void run_magnet_metadata_tests() {
    TemporaryDirectory temporary;
    const auto state_path = temporary.path() / "state";
    const auto save_path = temporary.path() / "downloads";
    std::filesystem::create_directories(save_path);

    bt::Engine engine([](const std::string&, const nlohmann::json&) {});
    engine.dispatch("engine.initialize", {
        {"protocolVersion", "1.0"},
        {"statePath", path_utf8(state_path)},
        {"config", {{"metadataTimeoutSeconds", 1}}},
    });
    const auto added = engine.dispatch("task.add", {
        {"source", {{"kind", "magnet"},
                    {"uri", "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"}}},
        {"savePath", path_utf8(save_path)},
        {"start", false},
    });
    const auto id = added.at("task").at("id").get<std::string>();
    expect(added.at("task").at("state") == "paused", "non-started magnet task must be paused");

    const auto resumed = engine.dispatch("task.resume", {{"id", id}});
    expect(resumed.at("task").at("state") == "metadata", "magnet resume must fetch metadata");

    nlohmann::json task;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        task = engine.dispatch("task.get", {{"id", id}}).at("task");
        if (task.at("state") == "error") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);

    expect(task.at("state") == "error", "magnet metadata timeout did not enter error state");
    expect(task.at("lastError").at("code") == "METADATA_TIMEOUT", "metadata timeout code mismatch");
    expect(task.at("lastError").at("retryable") == true, "metadata timeout must be retryable");

    const auto retried = engine.dispatch("task.retry", {{"id", id}});
    expect(retried.at("task").at("state") == "metadata", "metadata retry did not restart acquisition");
    engine.dispatch("task.pause", {{"id", id}});
    engine.dispatch("engine.shutdown", nlohmann::json::object());
}
