#include "bt_download/protocol.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

void run_protocol_tests() {
    std::istringstream input(
        "not-json\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"echo\",\"params\":{\"value\":42}}\n");
    std::ostringstream output;
    std::ostringstream log;
    bt::ProtocolServer server(input, output, log,
        [](const std::string& method, const nlohmann::json& params) {
            if (method != "echo") throw std::runtime_error("unexpected method");
            return params;
        });
    expect(server.run() == 0, "protocol server failed");
    std::istringstream lines(output.str());
    std::string first;
    std::string second;
    std::getline(lines, first);
    std::getline(lines, second);
    const auto parse_error = nlohmann::json::parse(first);
    const auto response = nlohmann::json::parse(second);
    expect(parse_error["error"]["data"]["code"] == "PARSE_ERROR", "parse error code mismatch");
    expect(response["result"]["value"] == 42, "response payload mismatch");
}
