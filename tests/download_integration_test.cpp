#include "bt_download/engine.hpp"

#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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
    const std::string& announce_url) {
    lt::file_storage storage;
    lt::add_files(storage, path_utf8(payload));
    lt::create_torrent torrent(storage, 16 * 1024, lt::create_torrent::v1_only);
    torrent.add_tracker(announce_url);
    torrent.set_creator("bt_download integration test");
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

std::string add_torrent_task(bt::Engine& engine, const std::filesystem::path& torrent,
    const std::filesystem::path& save_path) {
    const auto result = engine.dispatch("task.add", {
        {"source", {{"kind", "torrentFile"}, {"path", path_utf8(torrent)}}},
        {"savePath", path_utf8(save_path)},
        {"start", true},
    });
    return result.at("task").at("id").get<std::string>();
}

void run_download_integration_test() {
    WinsockRuntime winsock;
    TemporaryDirectory temporary;
    LocalHttpTracker tracker;

    const auto seed_root = temporary.path() / "seed";
    const auto single_source = seed_root / "single.bin";
    const auto bundle_source = seed_root / "bundle";
    const auto magnet_source = seed_root / "magnet.bin";
    write_payload(single_source, 180 * 1024 + 73, 11);
    write_payload(bundle_source / "first.bin", 96 * 1024 + 19, 23);
    write_payload(bundle_source / "nested" / "second.bin", 144 * 1024 + 41, 31);
    write_payload(magnet_source, 192 * 1024 + 29, 47);

    const auto single_torrent_path = temporary.path() / "single.torrent";
    const auto bundle_torrent_path = temporary.path() / "bundle.torrent";
    const auto magnet_torrent_path = temporary.path() / "magnet.torrent";
    const auto single_info = create_torrent_file(single_source, single_torrent_path, tracker.announce_url());
    const auto bundle_info = create_torrent_file(bundle_source, bundle_torrent_path, tracker.announce_url());
    const auto magnet_info = create_torrent_file(magnet_source, magnet_torrent_path, tracker.announce_url());

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
    expect(single_seed.is_valid() && bundle_seed.is_valid() && magnet_seed.is_valid(),
        "local seeder handle is invalid");
    wait_for_seeds({single_seed, bundle_seed, magnet_seed});
    bt::Engine engine([](const std::string&, const nlohmann::json&) {});
    engine.dispatch("engine.initialize", {
        {"protocolVersion", "1.0"},
        {"statePath", path_utf8(temporary.path() / "state")},
        {"config", {{"activeDownloads", 3}, {"metadataTimeoutSeconds", 15}}},
    });

    const auto single_download = temporary.path() / "single-download";
    const auto bundle_download = temporary.path() / "bundle-download";
    const auto magnet_download = temporary.path() / "magnet-download";
    std::filesystem::create_directories(single_download);
    std::filesystem::create_directories(bundle_download);
    std::filesystem::create_directories(magnet_download);

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

    engine.dispatch("engine.shutdown", nlohmann::json::object());
}

} // namespace

int main() {
    try {
        run_download_integration_test();
        std::cout << "Local tracker/seeder integration test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Integration test failure: " << exception.what() << '\n';
        return 1;
    }
}
