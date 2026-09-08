#include "bt_download/engine.hpp"
#include "bt_download/protocol.hpp"

#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#error The local tracker integration test currently requires Windows sockets
#endif

namespace {
namespace lt = libtorrent;
using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
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
        path_ = std::filesystem::temp_directory_path() / ("bt_download_integration_" + suffix);
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

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
    }

    ~WinsockRuntime() { WSACleanup(); }
};

class ChildProcess {
public:
    explicit ChildProcess(const std::vector<std::filesystem::path>& arguments) {
        expect(!arguments.empty(), "child process requires an executable path");
        std::wstring command_line;
        for (const auto& argument : arguments) {
            if (!command_line.empty()) command_line.push_back(L' ');
            command_line.push_back(L'\"');
            command_line.append(argument.wstring());
            command_line.push_back(L'\"');
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        auto mutable_command_line = command_line;
        if (!CreateProcessW(arguments.front().c_str(), mutable_command_line.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            throw std::runtime_error("cannot start recovery child process: " + std::to_string(GetLastError()));
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
    }

    ~ChildProcess() {
        if (process_ == nullptr) return;
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
        }
        CloseHandle(process_);
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void terminate() {
        expect(process_ != nullptr, "recovery child process is not running");
        expect(TerminateProcess(process_, 137) != FALSE,
            "cannot terminate recovery child process");
        expect(WaitForSingleObject(process_, 5000) == WAIT_OBJECT_0,
            "recovery child process did not terminate");
    }

    std::optional<DWORD> exit_code() const {
        DWORD code = STILL_ACTIVE;
        if (process_ == nullptr || GetExitCodeProcess(process_, &code) == FALSE || code == STILL_ACTIVE) {
            return std::nullopt;
        }
        return code;
    }

private:
    HANDLE process_{nullptr};
};

std::filesystem::path current_executable_path() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    expect(length > 0 && length < buffer.size(), "cannot resolve integration test executable path");
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

class LocalHttpTracker {
public:
    LocalHttpTracker() {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) throw std::runtime_error("cannot create tracker socket");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
            || ::listen(socket_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(socket_);
            throw std::runtime_error("cannot bind local tracker");
        }

        int length = sizeof(address);
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
            closesocket(socket_);
            throw std::runtime_error("cannot query local tracker port");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token stop_token) { serve(stop_token); });
    }

    ~LocalHttpTracker() {
        worker_.request_stop();
        ::shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        if (worker_.joinable()) worker_.join();
    }

    void set_peer_port(std::uint16_t port) { peer_port_.store(port); }
    std::uint32_t announce_count() const { return announce_count_.load(); }
    std::uint32_t observed_peer_count() const { return observed_peer_count_.load(); }
    std::uint16_t last_observed_peer_port() const { return last_observed_peer_port_.load(); }

    std::string announce_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/announce";
    }

private:
    void serve(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            const SOCKET client = ::accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            ++announce_count_;

            std::string request;
            char buffer[2048];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
                const int received = ::recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                request.append(buffer, static_cast<std::size_t>(received));
            }

            const auto announced_port = parse_announced_port(request);
            const auto peer_port = peer_port_.load();
            std::string body = "d8:intervali1e5:peers";
            if (announced_port != 0 && peer_port != 0 && announced_port != peer_port) {
                body += "6:";
                body.push_back(static_cast<char>(127));
                body.push_back(0);
                body.push_back(0);
                body.push_back(1);
                body.push_back(static_cast<char>((peer_port >> 8U) & 0xffU));
                body.push_back(static_cast<char>(peer_port & 0xffU));
                last_observed_peer_port_.store(announced_port);
                ++observed_peer_count_;
            } else {
                body += "0:";
            }
            body += 'e';

            const std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            send_all(client, headers);
            send_all(client, body);
            ::shutdown(client, SD_BOTH);
            closesocket(client);

        }
    }

    static std::uint16_t parse_announced_port(const std::string& request) {
        const auto marker = request.find("&port=");
        if (marker == std::string::npos) return 0;
        const auto begin = request.data() + marker + 6;
        const auto delimiter = request.find_first_of("& ", marker + 6);
        if (delimiter == std::string::npos) return 0;
        const auto end = request.data() + delimiter;
        unsigned int port = 0;
        const auto result = std::from_chars(begin, end, port);
        if (result.ec != std::errc{} || port > 65535) return 0;
        return static_cast<std::uint16_t>(port);
    }

    static void send_all(SOCKET socket, const std::string& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const int sent = ::send(socket, bytes.data() + offset,
                static_cast<int>(bytes.size() - offset), 0);
            if (sent <= 0) return;
            offset += static_cast<std::size_t>(sent);
        }
    }

    SOCKET socket_{INVALID_SOCKET};
    std::uint16_t port_{0};
    std::atomic<std::uint16_t> peer_port_{0};
    std::atomic<std::uint32_t> announce_count_{0};
    std::atomic<std::uint32_t> observed_peer_count_{0};
    std::atomic<std::uint16_t> last_observed_peer_port_{0};
    std::jthread worker_;
};

class LocalHttpFileServer {
public:
    explicit LocalHttpFileServer(std::vector<char> payload)
        : payload_(std::move(payload)) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) throw std::runtime_error("cannot create HTTP file socket");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
            || ::listen(socket_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(socket_);
            throw std::runtime_error("cannot bind HTTP file server");
        }
        int length = sizeof(address);
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
            closesocket(socket_);
            throw std::runtime_error("cannot query HTTP file server port");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token stop_token) { serve(stop_token); });
    }

    ~LocalHttpFileServer() {
        worker_.request_stop();
        ::shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        if (worker_.joinable()) worker_.join();
        for (auto& client : clients_) client.request_stop();
        clients_.clear();
    }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/payload.bin";
    }

    std::string redirect_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/redirect.bin";
    }

    std::string broken_redirect_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/broken-redirect.bin";
    }

    std::string no_range_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/no-range.bin";
    }

    std::string hash_url() const {
        return "http://127.0.0.1:" + std::to_string(port_)
            + "/a1b2c3d4e5f67890abcdef1234567890";
    }

    std::uint32_t range_requests() const { return range_requests_.load(); }
    std::uint32_t max_concurrent_requests() const {
        return max_concurrent_requests_.load();
    }

