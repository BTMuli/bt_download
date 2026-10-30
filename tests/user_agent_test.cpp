#include "bt_download/engine.hpp"
#include "peer_identity.hpp"

#include <algorithm>
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

void verify_peer_id_conventions() {
    // Actual client conventions: libtorrent's base-36 revision, qB Enhanced's
    // build tag, Deluge's release marker, old/new Transmission and aria2.
    const std::pair<std::string, std::string> cases[] = {
        {"-LT20B0-", "libtorrent 2.0.11"},
        {"-qB5240-", "qBittorrent 5.2.4"},
        {"-qB524A-", "qBittorrent 5.2.4.10"},
        {"-DE220s-", "Deluge 2.2.0"},
        {"-DE220b-", "Deluge 2.2.0b"},
        {"-TR4100-", "Transmission 4.1.0"},
        {"-TR2940-", "Transmission 2.94"},
        {"-UT2210-", "uTorrent 2.2.1"},
        {"-BC0207-", "BitComet 2.07"},
        {"A2-1-37-0-", "aria2 1.37.0"},
        {"M4-20-8-", "Mainline 4.20.8"},
        {"A2-1-x-0-", "unknown"},
        {"-ZZ1234-", "unknown"},
        {"random", "unknown"},
    };
    for (const auto& [prefix, expected] : cases) {
        libtorrent::peer_id pid;
        std::copy(prefix.begin(), prefix.end(), pid.data());
        expect(bt::identify_peer_client(pid) == expected, "Peer ID convention decoded incorrectly");
    }
    bt::PeerIdentityStore store;
    const libtorrent::tcp::endpoint endpoint{libtorrent::make_address("127.0.0.1"), 6881};
    libtorrent::peer_id pid;
    pid[0] = 'x';
    int first = 0;
    int replacement = 0;
    store.connected(endpoint, &first);
    expect(store.user_agent(endpoint, pid).empty(), "missing declaration was fabricated");
    store.reported(endpoint, &first, pid, "BangumiToday/0.8.0");
    expect(store.user_agent(endpoint, pid) == "BangumiToday/0.8.0", "declaration was lost");
    store.connected(endpoint, &replacement);
    store.disconnected(endpoint, &first);
    expect(store.user_agent(endpoint, pid).empty(), "reconnection reused a stale declaration");
    store.reported(endpoint, &replacement, pid, "Replacement/1.0");
    expect(store.user_agent(endpoint, pid) == "Replacement/1.0", "late disconnect erased replacement");
    store.disconnected(endpoint, &replacement);
    expect(store.user_agent(endpoint, pid).empty(), "disconnection retained a declaration");
}

} // namespace

void run_user_agent_tests() {
    verify_peer_id_conventions();
    TemporaryDirectory temporary;
    const auto state_path = temporary.path() / "state";

    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        const auto result = engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.9"},
            {"statePath", path_utf8(state_path / "with-user-agent")},
            {"userAgent", "BangumiToday/0.8.0"}});
        expect(result.contains("protocolVersion"),
            "initialize with a userAgent did not complete");
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        expect(initialize_rejected(engine, {
            {"protocolVersion", "1.9"},
            {"statePath", path_utf8(state_path / "non-string-user-agent")},
            {"userAgent", 42}}, "userAgent must be a string"),
            "non-string userAgent was accepted");
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        expect(initialize_rejected(engine, {
            {"protocolVersion", "1.9"},
            {"statePath", path_utf8(state_path / "empty-user-agent")},
            {"userAgent", ""}}, "between 1 and 255"),
            "empty userAgent was accepted");
    }
    {
        const std::string overlong(256, 'x');
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        expect(initialize_rejected(engine, {
            {"protocolVersion", "1.9"},
            {"statePath", path_utf8(state_path / "overlong-user-agent")},
            {"userAgent", overlong}}, "between 1 and 255"),
            "overlong userAgent was accepted");
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        const auto result = engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.9"},
            {"statePath", path_utf8(state_path / "default-user-agent")}});
        expect(result.contains("protocolVersion"),
            "initialize without a userAgent did not complete");
    }
}
