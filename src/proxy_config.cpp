#include "bt_download/proxy_config.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>

namespace bt {
namespace {

constexpr std::size_t max_proxy_url_bytes = 4096;
constexpr std::size_t max_proxy_host_bytes = 255;
constexpr std::size_t max_proxy_credential_bytes = 1024;
constexpr std::size_t max_bypass_rules = 256;
constexpr std::size_t max_bypass_rule_bytes = 512;

std::optional<std::string> optional_string(
    const nlohmann::json& value, std::string_view name, std::size_t max_bytes) {
    if (!value.contains(name)) return std::nullopt;
    const auto& field = value.at(name);
    if (field.is_null()) return std::nullopt;
    if (!field.is_string()) {
        throw std::invalid_argument(std::string(name) + " must be a string or null");
    }
    auto result = field.get<std::string>();
    if (result.empty() || result.size() > max_bytes) {
        throw std::invalid_argument(std::string(name) + " has an invalid length");
    }
    return result;
}

std::string required_string(
    const nlohmann::json& value, std::string_view name, std::size_t max_bytes) {
    const auto result = optional_string(value, name, max_bytes);
    if (!result) throw std::invalid_argument(std::string(name) + " is required");
    return *result;
}

} // namespace

EngineProxyConfig parse_proxy_config(const nlohmann::json& value) {
    if (!value.is_object()) throw std::invalid_argument("proxy must be an object");
    if (!value.contains("enabled") || !value.at("enabled").is_boolean()) {
        throw std::invalid_argument("proxy.enabled must be a boolean");
    }

    EngineProxyConfig result;
    result.enabled = value.at("enabled").get<bool>();
    result.http_proxy = optional_string(value, "httpProxy", max_proxy_url_bytes);
    result.https_proxy = optional_string(value, "httpsProxy", max_proxy_url_bytes);

    if (value.contains("bypass")) {
        const auto& bypass = value.at("bypass");
        if (!bypass.is_array() || bypass.size() > max_bypass_rules) {
            throw std::invalid_argument("proxy.bypass must be an array with at most 256 entries");
        }
        result.bypass.reserve(bypass.size());
        for (const auto& rule : bypass) {
            if (!rule.is_string()) throw std::invalid_argument("proxy.bypass entries must be strings");
            auto text = rule.get<std::string>();
            if (text.empty() || text.size() > max_bypass_rule_bytes) {
                throw std::invalid_argument("proxy.bypass entry has an invalid length");
            }
            result.bypass.push_back(std::move(text));
        }
    }

    if (value.contains("peerProxy") && !value.at("peerProxy").is_null()) {
        const auto& peer = value.at("peerProxy");
        if (!peer.is_object()) throw std::invalid_argument("proxy.peerProxy must be an object or null");
        if (!peer.contains("port") || !peer.at("port").is_number_integer()) {
            throw std::invalid_argument("proxy.peerProxy.port must be an integer");
        }
        const auto port = peer.at("port").get<std::int64_t>();
        if (port < 1 || port > (std::numeric_limits<std::uint16_t>::max)()) {
            throw std::invalid_argument("proxy.peerProxy.port must be between 1 and 65535");
        }
        PeerProxyEndpoint endpoint;
        endpoint.host = required_string(peer, "host", max_proxy_host_bytes);
        endpoint.port = static_cast<std::uint16_t>(port);
        endpoint.username = optional_string(peer, "username", max_proxy_credential_bytes).value_or("");
        endpoint.password = optional_string(peer, "password", max_proxy_credential_bytes).value_or("");
        result.peer_proxy = std::move(endpoint);
    }
    return result;
}

nlohmann::json proxy_status_json(const EngineProxyConfig& config) {
    return {
        {"enabled", config.enabled},
        {"httpConfigured", config.enabled && config.http_proxy.has_value()},
        {"httpsConfigured", config.enabled && config.https_proxy.has_value()},
        {"peerConfigured", config.enabled && config.peer_proxy.has_value()},
    };
}

} // namespace bt