private:
    void serve(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            const SOCKET client = ::accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            clients_.emplace_back([this, client](std::stop_token client_stop) {
                serve_client(client, client_stop);
            });
        }
    }

    void serve_client(SOCKET client, std::stop_token stop_token) {
        struct ActiveGuard {
            explicit ActiveGuard(std::atomic<std::uint32_t>& value) : value_(value) {
                ++value_;
            }
            ~ActiveGuard() { --value_; }
            std::atomic<std::uint32_t>& value_;
        } guard(active_requests_);
        auto observed = active_requests_.load();
        auto maximum = max_concurrent_requests_.load();
        while (observed > maximum
            && !max_concurrent_requests_.compare_exchange_weak(maximum, observed)) {}

        std::string request;
        std::array<char, 2048> buffer{};
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int received = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) break;
            request.append(buffer.data(), static_cast<std::size_t>(received));
        }

        if (request.starts_with("GET /redirect.bin ")) {
            const std::string body = "redirect response body must not reach the output file";
            const std::string response = "HTTP/1.1 302 Found\r\nLocation: /payload.bin\r\n"
                "Content-Type: text/plain\r\nContent-Length: " + std::to_string(body.size())
                + "\r\nConnection: close\r\n\r\n" + body;
            send_all(client, response);
            ::shutdown(client, SD_BOTH);
            closesocket(client);
            return;
        }
        if (request.starts_with("GET /broken-redirect.bin ")) {
            const std::string body = "redirect without a location";
            const std::string response = "HTTP/1.1 302 Found\r\nContent-Type: text/plain\r\n"
                "Content-Length: " + std::to_string(body.size())
                + "\r\nConnection: close\r\n\r\n" + body;
            send_all(client, response);
            ::shutdown(client, SD_BOTH);
            closesocket(client);
            return;
        }

        const auto requested = requested_range(request);
        if (requested) ++range_requests_;
        const bool supports_range = !request.starts_with("GET /no-range.bin ");
        const auto start = requested && supports_range ? requested->first : 0;
        const auto end = requested && supports_range
            ? std::min(requested->second, payload_.size() - 1)
            : payload_.size() - 1;
        if (start >= payload_.size()) {
            send_all(client, "HTTP/1.1 416 Range Not Satisfiable\r\nConnection: close\r\n\r\n");
        } else {
            std::string headers = requested && supports_range
                ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
            headers += "Content-Type: application/octet-stream\r\n"
                "Accept-Ranges: bytes\r\nETag: \"integration-payload-v1\"\r\n"
                "Last-Modified: Sat, 22 Aug 2026 00:00:00 GMT\r\n";
            if (requested && supports_range) {
                headers += "Content-Range: bytes " + std::to_string(start) + "-"
                    + std::to_string(end) + "/" + std::to_string(payload_.size()) + "\r\n";
            }
            if (request.starts_with("GET /a1b2c3d4e5f67890abcdef1234567890 ")) {
                headers += "Content-Disposition: attachment; filename=\"Pretty Name.mkv\"; "
                    "filename*=UTF-8''Pretty%20Name.mkv\r\n";
            }
            headers += "Content-Length: " + std::to_string(end - start + 1)
                + "\r\nConnection: close\r\n\r\n";
            if (send_all(client, headers)) {
                send_payload(client, start, end + 1, stop_token);
            }
        }
        ::shutdown(client, SD_BOTH);
        closesocket(client);
    }

    static std::optional<std::pair<std::size_t, std::size_t>> requested_range(
        const std::string& request) {
        constexpr std::string_view marker = "\r\nRange: bytes=";
        const auto position = request.find(marker);
        if (position == std::string::npos) return std::nullopt;
        const auto begin = request.data() + position + marker.size();
        const auto dash = request.find('-', position + marker.size());
        if (dash == std::string::npos) return std::nullopt;
        const auto line_end = request.find("\r\n", dash);
        if (line_end == std::string::npos) return std::nullopt;
        std::size_t start = 0;
        std::size_t end = (std::numeric_limits<std::size_t>::max)();
        const auto parsed_start = std::from_chars(begin, request.data() + dash, start);
        if (parsed_start.ec != std::errc{}) return std::nullopt;
        if (line_end > dash + 1) {
            const auto parsed_end = std::from_chars(
                request.data() + dash + 1, request.data() + line_end, end);
            if (parsed_end.ec != std::errc{}) return std::nullopt;
        }
        return std::pair{start, end};
    }

    static bool send_all(SOCKET socket, const std::string& value) {
        std::size_t offset = 0;
        while (offset < value.size()) {
            const int sent = ::send(socket, value.data() + offset,
                static_cast<int>(value.size() - offset), 0);
            if (sent <= 0) return false;
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    void send_payload(SOCKET client, std::size_t offset, std::size_t end,
        std::stop_token stop_token) const {
        constexpr std::size_t chunk_size = 16 * 1024;
        while (offset < end && !stop_token.stop_requested()) {
            const auto count = std::min(chunk_size, end - offset);
            const int sent = ::send(client, payload_.data() + offset, static_cast<int>(count), 0);
            if (sent <= 0) return;
            offset += static_cast<std::size_t>(sent);
            std::this_thread::sleep_for(2ms);
        }
    }

    SOCKET socket_{INVALID_SOCKET};
    std::uint16_t port_{0};
    std::vector<char> payload_;
    std::atomic<std::uint32_t> range_requests_{0};
    std::atomic<std::uint32_t> active_requests_{0};
    std::atomic<std::uint32_t> max_concurrent_requests_{0};
    std::jthread worker_;
    std::vector<std::jthread> clients_;
};

void write_payload(const std::filesystem::path& path, std::size_t size, std::uint8_t seed) {
    std::filesystem::create_directories(path.parent_path());
    std::vector<char> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>((i * 37U + seed * 17U) & 0xffU);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    expect(static_cast<bool>(output), "cannot create integration test payload");
}

std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(static_cast<bool>(input), "downloaded payload is missing: " + path_utf8(path));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::shared_ptr<lt::torrent_info> create_torrent_file(
    const std::filesystem::path& payload,
    const std::filesystem::path& torrent_path,
    const std::string& announce_url,
    bool private_torrent = false) {
    lt::file_storage storage;
    lt::add_files(storage, path_utf8(payload));
    lt::create_torrent torrent(storage, 16 * 1024, lt::create_torrent::v1_only);
    if (!announce_url.empty()) torrent.add_tracker(announce_url);
    torrent.set_creator("bt_download integration test");
    torrent.set_priv(private_torrent);
    lt::error_code error;
    lt::set_piece_hashes(torrent, path_utf8(payload.parent_path()), error);
    expect(!error, "cannot hash integration torrent: " + error.message());

    const auto bytes = torrent.generate_buf();
    std::ofstream output(torrent_path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    expect(static_cast<bool>(output), "cannot write integration torrent");
    output.close();
    expect(static_cast<bool>(output), "cannot flush integration torrent");

    auto info = std::make_shared<lt::torrent_info>(path_utf8(torrent_path), error);
    expect(!error, "cannot reopen integration torrent: " + error.message());
    return info;
}

std::uint64_t create_oversized_torrent_file(const std::filesystem::path& torrent_path,
    const std::filesystem::path& save_path) {
    std::error_code error;
    const auto space = std::filesystem::space(save_path, error);
    expect(!error, "cannot query integration test free space: " + error.message());
    constexpr std::uint64_t headroom = 1024ULL * 1024ULL * 1024ULL;
    expect(space.available <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) - headroom,
        "integration test volume reports an unsupported amount of free space");
    const auto required = static_cast<std::int64_t>(space.available + headroom);

    lt::file_storage storage;
    storage.add_file("disk-full.bin", required);
    lt::create_torrent torrent(storage, 4 * 1024 * 1024, lt::create_torrent::v1_only);
    const auto placeholder_hash = (lt::sha1_hash::max)();
    for (int index = 0; index < torrent.num_pieces(); ++index) {
        torrent.set_hash(lt::piece_index_t{index}, placeholder_hash);
    }
    torrent.set_creator("bt_download disk-full integration test");

    const auto bytes = torrent.generate_buf();
    std::ofstream output(torrent_path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    expect(static_cast<bool>(output), "cannot flush oversized integration torrent");
    return static_cast<std::uint64_t>(required);
}

nlohmann::json dispatch_rpc(bt::Engine& engine, const std::string& method,
    const nlohmann::json& params) {
    std::istringstream input(nlohmann::json({
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params},
    }).dump() + "\n");
    std::ostringstream output;
    std::ostringstream log;
    bt::ProtocolServer server(input, output, log,
        [&engine](const std::string& request_method, const nlohmann::json& request_params) {
            return engine.dispatch(request_method, request_params);
        });
    expect(server.run() == 0, "integration JSON-RPC request failed to run");
    return nlohmann::json::parse(output.str());
}

std::string magnet_uri(const lt::torrent_info& info, const LocalHttpTracker& tracker) {
    std::ostringstream hash;
    expect(info.info_hashes().has_v1(), "integration torrent does not have a v1 info-hash");
    hash << info.info_hashes().v1;
    std::string encoded_tracker = tracker.announce_url();
    std::string::size_type position = 0;
    while ((position = encoded_tracker.find(':', position)) != std::string::npos) {
        encoded_tracker.replace(position, 1, "%3A");
        position += 3;
    }
    position = 0;
    while ((position = encoded_tracker.find('/', position)) != std::string::npos) {
        encoded_tracker.replace(position, 1, "%2F");
        position += 3;
    }
    return "magnet:?xt=urn:btih:" + hash.str() + "&tr=" + encoded_tracker;
}

lt::torrent_handle add_seed(lt::session& session, const std::shared_ptr<lt::torrent_info>& info,
    const std::filesystem::path& save_path) {
    lt::add_torrent_params add;
    add.ti = info;
    add.save_path = path_utf8(save_path);
    add.flags &= ~lt::torrent_flags::paused;
    add.flags &= ~lt::torrent_flags::auto_managed;
    lt::error_code error;
    auto handle = session.add_torrent(std::move(add), error);
    expect(!error, "cannot add local seed: " + error.message());
    return handle;
}

void wait_for_seeds(const std::vector<lt::torrent_handle>& handles) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (const auto& handle : handles) {
            const auto status = handle.status();
            if (status.errc) throw std::runtime_error("local seeder error: " + status.errc.message());
            ready = ready && status.is_seeding;
        }
        if (ready) return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("local seeder did not finish checking its payloads");
}

