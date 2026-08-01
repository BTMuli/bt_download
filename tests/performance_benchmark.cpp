#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#else
#error The bt_download performance benchmark currently requires Windows
#endif

namespace {
namespace lt = libtorrent;
using namespace std::chrono_literals;

constexpr double bytes_per_mib = 1024.0 * 1024.0;

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
        path_ = std::filesystem::temp_directory_path() / ("bt_download_benchmark_" + suffix);
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
        expect(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
    }
    ~WinsockRuntime() { WSACleanup(); }
};

class LocalHttpTracker {
public:
    LocalHttpTracker() {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        expect(socket_ != INVALID_SOCKET, "cannot create benchmark tracker socket");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
            || ::listen(socket_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(socket_);
            throw std::runtime_error("cannot bind benchmark tracker");
        }

        int length = sizeof(address);
        expect(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != SOCKET_ERROR,
            "cannot query benchmark tracker port");
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

    std::string announce_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/announce";
    }

private:
    static std::uint16_t parse_announced_port(const std::string& request) {
        const auto marker = request.find("&port=");
        if (marker == std::string::npos) return 0;
        const auto delimiter = request.find_first_of("& ", marker + 6);
        if (delimiter == std::string::npos) return 0;
        unsigned int port = 0;
        const auto result = std::from_chars(request.data() + marker + 6, request.data() + delimiter, port);
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

    void serve(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            const SOCKET client = ::accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;

            std::string request;
            std::array<char, 2048> buffer{};
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
                const int received = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (received <= 0) break;
                request.append(buffer.data(), static_cast<std::size_t>(received));
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

    SOCKET socket_{INVALID_SOCKET};
    std::uint16_t port_{0};
    std::atomic<std::uint16_t> peer_port_{0};
    std::jthread worker_;
};

void write_payload(const std::filesystem::path& path, std::uint64_t size, std::uint8_t seed) {
    std::filesystem::create_directories(path.parent_path());
    std::vector<char> block(1024 * 1024);
    for (std::size_t index = 0; index < block.size(); ++index) {
        block[index] = static_cast<char>((index * 37U + seed * 17U) & 0xffU);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const auto count = static_cast<std::streamsize>((std::min)(remaining,
            static_cast<std::uint64_t>(block.size())));
        output.write(block.data(), count);
        remaining -= static_cast<std::uint64_t>(count);
    }
    expect(static_cast<bool>(output), "cannot create benchmark payload");
}

std::shared_ptr<lt::torrent_info> create_torrent_file(const std::filesystem::path& payload,
    const std::filesystem::path& torrent_path, const std::string& announce_url) {
    lt::file_storage storage;
    lt::add_files(storage, path_utf8(payload));
    lt::create_torrent torrent(storage, 256 * 1024, lt::create_torrent::v1_only);
    torrent.add_tracker(announce_url);
    torrent.set_creator("bt_download performance benchmark");
    torrent.set_priv(true);
    lt::error_code error;
    lt::set_piece_hashes(torrent, path_utf8(payload.parent_path()), error);
    expect(!error, "cannot hash benchmark torrent: " + error.message());

    const auto bytes = torrent.generate_buf();
    std::ofstream output(torrent_path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    expect(static_cast<bool>(output), "cannot write benchmark torrent");

    auto info = std::make_shared<lt::torrent_info>(path_utf8(torrent_path), error);
    expect(!error, "cannot reopen benchmark torrent: " + error.message());
    return info;
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
    expect(!error, "cannot add benchmark seed: " + error.message());
    return handle;
}

void wait_for_seeds(const std::vector<lt::torrent_handle>& handles) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (const auto& handle : handles) {
            const auto status = handle.status();
            if (status.errc) throw std::runtime_error("benchmark seeder error: " + status.errc.message());
            ready = ready && status.is_seeding;
        }
        if (ready) return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("benchmark seeder did not finish checking payloads");
}

std::filesystem::path current_executable_directory() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    expect(length > 0 && length < buffer.size(), "cannot resolve benchmark executable path");
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

std::uint64_t file_time_ticks(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

struct ProcessMetrics {
    std::uint64_t cpu_ticks{};
    std::uint64_t read_bytes{};
    std::uint64_t write_bytes{};
    std::uint64_t working_set_bytes{};
};

struct TimedEvent {
    std::chrono::steady_clock::time_point timestamp;
    std::string method;
    nlohmann::json params;
};

class EngineProcess {
public:
    explicit EngineProcess(const std::filesystem::path& executable) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE child_stdin_read = nullptr;
        HANDLE child_stdout_write = nullptr;
        HANDLE child_stderr_write = nullptr;
        expect(CreatePipe(&child_stdin_read, &stdin_write_, &security, 0) != FALSE,
            "cannot create engine stdin pipe");
        expect(CreatePipe(&stdout_read_, &child_stdout_write, &security, 0) != FALSE,
            "cannot create engine stdout pipe");
        expect(CreatePipe(&stderr_read_, &child_stderr_write, &security, 0) != FALSE,
            "cannot create engine stderr pipe");
        expect(SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0) != FALSE,
            "cannot protect engine stdin parent handle");
        expect(SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0) != FALSE,
            "cannot protect engine stdout parent handle");
        expect(SetHandleInformation(stderr_read_, HANDLE_FLAG_INHERIT, 0) != FALSE,
            "cannot protect engine stderr parent handle");

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_stdin_read;
        startup.hStdOutput = child_stdout_write;
        startup.hStdError = child_stderr_write;
        PROCESS_INFORMATION process{};
        std::wstring command_line = L"\"" + executable.wstring() + L"\"";
        const auto started = std::chrono::steady_clock::now();
        const bool created = CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process) != FALSE;
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdout_write);
        CloseHandle(child_stderr_write);
        if (!created) throw std::runtime_error("cannot start bt_download.exe: " + std::to_string(GetLastError()));

        process_ = process.hProcess;
        CloseHandle(process.hThread);
        stdout_thread_ = std::jthread([this](std::stop_token) { read_stdout(); });
        stderr_thread_ = std::jthread([this](std::stop_token) { read_stderr(); });
        std::unique_lock lock(mutex_);
        expect(condition_.wait_for(lock, 5s, [this] { return ready_ || reader_failed_; }),
            "timed out waiting for event.ready");
        expect(!reader_failed_, "engine stdout failed before event.ready: " + reader_error_);
        ready_latency_ = std::chrono::duration<double, std::milli>(ready_at_ - started).count();
    }

    ~EngineProcess() {
        close_handle(stdin_write_);
        if (process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
        }
        close_handle(process_);
        close_handle(stdout_read_);
        close_handle(stderr_read_);
    }

    EngineProcess(const EngineProcess&) = delete;
    EngineProcess& operator=(const EngineProcess&) = delete;

    nlohmann::json request(const std::string& method, const nlohmann::json& params = nlohmann::json::object()) {
        int id = 0;
        {
            std::scoped_lock lock(mutex_);
            id = next_id_++;
        }
        const auto line = nlohmann::json({
            {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params},
        }).dump() + "\n";
        const auto started = std::chrono::steady_clock::now();
        DWORD written = 0;
        expect(WriteFile(stdin_write_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE
                && written == line.size(),
            "cannot write JSON-RPC request to engine");

        std::unique_lock lock(mutex_);
        expect(condition_.wait_for(lock, 10s, [this, id] {
            return responses_.contains(id) || reader_failed_;
        }), "timed out waiting for JSON-RPC response");
        if (reader_failed_ && !responses_.contains(id)) {
            DWORD exit_code = STILL_ACTIVE;
            GetExitCodeProcess(process_, &exit_code);
            throw std::runtime_error("engine stdout closed while waiting for response; exit code "
                + std::to_string(exit_code) + "; reader: " + reader_error_ + "; stderr: " + diagnostics_);
        }
        auto response = std::move(responses_.at(id));
        responses_.erase(id);
        request_latencies_ms_.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        if (response.contains("error")) {
            throw std::runtime_error("engine request failed: " + response.at("error").dump());
        }
        return response.at("result");
    }

    void shutdown() {
        (void)request("engine.shutdown");
        close_handle(stdin_write_);
        expect(WaitForSingleObject(process_, 15000) == WAIT_OBJECT_0, "engine did not shut down cleanly");
    }

    ProcessMetrics metrics() const {
        FILETIME created{}, exited{}, kernel{}, user{};
        expect(GetProcessTimes(process_, &created, &exited, &kernel, &user) != FALSE,
            "cannot query engine CPU time");
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        expect(GetProcessMemoryInfo(process_, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory)) != FALSE, "cannot query engine working set");
        IO_COUNTERS io{};
        expect(GetProcessIoCounters(process_, &io) != FALSE, "cannot query engine I/O counters");
        return {file_time_ticks(kernel) + file_time_ticks(user), io.ReadTransferCount,
            io.WriteTransferCount, memory.WorkingSetSize};
    }

    double ready_latency_ms() const { return ready_latency_; }

    std::vector<double> request_latencies_ms() const {
        std::scoped_lock lock(mutex_);
        return request_latencies_ms_;
    }

    std::vector<TimedEvent> events() const {
        std::scoped_lock lock(mutex_);
        return events_;
    }

    std::string diagnostics() const {
        std::scoped_lock lock(mutex_);
        return diagnostics_;
    }

