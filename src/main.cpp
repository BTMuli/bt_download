#include "bt_download/engine.hpp"
#include "bt_download/protocol.hpp"

#include <iostream>

int main() {
    std::ios::sync_with_stdio(false);
    bt::ProtocolServer* protocol = nullptr;
    bt::Engine engine([&protocol](const std::string& method, const nlohmann::json& params) {
        if (protocol != nullptr) protocol->send_event(method, params);
    });
    bt::ProtocolServer server(std::cin, std::cout, std::cerr,
        [&engine](const std::string& method, const nlohmann::json& params) { return engine.dispatch(method, params); });
    protocol = &server;
    server.send_event("event.ready", {{"protocolVersion", BT_DOWNLOAD_PROTOCOL_VERSION}, {"engineVersion", BT_DOWNLOAD_VERSION}});
    const int result = server.run();
    if (!engine.shutdown_requested()) engine.dispatch("engine.shutdown", nlohmann::json::object());
    return result;
}