nlohmann::json wait_for_completion(bt::Engine& engine, const std::string& id, const LocalHttpTracker& tracker,
    const lt::torrent_handle& seed) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    nlohmann::json task;
    while (std::chrono::steady_clock::now() < deadline) {
        task = engine.dispatch("task.get", {{"id", id}}).at("task");
        if (task.at("state") == "completed") return task;
        if (task.at("state") == "error") {
            throw std::runtime_error("download entered error state: " + task.at("lastError").dump());
        }
        std::this_thread::sleep_for(100ms);
    }
    const auto seed_status = seed.status();
    throw std::runtime_error("local download timed out after " + std::to_string(tracker.announce_count())
        + " tracker announces and " + std::to_string(tracker.observed_peer_count())
        + " observed downloader ports (last " + std::to_string(tracker.last_observed_peer_port())
        + "); seeder peers=" + std::to_string(seed_status.num_peers)
        + ", uploads=" + std::to_string(seed_status.total_upload)
        + "; last task snapshot: " + task.dump());
}

nlohmann::json wait_for_state(bt::Engine& engine, const std::string& id, std::string_view expected,
    std::chrono::seconds timeout = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    nlohmann::json task;
    while (std::chrono::steady_clock::now() < deadline) {
        task = engine.dispatch("task.get", {{"id", id}}).at("task");
        if (task.at("state").get<std::string>() == expected) return task;
        if (task.at("state") == "error") {
            throw std::runtime_error("task entered error state: " + task.at("lastError").dump());
        }
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("task did not enter expected state " + std::string(expected)
        + "; last snapshot: " + task.dump());
}

std::string add_torrent_task(bt::Engine& engine, const std::filesystem::path& torrent,
    const std::filesystem::path& save_path) {
    const auto result = engine.dispatch("task.add", {
        {"source", {{"kind", "torrentFile"}, {"path", path_utf8(torrent)}}},
        {"savePath", path_utf8(save_path)},
        {"start", true},
    });
    return result.at("task").at("id").get<std::string>();
}

nlohmann::json wait_for_json_file(const std::filesystem::path& path, const ChildProcess& child) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto exit_code = child.exit_code()) {
            throw std::runtime_error("recovery child exited before its checkpoint with code "
                + std::to_string(*exit_code));
        }
        if (std::filesystem::is_regular_file(path)) {
            try {
                std::ifstream input(path, std::ios::binary);
                return nlohmann::json::parse(input);
            } catch (const nlohmann::json::exception&) {
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    throw std::runtime_error("recovery child did not publish a checkpoint");
}

void wait_for_recovered_checkpoint(bt::Engine& engine, const std::string& id,
    std::uint64_t checkpoint_bytes) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    nlohmann::json task;
    while (std::chrono::steady_clock::now() < deadline) {
        task = engine.dispatch("task.get", {{"id", id}}).at("task");
        if (task.at("state") == "error") {
            throw std::runtime_error("restored download entered error state: " + task.at("lastError").dump());
        }
        const auto state = task.at("state").get<std::string>();
        if ((state == "downloading" || state == "completed")
            && task.at("downloadedBytes").get<std::uint64_t>() >= checkpoint_bytes) {
            return;
        }
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("restored task regressed behind its persisted checkpoint: " + task.dump());
}

void wait_for_removal(const std::filesystem::path& payload) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::filesystem::exists(payload) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    expect(!std::filesystem::exists(payload), "deleteData did not remove the torrent payload");
}