private:
    static void close_handle(HANDLE& handle) {
        if (handle != nullptr) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }

    void handle_stdout_line(const std::string& line) {
        try {
            auto message = nlohmann::json::parse(line);
            std::scoped_lock lock(mutex_);
            if (message.contains("id") && message.at("id").is_number_integer()) {
                const auto id = message.at("id").get<int>();
                responses_[id] = std::move(message);
            } else if (message.contains("method")) {
                const auto method = message.at("method").get<std::string>();
                const auto now = std::chrono::steady_clock::now();
                if (method == "event.ready") {
                    ready_ = true;
                    ready_at_ = now;
                }
                events_.push_back({now, method, message.value("params", nlohmann::json::object())});
            }
            condition_.notify_all();
        } catch (const std::exception& exception) {
            std::scoped_lock lock(mutex_);
            reader_failed_ = true;
            reader_error_ = "cannot parse protocol line '" + line + "': " + exception.what();
            condition_.notify_all();
        }
    }

    void read_stdout() {
        std::array<char, 4096> block{};
        std::string buffered;
        DWORD read = 0;
        while (ReadFile(stdout_read_, block.data(), static_cast<DWORD>(block.size()), &read, nullptr) != FALSE
            && read > 0) {
            buffered.append(block.data(), read);
            std::size_t newline = 0;
            while ((newline = buffered.find('\n')) != std::string::npos) {
                auto line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                handle_stdout_line(line);
            }
        }
        std::scoped_lock lock(mutex_);
        if (!reader_failed_) {
            reader_failed_ = true;
            reader_error_ = "stdout pipe reached EOF";
        }
        condition_.notify_all();
    }

    void read_stderr() {
        std::array<char, 4096> block{};
        DWORD read = 0;
        while (ReadFile(stderr_read_, block.data(), static_cast<DWORD>(block.size()), &read, nullptr) != FALSE
            && read > 0) {
            std::scoped_lock lock(mutex_);
            diagnostics_.append(block.data(), read);
            constexpr std::size_t max_diagnostics = 64 * 1024;
            if (diagnostics_.size() > max_diagnostics) {
                diagnostics_.erase(0, diagnostics_.size() - max_diagnostics);
            }
        }
    }

    HANDLE process_{nullptr};
    HANDLE stdin_write_{nullptr};
    HANDLE stdout_read_{nullptr};
    HANDLE stderr_read_{nullptr};
    std::jthread stdout_thread_;
    std::jthread stderr_thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::map<int, nlohmann::json> responses_;
    std::vector<TimedEvent> events_;
    std::vector<double> request_latencies_ms_;
    std::string diagnostics_;
    std::string reader_error_;
    std::chrono::steady_clock::time_point ready_at_{};
    double ready_latency_{};
    int next_id_{1};
    bool ready_{false};
    bool reader_failed_{false};
};

