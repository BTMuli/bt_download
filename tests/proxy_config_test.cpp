#include "bt_download/engine.hpp"
#include "bt_download/http_download.hpp"
#include "bt_download/proxy_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / ("bt_download_proxy_test_" + suffix);
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

void run_proxy_config_tests() {
    const auto json = nlohmann::json{
        {"enabled", true},
        {"httpProxy", "http://127.0.0.1:7890"},
        {"httpsProxy", "http://user:secret@127.0.0.1:7891"},
        {"bypass", nlohmann::json::array({"<local>", "*.example.com"})},
        {"peerProxy", {
            {"host", "127.0.0.1"}, {"port", 7891},
            {"username", "user"}, {"password", "secret"},
        }},
    };
    const auto config = bt::parse_proxy_config(json);
    expect(config.enabled && config.peer_proxy && config.peer_proxy->port == 7891,
        "valid proxy config was not parsed");
    expect(bt::select_http_proxy("http://example.net/file", config)
            == "http://127.0.0.1:7890",
        "HTTP URL did not select the HTTP proxy");
    expect(bt::select_http_proxy("https://example.net/file", config)
            == "http://user:secret@127.0.0.1:7891",
        "HTTPS URL did not select the HTTPS proxy");
    expect(!bt::select_http_proxy("https://api.example.com/file", config),
        "Windows wildcard bypass rule was ignored");
    expect(!bt::select_http_proxy("http://localhost/file", config),
        "Windows local-host bypass rule was ignored");

    const auto status = bt::proxy_status_json(config);
    expect(status.at("enabled") == true && status.at("peerConfigured") == true,
        "proxy status did not expose capability flags");
    expect(status.dump().find("secret") == std::string::npos
            && status.dump().find("127.0.0.1") == std::string::npos,
        "proxy status exposed a proxy endpoint or credential");

    TemporaryDirectory temporary;
    bt::Engine engine([](const std::string&, const nlohmann::json&) {});
    const auto initialized = engine.dispatch("engine.initialize", {
        {"protocolVersion", "1.5"},
        {"statePath", path_utf8(temporary.path())},
        {"proxy", json},
    });
    expect(initialized.at("proxy").at("enabled") == true,
        "initial proxy config was not applied");
    const auto configured = engine.dispatch("engine.configureProxy", {{"enabled", false}});
    expect(configured.at("proxy").at("enabled") == false,
        "runtime proxy disable was not applied");
    engine.dispatch("engine.shutdown", nlohmann::json::object());

    std::ifstream catalog(temporary.path() / "catalog.json", std::ios::binary);
    const std::string persisted{
        std::istreambuf_iterator<char>(catalog), std::istreambuf_iterator<char>()};
    expect(persisted.find("secret") == std::string::npos
            && persisted.find("httpProxy") == std::string::npos,
        "ephemeral proxy config was persisted in the task catalog");

    bool rejected = false;
    try {
        (void)bt::parse_proxy_config({{"enabled", true}, {"peerProxy", {{"host", "x"}, {"port", 0}}}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "invalid proxy port was accepted");
}