std::optional<std::uint64_t> persisted_checkpoint_bytes(const std::filesystem::path& resume_path) {
    try {
        if (!std::filesystem::is_regular_file(resume_path)) return std::nullopt;
        std::ifstream input(resume_path, std::ios::binary);
        std::vector<char> bytes(std::istreambuf_iterator<char>(input), {});
        if (bytes.empty()) return std::nullopt;
        lt::error_code error;
        const auto resume = lt::read_resume_data(lt::span<const char>(bytes.data(), bytes.size()), error);
        if (error || !resume.ti) return std::nullopt;
        std::uint64_t checkpoint = 0;
        for (const auto index : resume.have_pieces.range()) {
            if (resume.have_pieces[index]) {
                checkpoint += static_cast<std::uint64_t>(resume.ti->piece_size(index));
            }
        }
        return checkpoint;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

int run_recovery_child(const std::filesystem::path& torrent_path,
    const std::filesystem::path& save_path, const std::filesystem::path& state_path,
    const std::filesystem::path& checkpoint_path) {
    bt::Engine engine([](const std::string&, const nlohmann::json&) {});
    engine.dispatch("engine.initialize", {
        {"protocolVersion", "1.4"},
        {"statePath", path_utf8(state_path)},
        {"config", {{"activeDownloads", 1}, {"downloadRateLimit", 512 * 1024}}},
    });
    const auto id = add_torrent_task(engine, torrent_path, save_path);
    nlohmann::json task;
    const auto progress_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < progress_deadline) {
        task = engine.dispatch("task.get", {{"id", id}}).at("task");
        const auto downloaded = task.at("downloadedBytes").get<std::uint64_t>();
        const auto total = task.at("totalBytes").get<std::uint64_t>();
        if (downloaded >= 384 * 1024 && downloaded < total) break;
        if (task.at("state") == "error") {
            throw std::runtime_error("recovery child download failed: " + task.at("lastError").dump());
        }
        std::this_thread::sleep_for(50ms);
    }
    const auto observed_bytes = task.at("downloadedBytes").get<std::uint64_t>();
    expect(observed_bytes >= 384 * 1024 && observed_bytes < task.at("totalBytes").get<std::uint64_t>(),
        "recovery child did not reach partial progress");

    const auto resume_path = state_path / "resume" / (id + ".fastresume");
    engine.dispatch("task.pause", {{"id", id}});
    const auto resume_deadline = std::chrono::steady_clock::now() + 10s;
    std::uint64_t checkpoint_bytes = 0;
    while (std::chrono::steady_clock::now() < resume_deadline) {
        checkpoint_bytes = persisted_checkpoint_bytes(resume_path).value_or(0);
        if (checkpoint_bytes >= 384 * 1024) break;
        std::this_thread::sleep_for(50ms);
    }
    expect(checkpoint_bytes >= 384 * 1024,
        "recovery child did not persist completed pieces at its checkpoint");

    engine.dispatch("task.resume", {{"id", id}});
    {
        std::ofstream output(checkpoint_path, std::ios::binary | std::ios::trunc);
        output << nlohmann::json({{"id", id}, {"checkpointBytes", checkpoint_bytes}}).dump();
        output.flush();
        expect(static_cast<bool>(output), "recovery child cannot publish its checkpoint");
    }
    while (true) std::this_thread::sleep_for(1s);
}

void run_http_download_integration_test() {
    WinsockRuntime winsock;
    TemporaryDirectory temporary;
    std::vector<char> payload(2 * 1024 * 1024 + 137);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>((index * 29U + 17U) & 0xffU);
    }
    LocalHttpFileServer server(payload);
    const auto state_path = temporary.path() / "http-state";
    const auto save_path = temporary.path() / "http-download";
    std::filesystem::create_directories(save_path);

    bt::Engine engine([](const std::string&, const nlohmann::json&) {});
    engine.dispatch("engine.initialize", {
        {"protocolVersion", "1.4"},
        {"statePath", path_utf8(state_path)},
        {"config", {{"activeDownloads", 1}, {"downloadRateLimit", 512 * 1024}}},
    });
    expect(engine.dispatch("engine.status", nlohmann::json::object())
            .at("features").dump().find("httpMultiConnection") != std::string::npos,
        "engine.status did not advertise HTTP multi-connection support");
    const auto added = engine.dispatch("task.add", {
        {"source", {{"kind", "http"}, {"url", server.url()}}},
        {"savePath", path_utf8(save_path)},
        {"start", true},
    });
    const auto id = added.at("task").at("id").get<std::string>();
    expect(added.at("task").at("sourceKind") == "http", "HTTP task source kind was not retained");
    expect(added.at("task").at("infoHash").is_null(), "HTTP task unexpectedly exposed an info-hash");

    nlohmann::json partial;
    nlohmann::json partial_details;
    const auto partial_deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < partial_deadline) {
        partial = engine.dispatch("task.get", {{"id", id}}).at("task");
        if (partial.at("state") == "error") {
            throw std::runtime_error("HTTP download entered error state: " + partial.at("lastError").dump());
        }
        const auto downloaded = partial.at("downloadedBytes").get<std::uint64_t>();
        const auto total = partial.at("totalBytes").get<std::uint64_t>();
        if (downloaded > 0 && total > downloaded) {
            partial_details = engine.dispatch("task.details", {{"id", id}});
            const auto pieces = partial_details.at("completedPieces").get<std::string>();
            if (pieces.find('1') != std::string::npos
                && pieces.find('0') != std::string::npos
                && partial_details.at("httpConnections").get<std::size_t>() > 1) {
                break;
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    expect(partial.at("downloadedBytes").get<std::uint64_t>() > 0,
        "HTTP task did not report partial progress");
    const auto proxy_update = engine.dispatch("engine.configureProxy", {
        {"enabled", true},
        {"httpProxy", "http://127.0.0.1:1"},
        {"httpsProxy", "http://127.0.0.1:1"},
        {"bypass", nlohmann::json::array({"127.0.0.1"})},
        {"peerProxy", {{"host", "127.0.0.1"}, {"port", 1}}},
    });
    expect(proxy_update.at("proxy").at("enabled") == true,
        "runtime proxy update was not applied");
    expect(engine.dispatch("engine.status", nlohmann::json::object())
            .at("proxy").at("peerConfigured") == true,
        "engine status did not report the configured peer proxy");
    const auto paused = engine.dispatch("task.pause", {{"id", id}}).at("task");
    expect(paused.at("state") == "paused", "HTTP task did not pause");
    expect(paused.at("downloadRate") == 0, "paused HTTP task retained a download rate");

    const auto details = engine.dispatch("task.details", {{"id", id}});
    expect(details.at("totalFiles") == 1 && details.at("totalPeers") == 0,
        "HTTP task overview did not expose single-file semantics");
    const auto piece_count = details.at("pieceCount").get<std::uint64_t>();
    const auto completed_pieces = details.at("completedPieces").get<std::string>();
    expect(details.at("pieceLength").get<std::uint64_t>() >= 256 * 1024
            && piece_count > 1 && piece_count <= 256
            && completed_pieces.size() == piece_count,
        "HTTP task overview did not expose bounded byte-range pieces");
    expect(completed_pieces.find('1') != std::string::npos
            && completed_pieces.find('0') != std::string::npos,
        "partial HTTP task did not expose both completed and pending byte ranges");
    expect(server.max_concurrent_requests() > 1,
        "HTTP task did not issue concurrent range requests");
    const auto partial_path = save_path
        / ("payload.bin." + id + ".part");
    const auto persisted_progress = bt::read_http_transfer_progress(partial_path);
    expect(persisted_progress && persisted_progress->ranges.size() > 1
            && persisted_progress->downloaded_bytes > 0
            && persisted_progress->downloaded_bytes < persisted_progress->total_bytes,
        "paused HTTP task did not persist its range progress");
    const auto files = engine.dispatch("task.files", {{"id", id}});
    expect(files.at("files").size() == 1
            && files.at("files").at(0).at("path") == "payload.bin",
        "HTTP task file details are incorrect");
    const auto peers = engine.dispatch("task.peers", {{"id", id}});
    expect(peers.at("peers").empty(), "HTTP task unexpectedly exposed peers");

    engine.dispatch("task.resume", {{"id", id}});
    nlohmann::json completed;
    const auto completion_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < completion_deadline) {
        completed = engine.dispatch("task.get", {{"id", id}}).at("task");
        if (completed.at("state") == "completed") break;
        if (completed.at("state") == "error") {
            throw std::runtime_error("resumed HTTP download failed: " + completed.at("lastError").dump());
        }
        std::this_thread::sleep_for(50ms);
    }
    expect(completed.at("state") == "completed", "HTTP task did not complete");
    expect(completed.at("downloadedBytes") == completed.at("totalBytes"),
        "HTTP task did not report complete byte counts");
    const auto completed_details = engine.dispatch("task.details", {{"id", id}});
    const auto final_pieces = completed_details.at("completedPieces").get<std::string>();
    expect(!final_pieces.empty()
            && final_pieces.find_first_not_of('1') == std::string::npos,
        "completed HTTP task retained incomplete byte ranges");
    expect(read_bytes(save_path / "payload.bin") == payload,
        "HTTP payload differs from the server response");
    expect(server.range_requests() > 0, "resumed HTTP task did not issue a Range request");

    const auto fallback_path = temporary.path() / "http-no-range";
    std::filesystem::create_directories(fallback_path);
    const auto fallback_id = engine.dispatch("task.add", {
        {"source", {{"kind", "http"}, {"url", server.no_range_url()}}},
        {"savePath", path_utf8(fallback_path)},
    }).at("task").at("id").get<std::string>();
    nlohmann::json fallback_completed;
    const auto fallback_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < fallback_deadline) {
        fallback_completed = engine.dispatch(
            "task.get", {{"id", fallback_id}}).at("task");
        if (fallback_completed.at("state") == "completed") break;
        if (fallback_completed.at("state") == "error") {
            throw std::runtime_error("HTTP no-range fallback failed: "
                + fallback_completed.at("lastError").dump());
        }
        std::this_thread::sleep_for(50ms);
    }
    expect(fallback_completed.at("state") == "completed",
        "HTTP task did not fall back when Range was unsupported");
    expect(read_bytes(fallback_path / "no-range.bin") == payload,
        "HTTP no-range fallback payload differs from the server response");
    auto fallback_state = fallback_path
        / ("no-range.bin." + fallback_id + ".part.ranges.json");
    expect(!std::filesystem::exists(fallback_state),
        "HTTP no-range fallback retained segmented state after completion");
    engine.dispatch("task.remove", {{"id", fallback_id}, {"deleteData", true}});

    bool duplicate_rejected = false;
    try {
        engine.dispatch("task.add", {
            {"source", {{"kind", "http"}, {"url", server.url()}}},
            {"savePath", path_utf8(save_path)},
        });
    } catch (const std::exception& exception) {
        duplicate_rejected = std::string(exception.what()).find("already exists") != std::string::npos;
    }
    expect(duplicate_rejected, "duplicate HTTP URL was accepted at the same save path");

    const auto redirect_path = temporary.path() / "http-redirect";
    std::filesystem::create_directories(redirect_path);
    const auto redirect_task = engine.dispatch("task.add", {
        {"source", {{"kind", "http"}, {"url", server.redirect_url()}}},
        {"savePath", path_utf8(redirect_path)},
    }).at("task");
    const auto redirect_id = redirect_task.at("id").get<std::string>();
    nlohmann::json redirect_completed;
    const auto redirect_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < redirect_deadline) {
        redirect_completed = engine.dispatch("task.get", {{"id", redirect_id}}).at("task");
        if (redirect_completed.at("state") == "completed") break;
        if (redirect_completed.at("state") == "error") {
            throw std::runtime_error(
                "redirected HTTP download failed: " + redirect_completed.at("lastError").dump());
        }
        std::this_thread::sleep_for(50ms);
    }
    expect(redirect_completed.at("state") == "completed",
        "redirected HTTP task did not complete");
    expect(read_bytes(redirect_path / "payload.bin") == payload,
        "HTTP redirect response body polluted the downloaded payload");
    expect(redirect_completed.at("displayName") == "payload.bin",
        "redirected HTTP task did not take the final URL filename");
    engine.dispatch("task.remove", {{"id", redirect_id}, {"deleteData", true}});

    const auto broken_redirect = engine.dispatch("task.add", {
        {"source", {{"kind", "http"}, {"url", server.broken_redirect_url()}}},
        {"savePath", path_utf8(redirect_path)},
    }).at("task");
    const auto broken_redirect_id = broken_redirect.at("id").get<std::string>();
    nlohmann::json broken_redirect_failed;
    const auto broken_redirect_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < broken_redirect_deadline) {
        broken_redirect_failed = engine.dispatch(
            "task.get", {{"id", broken_redirect_id}}).at("task");
        if (broken_redirect_failed.at("state") == "error") break;
        std::this_thread::sleep_for(50ms);
    }
    expect(broken_redirect_failed.at("state") == "error"
            && broken_redirect_failed.at("lastError").at("code") == "HTTP_REDIRECT_ERROR",
        "HTTP redirect without Location was not rejected");
    engine.dispatch("task.remove", {{"id", broken_redirect_id}, {"deleteData", true}});

    const auto named_path = temporary.path() / "http-named";
    std::filesystem::create_directories(named_path);
    const auto named_added = engine.dispatch("task.add", {
        {"source", {{"kind", "http"}, {"url", server.hash_url()}}},
        {"savePath", path_utf8(named_path)},
    }).at("task");
    expect(named_added.at("displayName") == "a1b2c3d4e5f67890abcdef1234567890",
        "HTTP task did not start with the URL filename");
    const auto named_id = named_added.at("id").get<std::string>();
    nlohmann::json named_completed;
    const auto named_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < named_deadline) {
        named_completed = engine.dispatch("task.get", {{"id", named_id}}).at("task");
        if (named_completed.at("state") == "completed") break;
        if (named_completed.at("state") == "error") {
            throw std::runtime_error("HTTP Content-Disposition download failed: "
                + named_completed.at("lastError").dump());
        }
        std::this_thread::sleep_for(50ms);
    }
    expect(named_completed.at("state") == "completed"
            && named_completed.at("displayName") == "Pretty Name.mkv",
        "HTTP task did not adopt the Content-Disposition filename");
    expect(read_bytes(named_path / "Pretty Name.mkv") == payload,
        "HTTP Content-Disposition payload was not saved under the header filename");
    const auto named_files = engine.dispatch("task.files", {{"id", named_id}});
    expect(named_files.at("files").at(0).at("path") == "Pretty Name.mkv",
        "HTTP task file list retained the URL hash name");

    const auto custom_path = temporary.path() / "http-custom-name";
    std::filesystem::create_directories(custom_path);
    const auto custom_id = engine.dispatch("task.add", {
        {"source", {{"kind", "http"}, {"url", server.hash_url()}}},
        {"savePath", path_utf8(custom_path)},
        {"displayName", "Custom Task"},
    }).at("task").at("id").get<std::string>();
    nlohmann::json custom_completed;
    const auto custom_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < custom_deadline) {
        custom_completed = engine.dispatch("task.get", {{"id", custom_id}}).at("task");
        if (custom_completed.at("state") == "completed") break;
        if (custom_completed.at("state") == "error") {
            throw std::runtime_error("HTTP custom displayName download failed: "
                + custom_completed.at("lastError").dump());
        }
        std::this_thread::sleep_for(50ms);
    }
    expect(custom_completed.at("state") == "completed"
            && custom_completed.at("displayName") == "Custom Task",
        "user-provided HTTP displayName was overwritten");
    expect(read_bytes(custom_path / "Pretty Name.mkv") == payload,
        "HTTP file was not saved under the Content-Disposition name when displayName was set");
    engine.dispatch("task.remove", {{"id", named_id}, {"deleteData", true}});
    engine.dispatch("task.remove", {{"id", custom_id}, {"deleteData", true}});
    engine.dispatch("engine.shutdown", nlohmann::json::object());

    bt::Engine restored([](const std::string&, const nlohmann::json&) {});
    restored.dispatch("engine.initialize", {
        {"protocolVersion", "1.4"}, {"statePath", path_utf8(state_path)}});
    const auto recovered = restored.dispatch("task.get", {{"id", id}}).at("task");
    expect(recovered.at("state") == "completed", "completed HTTP task was not restored");
    restored.dispatch("task.remove", {{"id", id}, {"deleteData", true}});
    expect(!std::filesystem::exists(save_path / "payload.bin"),
        "HTTP deleteData did not remove the target file");
    restored.dispatch("engine.shutdown", nlohmann::json::object());
}

