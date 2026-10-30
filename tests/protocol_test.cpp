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

    // 非法系统/Tracker 文本必须局部替换，不能被误报成 INVALID_PARAMS 或丢失事件。
    const std::string malformed = std::string("\xB2") + "text";
    const std::string replaced = "\xef\xbf\xbd" "text";
    const std::string valid = "\xe4\xb8\xad\xe6\x96\x87\xe4\xbf\x9d\xe6\x8c\x81\xe5\x8e\x9f\xe6\xa0\xb7";
    std::istringstream text_input(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"list\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"fail\"}\n");
    std::ostringstream text_output;
    std::ostringstream text_log;
    bt::ProtocolServer text_server(text_input, text_output, text_log,
        [&](const std::string& method, const nlohmann::json&) -> nlohmann::json {
            if (method == "fail") bt::fail(-32000, "NETWORK_UNAVAILABLE", malformed, true);
            return {{"id", "task-1"}, {"message", malformed}, {"valid", valid}};
        });
    text_server.send_event("event.taskUpdated", {{"message", malformed}});
    expect(text_server.run() == 0, "invalid UTF-8 terminated protocol processing");
    std::istringstream text_lines(text_output.str());
    std::string frame;
    std::getline(text_lines, frame);
    expect(nlohmann::json::parse(frame).at("params").at("message") == replaced,
        "invalid UTF-8 event was not repaired");
    std::getline(text_lines, frame);
    const auto list = nlohmann::json::parse(frame);
    expect(list.at("result").at("message") == replaced
            && list.at("result").at("valid") == valid
            && list.at("result").at("id") == "task-1",
        "invalid UTF-8 display text broke the result or modified valid text");
    std::getline(text_lines, frame);
    const auto failure = nlohmann::json::parse(frame).at("error");
    expect(failure.at("message") == replaced && failure.at("data").at("code") == "NETWORK_UNAVAILABLE"
            && failure.at("data").at("retryable") == true,
        "invalid UTF-8 error changed the business code or retry condition");
}