struct SampleWindow {
    ProcessMetrics begin;
    ProcessMetrics end;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point finished;
    std::uint64_t peak_working_set{};
};

SampleWindow sample_for(EngineProcess& engine, std::chrono::milliseconds duration) {
    SampleWindow window;
    window.started = std::chrono::steady_clock::now();
    window.begin = engine.metrics();
    window.peak_working_set = window.begin.working_set_bytes;
    const auto deadline = window.started + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
        window.peak_working_set = (std::max)(window.peak_working_set, engine.metrics().working_set_bytes);
    }
    window.finished = std::chrono::steady_clock::now();
    window.end = engine.metrics();
    window.peak_working_set = (std::max)(window.peak_working_set, window.end.working_set_bytes);
    return window;
}

double elapsed_seconds(const SampleWindow& window) {
    return std::chrono::duration<double>(window.finished - window.started).count();
}

double cpu_percent(const SampleWindow& window) {
    const auto seconds = elapsed_seconds(window);
    const auto cpu_seconds = static_cast<double>(window.end.cpu_ticks - window.begin.cpu_ticks) / 10'000'000.0;
    return seconds == 0.0 ? 0.0 : cpu_seconds * 100.0 / seconds;
}

double percentile_95(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1;
    return values[index];
}

struct EventStatistics {
    std::size_t ordinary_progress_events{};
    double minimum_gap_ms{};
    double maximum_rate_hz{};
};

