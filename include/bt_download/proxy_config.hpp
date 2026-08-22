#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bt {

struct PeerProxyEndpoint {
    std::string host;
    std::uint16_t port{0};
    std::string username;
    std::string password;
};

struct EngineProxyConfig {
    bool enabled{false};
    std::optional<std::string> http_proxy;
    std::optional<std::string> https_proxy;
    std::vector<std::string> bypass;
    std::optional<PeerProxyEndpoint> peer_proxy;
};

EngineProxyConfig parse_proxy_config(const nlohmann::json& value);
nlohmann::json proxy_status_json(const EngineProxyConfig& config);

} // namespace bt