void run_download_integration_test() {
    WinsockRuntime winsock;
    TemporaryDirectory temporary;
    LocalHttpTracker tracker;

    const auto seed_root = temporary.path() / "seed";
    const auto single_source = seed_root / "single.bin";
    const auto bundle_source = seed_root / "bundle";
    const auto magnet_source = seed_root / "magnet.bin";
    const auto private_source = seed_root / "private.bin";
    const auto recovery_source = seed_root / "recovery.bin";
    const auto select_source = seed_root / "select";
    write_payload(single_source, 180 * 1024 + 73, 11);
    write_payload(bundle_source / "first.bin", 96 * 1024 + 19, 23);
    write_payload(bundle_source / "nested" / "second.bin", 144 * 1024 + 41, 31);
    write_payload(magnet_source, 192 * 1024 + 29, 47);
    write_payload(private_source, 224 * 1024 + 61, 53);
    write_payload(recovery_source, 4 * 1024 * 1024 + 113, 59);
    write_payload(select_source / "keep.bin", 128 * 1024 + 7, 61);
    write_payload(select_source / "skip.bin", 64 * 1024 + 3, 67);

    const auto single_torrent_path = temporary.path() / "single.torrent";
    const auto bundle_torrent_path = temporary.path() / "bundle.torrent";
    const auto magnet_torrent_path = temporary.path() / "magnet.torrent";
    const auto private_torrent_path = temporary.path() / "private.torrent";
    const auto recovery_torrent_path = temporary.path() / "recovery.torrent";
    const auto select_torrent_path = temporary.path() / "select.torrent";
    const auto single_info = create_torrent_file(single_source, single_torrent_path, tracker.announce_url());
    const auto bundle_info = create_torrent_file(bundle_source, bundle_torrent_path, tracker.announce_url());
    const auto magnet_info = create_torrent_file(magnet_source, magnet_torrent_path, tracker.announce_url());
    const auto private_info = create_torrent_file(
        private_source, private_torrent_path, tracker.announce_url(), true);
    const auto recovery_info = create_torrent_file(recovery_source, recovery_torrent_path, tracker.announce_url());
    const auto select_info = create_torrent_file(select_source, select_torrent_path, tracker.announce_url());

    lt::settings_pack seed_settings;
    seed_settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    seed_settings.set_bool(lt::settings_pack::enable_dht, false);
    seed_settings.set_bool(lt::settings_pack::enable_lsd, false);
    seed_settings.set_bool(lt::settings_pack::enable_upnp, false);
    seed_settings.set_bool(lt::settings_pack::enable_natpmp, false);
    lt::session seeder(seed_settings);
    expect(seeder.listen_port() != 0, "local seeder did not open a listening port");
    tracker.set_peer_port(seeder.listen_port());
    const auto single_seed = add_seed(seeder, single_info, seed_root);
    const auto bundle_seed = add_seed(seeder, bundle_info, seed_root);
    const auto magnet_seed = add_seed(seeder, magnet_info, seed_root);
    const auto private_seed = add_seed(seeder, private_info, seed_root);
    const auto recovery_seed = add_seed(seeder, recovery_info, seed_root);
    const auto select_seed = add_seed(seeder, select_info, seed_root);
    expect(single_seed.is_valid() && bundle_seed.is_valid() && magnet_seed.is_valid()
            && private_seed.is_valid() && recovery_seed.is_valid() && select_seed.is_valid(),
        "local seeder handle is invalid");
    wait_for_seeds({single_seed, bundle_seed, magnet_seed, private_seed, recovery_seed, select_seed});
    bt::Engine engine([](const std::string&, const nlohmann::json&) {});
    engine.dispatch("engine.initialize", {
        {"protocolVersion", "1.4"},
        {"statePath", path_utf8(temporary.path() / "state")},
        {"config", {{"activeDownloads", 3}, {"metadataTimeoutSeconds", 15}}},
    });

    const auto single_download = temporary.path() / "single-download";
    const auto bundle_download = temporary.path() / "bundle-download";
    const auto magnet_download = temporary.path() / "magnet-download";
    const auto private_download = temporary.path() / "private-download";
    const auto disk_full_download = temporary.path() / "disk-full-download";
    const auto select_download = temporary.path() / "select-download";
    std::filesystem::create_directories(single_download);
    std::filesystem::create_directories(bundle_download);
    std::filesystem::create_directories(magnet_download);
    std::filesystem::create_directories(private_download);
    std::filesystem::create_directories(disk_full_download);
    std::filesystem::create_directories(select_download);

    const auto single_id = add_torrent_task(engine, single_torrent_path, single_download);
    const auto single_task = wait_for_completion(engine, single_id, tracker, single_seed);
    expect(single_task.at("downloadedBytes") == single_task.at("totalBytes"),
        "single-file task did not report complete byte counts");
    expect(read_bytes(single_source) == read_bytes(single_download / "single.bin"),
        "single-file payload differs from seed");

    const auto duplicate_download = temporary.path() / "duplicate-download";
    std::filesystem::create_directories(duplicate_download);
    bool duplicate_rejected = false;
    try {
        (void)add_torrent_task(engine, single_torrent_path, duplicate_download);
    } catch (const std::exception& exception) {
        duplicate_rejected = std::string(exception.what()).find("already active") != std::string::npos;
    }
    expect(duplicate_rejected, "same info-hash at a second save path was silently aliased");

    const auto bundle_id = add_torrent_task(engine, bundle_torrent_path, bundle_download);
    const auto bundle_task = wait_for_completion(engine, bundle_id, tracker, bundle_seed);
    expect(bundle_task.at("downloadedBytes") == bundle_task.at("totalBytes"),
        "multi-file task did not report complete byte counts");
    expect(read_bytes(bundle_source / "first.bin") == read_bytes(bundle_download / "bundle" / "first.bin"),
        "first multi-file payload differs from seed");
    expect(read_bytes(bundle_source / "nested" / "second.bin")
            == read_bytes(bundle_download / "bundle" / "nested" / "second.bin"),
        "nested multi-file payload differs from seed");

    const auto remove_result = engine.dispatch("task.remove", {{"id", single_id}, {"deleteData", false}});
    expect(remove_result.at("removed") == true && remove_result.at("dataDeleted") == false,
        "completed single-file task was not removed while preserving its payload");

    const auto magnet_result = engine.dispatch("task.add", {
        {"source", {{"kind", "magnet"}, {"uri", magnet_uri(*magnet_info, tracker)}}},
        {"savePath", path_utf8(magnet_download)},
        {"start", true},
    });
    expect(magnet_result.at("task").at("state") == "metadata", "magnet task did not enter metadata state");
    const auto magnet_id = magnet_result.at("task").at("id").get<std::string>();
    const auto magnet_task = wait_for_completion(engine, magnet_id, tracker, magnet_seed);
    expect(magnet_task.at("downloadedBytes") == magnet_task.at("totalBytes"),
        "magnet task did not report complete byte counts");
    expect(read_bytes(magnet_source) == read_bytes(magnet_download / "magnet.bin"),
        "magnet payload differs from seed");

    const auto select_result = engine.dispatch("task.add", {
        {"source", {{"kind", "torrentFile"}, {"path", path_utf8(select_torrent_path)}}},
        {"savePath", path_utf8(select_download)}, {"start", false}});
    const auto select_id = select_result.at("task").at("id").get<std::string>();
    const auto set_priorities = engine.dispatch("task.setFilePriorities",
        {{"id", select_id}, {"priorities", {{"1", 0}}}});
    expect(set_priorities.at("priorities") == nlohmann::json::array({4, 0}),
        "file selection returned the wrong priority vector");
    engine.dispatch("task.resume", {{"id", select_id}});
    const auto select_task = wait_for_completion(engine, select_id, tracker, select_seed);
    expect(std::filesystem::is_regular_file(select_download / "select" / "keep.bin"),
        "selected file was not downloaded");
    expect(!std::filesystem::exists(select_download / "select" / "skip.bin"),
        "skipped file was downloaded despite priority 0");
    expect(read_bytes(select_source / "keep.bin")
            == read_bytes(select_download / "select" / "keep.bin"),
        "selected file payload differs from seed");
    const auto select_details = engine.dispatch("task.files", {{"id", select_id}});
    const auto& select_files = select_details.at("files");
    expect(select_files.size() == 2 && select_files[0].at("priority") == 4
            && select_files[1].at("priority") == 0,
        "completed file selection did not expose priorities");
    const auto completed_set = dispatch_rpc(engine, "task.setFilePriorities",
        {{"id", bundle_id}, {"priorities", {{"0", 0}}}});
    expect(completed_set.at("error").at("data").at("code") == "TASK_UNAVAILABLE",
        "completed task accepted file priority changes");

    const auto private_id = add_torrent_task(engine, private_torrent_path, private_download);
    const auto private_task = wait_for_completion(engine, private_id, tracker, private_seed);
    expect(private_task.at("private") == true, "private torrent flag was not exposed in the task snapshot");
    expect(read_bytes(private_source) == read_bytes(private_download / "private.bin"),
        "private torrent payload differs from seed");

    const auto disk_full_torrent_path = temporary.path() / "disk-full.torrent";
    const auto required_bytes = create_oversized_torrent_file(disk_full_torrent_path, disk_full_download);
    const auto disk_full_response = dispatch_rpc(engine, "task.add", {
        {"source", {{"kind", "torrentFile"}, {"path", path_utf8(disk_full_torrent_path)}}},
        {"savePath", path_utf8(disk_full_download)},
        {"start", true},
    });
    expect(disk_full_response.at("error").at("code") == -32012,
        "disk-full preflight returned the wrong JSON-RPC code");
    const auto& disk_full_data = disk_full_response.at("error").at("data");
    expect(disk_full_data.at("code") == "DISK_FULL", "disk-full preflight returned the wrong business code");
    expect(disk_full_data.at("retryable") == true, "disk-full preflight must be retryable");
    expect(disk_full_data.at("requiredBytes").get<std::uint64_t>() == required_bytes,
        "disk-full preflight reported the wrong required byte count");
    expect(disk_full_data.at("requiredBytes").get<std::uint64_t>()
            > disk_full_data.at("availableBytes").get<std::uint64_t>(),
        "disk-full preflight did not report a real capacity deficit");

    engine.dispatch("engine.shutdown", nlohmann::json::object());

    const auto recovery_download = temporary.path() / "recovery-download";
    const auto recovery_state = temporary.path() / "recovery-state";
    const auto recovery_checkpoint = temporary.path() / "recovery-checkpoint.json";
    const auto unrelated_file = recovery_download / "keep.me";
    std::filesystem::create_directories(recovery_download);
    write_payload(unrelated_file, 37, 71);
    ChildProcess recovery_child({current_executable_path(), "--recovery-child", recovery_torrent_path,
        recovery_download, recovery_state, recovery_checkpoint});
    const auto checkpoint = wait_for_json_file(recovery_checkpoint, recovery_child);
    const auto recovery_id = checkpoint.at("id").get<std::string>();
    const auto checkpoint_bytes = checkpoint.at("checkpointBytes").get<std::uint64_t>();
    recovery_child.terminate();

    bt::Engine restored([](const std::string&, const nlohmann::json&) {});
    const auto initialized = restored.dispatch("engine.initialize", {
        {"protocolVersion", "1.4"}, {"statePath", path_utf8(recovery_state)}});
    expect(initialized.at("restoredTasks") == 1, "forced termination task was not restored");
    restored.dispatch("engine.configure", {{"downloadRateLimit", 1024}});
    wait_for_recovered_checkpoint(restored, recovery_id, checkpoint_bytes);
    restored.dispatch("engine.configure", {{"downloadRateLimit", 512 * 1024}});
    const auto recovered_task = wait_for_completion(restored, recovery_id, tracker, recovery_seed);
    expect(recovered_task.at("downloadedBytes") == recovered_task.at("totalBytes"),
        "restored task did not report complete byte counts");
    expect(read_bytes(recovery_source) == read_bytes(recovery_download / "recovery.bin"),
        "restored payload differs from seed");

    const auto remove_recovered = restored.dispatch("task.remove",
        {{"id", recovery_id}, {"deleteData", true}});
    expect(remove_recovered.at("removed") == true && remove_recovered.at("dataDeleted") == true,
        "restored task did not accept explicit data deletion");
    wait_for_removal(recovery_download / "recovery.bin");
    expect(std::filesystem::is_regular_file(unrelated_file),
        "deleteData crossed the torrent payload boundary");
    restored.dispatch("engine.shutdown", nlohmann::json::object());
}