EventStatistics analyze_events(const std::vector<TimedEvent>& events) {
    struct TaskEventState {
        std::string state;
        std::uint64_t downloaded{};
        std::optional<std::chrono::steady_clock::time_point> last_ordinary;
    };
    std::unordered_map<std::string, TaskEventState> tasks;
    EventStatistics statistics;
    double minimum_gap = (std::numeric_limits<double>::max)();
    for (const auto& event : events) {
        if (event.method != "event.taskAdded" && event.method != "event.taskUpdated") continue;
        if (!event.params.contains("task")) continue;
        const auto& task = event.params.at("task");
        const auto id = task.at("id").get<std::string>();
        const auto state = task.at("state").get<std::string>();
        const auto downloaded = task.at("downloadedBytes").get<std::uint64_t>();
        auto& previous = tasks[id];
        const bool ordinary = event.method == "event.taskUpdated" && state == "downloading"
            && previous.state == state && previous.downloaded != downloaded;
        if (ordinary) {
            ++statistics.ordinary_progress_events;
            if (previous.last_ordinary) {
                minimum_gap = (std::min)(minimum_gap,
                    std::chrono::duration<double, std::milli>(event.timestamp - *previous.last_ordinary).count());
            }
            previous.last_ordinary = event.timestamp;
        }
        previous.state = state;
        previous.downloaded = downloaded;
    }
    if (minimum_gap != (std::numeric_limits<double>::max)()) {
        statistics.minimum_gap_ms = minimum_gap;
        statistics.maximum_rate_hz = 1000.0 / minimum_gap;
    }
    return statistics;
}

struct BenchmarkOptions {
    bool quick{};
    std::filesystem::path engine_path;
    std::optional<std::filesystem::path> output_path;
};

BenchmarkOptions parse_options(int argc, char** argv) {
    BenchmarkOptions options;
    options.engine_path = current_executable_directory() / "bt_download.exe";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--quick") {
            options.quick = true;
        } else if (argument == "--engine" && index + 1 < argc) {
            options.engine_path = std::filesystem::absolute(argv[++index]);
        } else if (argument == "--output" && index + 1 < argc) {
            options.output_path = std::filesystem::absolute(argv[++index]);
        } else {
            throw std::runtime_error("usage: bt_download_performance_benchmark [--quick] [--engine PATH] [--output PATH]");
        }
    }
    expect(std::filesystem::is_regular_file(options.engine_path),
        "bt_download.exe was not found next to the benchmark; use --engine PATH");
    return options;
}

