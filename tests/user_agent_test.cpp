#include "bt_download/engine.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

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
        path_ = std::filesystem::temp_directory_path() / ("bt_download_user_agent_test_" + suffix);
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

bool initialize_rejected(bt::Engine& engine, const nlohmann::json& params,
    const std::string& expected_fragment) {
    try {
        engine.dispatch("engine.initialize", params);
    } catch (const std::exception& exception) {
        return std::string(exception.what()).find(expected_fragment) != std::string::npos;
    }
    return false;
}

} // namespace

void run_user_agent_tests() {
    TemporaryDirectory temporary;
    const auto state_path = temporary.path() / "state";

    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        const auto result = engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.2"},
            {"statePath", path_utf8(state_path / "with-user-agent")},
            {"userAgent", "BangumiToday/0.8.0"}});
        expect(result.contains("protocolVersion"),
            "initialize with a userAgent did not complete");
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        expect(initialize_rejected(engine, {
            {"protocolVersion", "1.2"},
            {"statePath", path_utf8(state_path / "non-string-user-agent")},
            {"userAgent", 42}}, "userAgent must be a string"),
            "non-string userAgent was accepted");
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        expect(initialize_rejected(engine, {
            {"protocolVersion", "1.2"},
            {"statePath", path_utf8(state_path / "empty-user-agent")},
            {"userAgent", ""}}, "between 1 and 255"),
            "empty userAgent was accepted");
    }
    {
        const std::string overlong(256, 'x');
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        expect(initialize_rejected(engine, {
            {"protocolVersion", "1.2"},
            {"statePath", path_utf8(state_path / "overlong-user-agent")},
            {"userAgent", overlong}}, "between 1 and 255"),
            "overlong userAgent was accepted");
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        const auto result = engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.2"},
            {"statePath", path_utf8(state_path / "default-user-agent")}});
        expect(result.contains("protocolVersion"),
            "initialize without a userAgent did not complete");
    }
}
