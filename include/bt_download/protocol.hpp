#pragma once

#include <functional>
#include <istream>
#include <mutex>
#include <ostream>
#include <string>

#include <nlohmann/json.hpp>

namespace bt {

constexpr std::size_t max_frame_bytes = 1024 * 1024;

struct RpcError {
    int rpc_code;
    std::string code;
    std::string message;
    bool retryable{false};
    nlohmann::json data = nlohmann::json::object();
};

class ProtocolServer {
public:
    using Handler = std::function<nlohmann::json(const std::string&, const nlohmann::json&)>;

    ProtocolServer(std::istream& input, std::ostream& output, std::ostream& log, Handler handler);
    int run();
    void send_event(const std::string& method, const nlohmann::json& params);

private:
    void process_line(const std::string& line);
    void write(const nlohmann::json& message);

    std::istream& input_;
    std::ostream& output_;
    std::ostream& log_;
    Handler handler_;
    std::mutex write_mutex_;
    bool stop_requested_{false};
};

[[noreturn]] void fail(int rpc_code, std::string code, std::string message,
                       bool retryable = false, nlohmann::json data = nlohmann::json::object());

} // namespace bt