nlohmann::json run_benchmark(const BenchmarkOptions& options) {
    const auto idle_duration = options.quick ? 5s : 300s;
    const std::uint64_t payload_bytes = 64ULL * 1024 * 1024;
    const std::int64_t download_limit = 8 * 1024 * 1024;
    constexpr int task_count = 2;

    WinsockRuntime winsock;
    TemporaryDirectory temporary;
    EngineProcess engine(options.engine_path);
    const auto initialized = engine.request("engine.initialize", {
        {"protocolVersion", "1.0"},
        {"statePath", path_utf8(temporary.path() / "state")},
        {"config", {
            {"activeDownloads", task_count},
            {"downloadRateLimit", download_limit},
            {"connectionsLimit", 64},
            {"connectionsPerTask", 32},
        }},
    });
    (void)initialized;

    std::this_thread::sleep_for(options.quick ? 1s : 5s);
    const auto idle = sample_for(engine,
        std::chrono::duration_cast<std::chrono::milliseconds>(idle_duration));

    LocalHttpTracker tracker;
    const auto seed_root = temporary.path() / "seed";
    std::vector<std::filesystem::path> torrent_paths;
    std::vector<std::shared_ptr<lt::torrent_info>> torrent_infos;
    for (int index = 0; index < task_count; ++index) {
        const auto payload = seed_root / ("benchmark-" + std::to_string(index) + ".bin");
        const auto torrent = temporary.path() / ("benchmark-" + std::to_string(index) + ".torrent");
        write_payload(payload, payload_bytes, static_cast<std::uint8_t>(index + 1));
        torrent_infos.push_back(create_torrent_file(payload, torrent, tracker.announce_url()));
        torrent_paths.push_back(torrent);
    }

    lt::settings_pack seed_settings;
    seed_settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    seed_settings.set_bool(lt::settings_pack::enable_dht, false);
    seed_settings.set_bool(lt::settings_pack::enable_lsd, false);
    seed_settings.set_bool(lt::settings_pack::enable_upnp, false);
    seed_settings.set_bool(lt::settings_pack::enable_natpmp, false);
    lt::session seeder(seed_settings);
    expect(seeder.listen_port() != 0, "benchmark seeder did not open a listening port");
    tracker.set_peer_port(seeder.listen_port());
    std::vector<lt::torrent_handle> seeds;
    for (const auto& info : torrent_infos) seeds.push_back(add_seed(seeder, info, seed_root));
    wait_for_seeds(seeds);

    std::vector<std::string> task_ids;
    const auto transfer_started = std::chrono::steady_clock::now();
    const auto transfer_begin = engine.metrics();
    std::uint64_t peak_working_set = transfer_begin.working_set_bytes;
    for (int index = 0; index < task_count; ++index) {
        const auto download = temporary.path() / ("download-" + std::to_string(index));
        std::filesystem::create_directories(download);
        const auto added = engine.request("task.add", {
            {"source", {{"kind", "torrentFile"}, {"path", path_utf8(torrent_paths[index])}}},
            {"savePath", path_utf8(download)},
            {"start", true},
        });
        task_ids.push_back(added.at("task").at("id").get<std::string>());
    }

    const auto transfer_deadline = transfer_started + (options.quick ? 45s : 120s);
    std::vector<bool> completed(task_count, false);
    while (std::chrono::steady_clock::now() < transfer_deadline) {
        bool all_completed = true;
        for (int index = 0; index < task_count; ++index) {
            if (completed[index]) continue;
            const auto task = engine.request("task.get", {{"id", task_ids[index]}}).at("task");
            if (task.at("state") == "error") {
                throw std::runtime_error("benchmark download failed: " + task.dump());
            }
            completed[index] = task.at("state") == "completed";
            all_completed = all_completed && completed[index];
        }
        peak_working_set = (std::max)(peak_working_set, engine.metrics().working_set_bytes);
        if (all_completed) break;
        std::this_thread::sleep_for(100ms);
    }
    expect(std::all_of(completed.begin(), completed.end(), [](bool value) { return value; }),
        "benchmark downloads did not complete before the deadline");
    const auto transfer_finished = std::chrono::steady_clock::now();
    const auto transfer_end = engine.metrics();
    peak_working_set = (std::max)(peak_working_set, transfer_end.working_set_bytes);

    const auto transfer_seconds = std::chrono::duration<double>(transfer_finished - transfer_started).count();
    SampleWindow transfer_window{transfer_begin, transfer_end, transfer_started, transfer_finished, peak_working_set};
    const auto event_statistics = analyze_events(engine.events());
    const auto latencies = engine.request_latencies_ms();
    const auto p95_latency = percentile_95(latencies);
    const auto idle_cpu = cpu_percent(idle);
    const auto idle_working_set_mib = static_cast<double>(idle.peak_working_set) / bytes_per_mib;
    const auto active_working_set_mib = static_cast<double>(peak_working_set) / bytes_per_mib;
    const auto ready_passed = engine.ready_latency_ms() < 2000.0;
    const auto idle_cpu_passed = idle_cpu < 1.0;
    const auto idle_memory_passed = idle_working_set_mib < 80.0;
    const auto active_memory_passed = active_working_set_mib < 250.0;
    const auto latency_passed = p95_latency < 100.0;
    const auto events_passed = event_statistics.ordinary_progress_events >= 2
        && event_statistics.minimum_gap_ms >= 450.0;
    const auto passed = ready_passed && idle_cpu_passed && idle_memory_passed
        && active_memory_passed && latency_passed && events_passed;

    nlohmann::json result = {
        {"schemaVersion", 1},
        {"mode", options.quick ? "quick" : "release"},
        {"authoritative", !options.quick},
        {"enginePath", path_utf8(options.engine_path)},
        {"configuration", {
            {"idleDurationSeconds", elapsed_seconds(idle)},
            {"taskCount", task_count},
            {"payloadBytesPerTask", payload_bytes},
            {"downloadRateLimitBytesPerSecond", download_limit},
        }},
        {"startup", {
            {"readyMilliseconds", engine.ready_latency_ms()},
            {"targetMilliseconds", 2000.0},
            {"passed", ready_passed},
        }},
        {"idle", {
            {"cpuPercentOfOneCore", idle_cpu},
            {"targetCpuPercentOfOneCore", 1.0},
            {"peakWorkingSetMiB", idle_working_set_mib},
            {"targetWorkingSetMiB", 80.0},
            {"passed", idle_cpu_passed && idle_memory_passed},
        }},
        {"transfer", {
            {"elapsedSeconds", transfer_seconds},
            {"payloadMiBPerSecond", static_cast<double>(payload_bytes * task_count) / bytes_per_mib / transfer_seconds},
            {"processReadMiBPerSecond", static_cast<double>(transfer_end.read_bytes - transfer_begin.read_bytes)
                / bytes_per_mib / transfer_seconds},
            {"processWriteMiBPerSecond", static_cast<double>(transfer_end.write_bytes - transfer_begin.write_bytes)
                / bytes_per_mib / transfer_seconds},
            {"cpuPercentOfOneCore", cpu_percent(transfer_window)},
            {"peakWorkingSetMiB", active_working_set_mib},
            {"targetWorkingSetMiB", 250.0},
            {"passed", active_memory_passed},
        }},
        {"protocol", {
            {"requestCount", latencies.size()},
            {"requestP95Milliseconds", p95_latency},
            {"targetRequestP95Milliseconds", 100.0},
            {"ordinaryProgressEvents", event_statistics.ordinary_progress_events},
            {"minimumOrdinaryProgressGapMilliseconds", event_statistics.minimum_gap_ms},
            {"maximumOrdinaryProgressRateHz", event_statistics.maximum_rate_hz},
            {"targetMaximumProgressRateHz", 2.0},
            {"passed", latency_passed && events_passed},
        }},
        {"passed", passed},
    };
    engine.shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto result = run_benchmark(options);
        const auto serialized = result.dump(2) + "\n";
        std::cout << serialized;
        if (options.output_path) {
            if (!options.output_path->parent_path().empty()) {
                std::filesystem::create_directories(options.output_path->parent_path());
            }
            std::ofstream output(*options.output_path, std::ios::binary | std::ios::trunc);
            output << serialized;
            if (!output) throw std::runtime_error("cannot write benchmark result file");
        }
        return result.at("passed").get<bool>() ? 0 : 2;
    } catch (const std::exception& exception) {
        std::cerr << "bt_download performance benchmark failed: " << exception.what() << '\n';
        return 1;
    }
}