void run_tracker_and_seeding_integration_test() {
    WinsockRuntime winsock;
    TemporaryDirectory temporary;
    LocalHttpTracker origin_tracker;
    LocalHttpTracker supplemental_tracker;
    LocalHttpTracker private_spy_tracker;

    const auto seed_root = temporary.path() / "seed";
    const auto public_source = seed_root / "supplemental.bin";
    const auto private_source = seed_root / "private-supplemental.bin";
    const auto seeding_source = seed_root / "limited-seeding.bin";
    write_payload(public_source, 384 * 1024 + 17, 83);
    write_payload(private_source, 320 * 1024 + 23, 89);
    write_payload(seeding_source, 1024 * 1024 + 31, 97);

    const auto public_torrent_path = temporary.path() / "supplemental.torrent";
    const auto private_torrent_path = temporary.path() / "private-supplemental.torrent";
    const auto seeding_torrent_path = temporary.path() / "limited-seeding.torrent";
    const auto public_info = create_torrent_file(public_source, public_torrent_path, "");
    const auto private_info = create_torrent_file(
        private_source, private_torrent_path, origin_tracker.announce_url(), true);
    const auto seeding_info = create_torrent_file(
        seeding_source, seeding_torrent_path, origin_tracker.announce_url());

    lt::settings_pack peer_settings;
    peer_settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    peer_settings.set_bool(lt::settings_pack::enable_dht, false);
    peer_settings.set_bool(lt::settings_pack::enable_lsd, false);
    peer_settings.set_bool(lt::settings_pack::enable_upnp, false);
    peer_settings.set_bool(lt::settings_pack::enable_natpmp, false);
    lt::session seeder(peer_settings);
    expect(seeder.listen_port() != 0, "Tracker/seeding test seeder did not listen");
    origin_tracker.set_peer_port(seeder.listen_port());
    supplemental_tracker.set_peer_port(seeder.listen_port());
    const auto public_seed = add_seed(seeder, public_info, seed_root);
    const auto private_seed = add_seed(seeder, private_info, seed_root);
    const auto limited_seed = add_seed(seeder, seeding_info, seed_root);
    wait_for_seeds({public_seed, private_seed, limited_seed});

    {
        const auto download = temporary.path() / "supplemental-download";
        std::filesystem::create_directories(download);
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.4"}, {"statePath", path_utf8(temporary.path() / "tracker-state")}});
        const auto id = add_torrent_task(engine, public_torrent_path, download);
        std::this_thread::sleep_for(500ms);
        engine.dispatch("engine.configure", {
            {"additionalTrackers", nlohmann::json::array({supplemental_tracker.announce_url()})}});
        const auto task = wait_for_completion(engine, id, supplemental_tracker, public_seed);
        expect(task.at("seedStopReason") == "disabled",
            "safe default did not stop seeding");
        expect(supplemental_tracker.announce_count() > 0,
            "dynamically configured supplemental Tracker was not announced");
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }

    {
        const auto download = temporary.path() / "private-supplemental-download";
        std::filesystem::create_directories(download);
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.4"}, {"statePath", path_utf8(temporary.path() / "private-tracker-state")},
            {"config", {{"additionalTrackers", nlohmann::json::array({private_spy_tracker.announce_url()})}}}});
        const auto observed_before = origin_tracker.observed_peer_count();
        const auto added = engine.dispatch("task.add", {
            {"source", {{"kind", "magnet"}, {"uri", magnet_uri(*private_info, origin_tracker)}}},
            {"savePath", path_utf8(download)}, {"start", true}});
        expect(added.at("task").at("state") == "metadata",
            "private Magnet did not start in metadata state");
        const auto id = added.at("task").at("id").get<std::string>();
        const auto announce_deadline = std::chrono::steady_clock::now() + 5s;
        while (origin_tracker.observed_peer_count() == observed_before
            && std::chrono::steady_clock::now() < announce_deadline) {
            std::this_thread::sleep_for(100ms);
        }
        expect(origin_tracker.observed_peer_count() > observed_before,
            "private Magnet did not announce to its embedded origin Tracker");
        expect(private_spy_tracker.announce_count() == 0,
            "metadata-pending Magnet leaked its info-hash to a supplemental Tracker");
        engine.dispatch("task.remove", {{"id", id}, {"deleteData", false}});

        const auto private_id = add_torrent_task(engine, private_torrent_path, download);
        const auto task = wait_for_completion(engine, private_id, origin_tracker, private_seed);
        expect(task.at("private") == true, "private supplemental Tracker test lost private flag");
        std::this_thread::sleep_for(500ms);
        expect(private_spy_tracker.announce_count() == 0,
            "private torrent leaked its info-hash to a supplemental Tracker");
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }

    {
        const auto download = temporary.path() / "limited-seeding-download";
        const auto leecher_download = temporary.path() / "limited-seeding-leecher";
        const auto seeding_state_path = temporary.path() / "seeding-state";
        std::filesystem::create_directories(download);
        std::filesystem::create_directories(leecher_download);
        std::string id;
        std::uint16_t first_engine_peer_port = 0;
        std::atomic_bool automatic_recheck_saw_checking{false};
        {
            bt::Engine engine([&automatic_recheck_saw_checking](const std::string& event,
                const nlohmann::json& params) {
                if (event == "event.taskUpdated" && params.contains("task")) {
                    if (params.at("task").at("state") == "checking") {
                        automatic_recheck_saw_checking.store(true);
                    }
                }
            });
            engine.dispatch("engine.initialize", {
                {"protocolVersion", "1.4"}, {"statePath", path_utf8(seeding_state_path)},
                {"config", {{"seedingEnabled", true}, {"seedRatioLimit", 0.1},
                    {"seedTimeLimitMinutes", 0}}}});
            id = add_torrent_task(engine, seeding_torrent_path, download);
            const auto seeding_task = wait_for_state(engine, id, "seeding");
            expect(seeding_task.at("seedStopReason").is_null(),
                "limited seeding task stopped before a limit was reached");
            first_engine_peer_port = origin_tracker.last_observed_peer_port();
            expect(first_engine_peer_port != 0 && first_engine_peer_port != seeder.listen_port(),
                "Tracker did not observe the engine peer port");

            const auto seeding_payload = download / "limited-seeding.bin";
            std::error_code remove_error;
            expect(std::filesystem::remove(seeding_payload, remove_error) && !remove_error,
                "could not remove the active seeding payload for automatic recovery");
            origin_tracker.set_peer_port(seeder.listen_port());
            automatic_recheck_saw_checking.store(false);
            const auto repair_deadline = std::chrono::steady_clock::now() + 30s;
            nlohmann::json repaired;
            while (std::chrono::steady_clock::now() < repair_deadline) {
                repaired = engine.dispatch("task.get", {{"id", id}}).at("task");
                std::error_code payload_error;
                const auto payload_size = std::filesystem::is_regular_file(seeding_payload)
                    ? std::filesystem::file_size(seeding_payload, payload_error)
                    : 0;
                if (automatic_recheck_saw_checking.load() && repaired.at("state") == "seeding"
                    && !payload_error && payload_size == std::filesystem::file_size(seeding_source)) {
                    break;
                }
                std::this_thread::sleep_for(100ms);
            }
            expect(automatic_recheck_saw_checking.load(),
                "deleting an active seeding payload did not trigger automatic checking");
            expect(repaired.at("state") == "seeding",
                "automatically repaired seeding task did not return to seeding");
            expect(std::filesystem::is_regular_file(seeding_payload),
                "automatic seeding recovery did not redownload the payload");
            expect(read_bytes(seeding_payload) == read_bytes(seeding_source),
                "automatically rechecked payload differs from the original seed");

            const auto paused = engine.dispatch("task.pause", {{"id", id}}).at("task");
            expect(paused.at("state") == "paused", "seeding task did not pause");
            engine.dispatch("engine.shutdown", nlohmann::json::object());
        }

        std::vector<std::string> recheck_states;
        bt::Engine restored([&recheck_states](const std::string& event, const nlohmann::json& params) {
            if (event == "event.taskUpdated" && params.contains("task")) {
                recheck_states.push_back(params.at("task").at("state").get<std::string>());
            }
        });
        const auto initialized = restored.dispatch("engine.initialize", {
            {"protocolVersion", "1.4"}, {"statePath", path_utf8(seeding_state_path)}});
        expect(initialized.at("restoredTasks") == 1, "paused seeding task was not restored");
        expect(restored.dispatch("task.get", {{"id", id}}).at("task").at("state") == "paused",
            "restored seeding task lost its paused state");
        origin_tracker.set_peer_port(seeder.listen_port());
        restored.dispatch("task.resume", {{"id", id}});
        wait_for_state(restored, id, "seeding");
        const auto port_deadline = std::chrono::steady_clock::now() + 5s;
        while (origin_tracker.last_observed_peer_port() == first_engine_peer_port
            && std::chrono::steady_clock::now() < port_deadline) {
            std::this_thread::sleep_for(100ms);
        }
        const auto engine_peer_port = origin_tracker.last_observed_peer_port();
        expect(engine_peer_port != 0 && engine_peer_port != seeder.listen_port(),
            "Tracker did not observe the restored engine peer port");
        origin_tracker.set_peer_port(engine_peer_port);

        lt::session leecher(peer_settings);
        lt::add_torrent_params add;
        add.ti = seeding_info;
        add.save_path = path_utf8(leecher_download);
        add.flags &= ~lt::torrent_flags::paused;
        add.flags &= ~lt::torrent_flags::auto_managed;
        lt::error_code error;
        const auto leecher_handle = leecher.add_torrent(std::move(add), error);
        expect(!error && leecher_handle.is_valid(), "cannot add limited-seeding leecher");

        const auto completed = wait_for_state(restored, id, "completed");
        expect(completed.at("seedStopReason") == "ratio",
            "limited seeding did not stop on the ratio condition");
        expect(completed.at("shareRatio").get<double>() >= 0.1,
            "reported share ratio did not reach the configured limit");
        const auto uploaded_before_restart = completed.at("uploadedBytes").get<std::uint64_t>();

        const auto seeding_payload = download / "limited-seeding.bin";
        std::error_code remove_error;
        expect(std::filesystem::remove(seeding_payload, remove_error) && !remove_error,
            "could not remove the completed seeding payload for recheck");
        origin_tracker.set_peer_port(seeder.listen_port());
        recheck_states.clear();
        restored.dispatch("task.recheck", {{"id", id}});
        const auto rechecked = wait_for_state(restored, id, "completed");
        expect(std::filesystem::is_regular_file(seeding_payload),
            "rechecking a missing completed payload did not redownload it");
        expect(read_bytes(seeding_payload) == read_bytes(seeding_source),
            "rechecked payload differs from the original seed");
        bool saw_checking = false;
        for (const auto& state : recheck_states) {
            saw_checking = saw_checking || state == "checking";
        }
        expect(saw_checking, "rechecking a missing completed payload did not enter checking state");
        expect(rechecked.at("downloadedBytes") == rechecked.at("totalBytes"),
            "rechecked task did not report complete byte counts");
        restored.dispatch("engine.shutdown", nlohmann::json::object());

        bt::Engine verified([](const std::string&, const nlohmann::json&) {});
        verified.dispatch("engine.initialize", {
            {"protocolVersion", "1.4"}, {"statePath", path_utf8(seeding_state_path)}});
        const auto recovered = verified.dispatch("task.get", {{"id", id}}).at("task");
        expect(recovered.at("state") == "completed" && recovered.at("seedStopReason") == "ratio",
            "completed seeding state was not restored");
        expect(recovered.at("uploadedBytes").get<std::uint64_t>() >= uploaded_before_restart,
            "persistent uploaded byte counter moved backwards after restart");
        verified.dispatch("engine.shutdown", nlohmann::json::object());
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 6 && std::string_view(argv[1]) == "--recovery-child") {
            return run_recovery_child(argv[2], argv[3], argv[4], argv[5]);
        }
        run_http_download_integration_test();
        run_download_integration_test();
        run_tracker_and_seeding_integration_test();
        std::cout << "Local HTTP and tracker/seeder integration tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Integration test failure: " << exception.what() << '\n';
        return 1;
    }
}
