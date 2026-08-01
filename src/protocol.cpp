#include "bt_download/protocol.hpp"

#include <exception>
#include <utility>

namespace bt {
namespace {

class RpcException final : public std::exception {
public:
    explicit RpcException(RpcError error) : error_(std::move(error)) {}
    const char* what() const noexcept override { return error_.message.c_str(); }
    const RpcError& error() const noexcept { return error_; }
private:
    RpcError error_;
};

nlohmann::json error_json(const RpcError& error) {
    auto data = error.data;
    data["code"] = error.code;
    data["retryable"] = error.retryable;
    return {{"code", error.rpc_code}, {"message", error.message}, {"data", std::move(data)}};
}

} // namespace

[[noreturn]] void fail(int rpc_code, std::string code, std::string message, bool retryable, nlohmann::json data) {
    throw RpcException({rpc_code, std::move(code), std::move(message), retryable, std::move(data)});
}

ProtocolServer::ProtocolServer(std::istream& input, std::ostream& output, std::ostream& log, Handler handler)
    : input_(input), output_(output), log_(log), handler_(std::move(handler)) {}

int ProtocolServer::run() {
    std::string line;
    while (std::getline(input_, line)) {
        if (line.size() > max_frame_bytes) {
            write({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", error_json({-32600, "FRAME_TOO_LARGE", "request frame exceeds 1 MiB", false})}});
            continue;
        }
        process_line(line);
        if (stop_requested_) break;
    }
    return 0;
}

void ProtocolServer::send_event(const std::string& method, const nlohmann::json& params) {
    write({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

void ProtocolServer::process_line(const std::string& line) {
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error&) {
        write({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", error_json({-32700, "PARSE_ERROR", "invalid JSON", false})}});
        return;
    }
    nlohmann::json id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" || !request.contains("method") || !request["method"].is_string()) {
        write({{"jsonrpc", "2.0"}, {"id", id}, {"error", error_json({-32600, "INVALID_REQUEST", "invalid JSON-RPC request", false})}});
        return;
    }
    if (!request.contains("id")) {
        log_ << "Ignoring client notification: " << request["method"].get<std::string>() << '\n';
        return;
    }
    try {
        const auto params = request.value("params", nlohmann::json::object());
        if (!params.is_object()) fail(-32602, "INVALID_PARAMS", "params must be an object");
        auto result = handler_(request["method"].get<std::string>(), params);
        write({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
        if (request["method"] == "engine.shutdown") stop_requested_ = true;
    } catch (const RpcException& exception) {
        write({{"jsonrpc", "2.0"}, {"id", id}, {"error", error_json(exception.error())}});
    } catch (const nlohmann::json::exception& exception) {
        write({{"jsonrpc", "2.0"}, {"id", id}, {"error", error_json({-32602, "INVALID_PARAMS", exception.what(), false})}});
    } catch (const std::exception& exception) {
        log_ << "Unhandled request error: " << exception.what() << '\n';
        write({{"jsonrpc", "2.0"}, {"id", id}, {"error", error_json({-32603, "INTERNAL_ERROR", "internal engine error", true})}});
    }
}

void ProtocolServer::write(const nlohmann::json& message) {
    std::scoped_lock lock(write_mutex_);
    output_ << message.dump() << '\n';
    output_.flush();
}

} // namespace bt
