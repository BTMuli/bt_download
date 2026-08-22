#include "bt_download/engine.hpp"
#include "bt_download/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/hasher.hpp>

namespace {

namespace lt = libtorrent;

void expect(bool condition, const char* message) {
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
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() / ("bt_download_tabbed_details_test_" + suffix);
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

std::filesystem::path create_multi_file_torrent(const std::filesystem::path& directory) {
    constexpr char keep[] = "keep-0123456789abcdef";
    constexpr char skip[] = "skip-0123456789abcdef";
    std::vector<char> payload(std::begin(keep), std::end(keep) - 1);
    payload.insert(payload.end(), std::begin(skip), std::end(skip) - 1);

    lt::file_storage files;
    files.add_file("multi/keep.bin", static_cast<std::int64_t>(std::size(keep) - 1));
    files.add_file("multi/skip.bin", static_cast<std::int64_t>(std::size(skip) - 1));
    lt::create_torrent torrent(files, 16 * 1024, lt::create_torrent::v1_only);
    torrent.set_hash(lt::piece_index_t{0},
        lt::hasher(payload.data(), static_cast<int>(payload.size())).final());
    const auto bytes = torrent.generate_buf();
    const auto path = directory / "multi.torrent";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("failed to create multi-file test torrent");
    return path;
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
    expect(server.run() == 0, "tabbed details JSON-RPC request failed to run");
    return nlohmann::json::parse(output.str());
}

void expect_error(const nlohmann::json& response, const char* expected_code) {
    expect(response.contains("error"), "tabbed details request was expected to fail");
    expect(response.at("error").at("data").at("code") == expected_code,
        "tabbed details request returned the wrong business code");
}

} // namespace

void run_tabbed_details_tests() {
    TemporaryDirectory temporary;
    const auto state_path = temporary.path() / "state";
    const auto save_path = temporary.path() / "downloads";
    std::filesystem::create_directories(save_path);
    const auto torrent_path = create_multi_file_torrent(temporary.path());

    std::string task_id;
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.4"}, {"statePath", path_utf8(state_path)}});

        const auto added = engine.dispatch("task.add", {
            {"source", {{"kind", "torrentFile"}, {"path", path_utf8(torrent_path)}}},
            {"savePath", path_utf8(save_path)}, {"start", false}});
        task_id = added.at("task").at("id").get<std::string>();

        const auto details = engine.dispatch("task.details", {{"id", task_id}});
        expect(details.at("totalFiles") == 2
                && details.at("contentFiles") == 2,
            "1.4 overview did not report the file count");
        expect(details.at("totalPeers") == 0, "1.4 overview did not report the peer count");
        expect(details.at("files") == nlohmann::json::array(),
            "1.4 task.details must not carry the file list");
        expect(details.at("peers") == nlohmann::json::array(),
            "1.4 task.details must not carry the peer list");

        const auto all_files = dispatch_rpc(engine, "task.files", {{"id", task_id}});
        const auto& files = all_files.at("result").at("files");
        expect(files.size() == 2 && files[0].at("path") == "multi\\keep.bin"
                && files[1].at("path") == "multi\\skip.bin"
                && files[0].at("isPadding") == false
                && files[1].at("isPadding") == false,
            "task.files returned the wrong file list");
        expect(all_files.at("result").at("filesTruncated") == false,
            "default file page must not be truncated");
        expect(all_files.at("result").at("totalFiles") == 2,
            "task.files reported the wrong total");
        expect(all_files.at("result").at("contentFiles") == 2,
            "task.files reported the wrong content total");
        expect(all_files.at("result").at("nextOffset").is_null(),
            "complete file page must not expose nextOffset");

        const auto first_page = dispatch_rpc(engine, "task.files",
            {{"id", task_id}, {"offset", 0}, {"limit", 1}});
        expect(first_page.at("result").at("files").size() == 1
                && first_page.at("result").at("files")[0].at("path") == "multi\\keep.bin",
            "task.files first page was wrong");
        expect(first_page.at("result").at("filesTruncated") == true
                && first_page.at("result").at("nextOffset") == 1,
            "task.files truncation semantics were wrong");

        const auto second_page = dispatch_rpc(engine, "task.files",
            {{"id", task_id}, {"offset", 1}, {"limit", 1}});
        expect(second_page.at("result").at("files").size() == 1
                && second_page.at("result").at("files")[0].at("path") == "multi\\skip.bin"
                && second_page.at("result").at("filesTruncated") == false,
            "task.files second page was wrong");

        const auto past_end = dispatch_rpc(engine, "task.files",
            {{"id", task_id}, {"offset", 2}, {"limit", 1}});
        expect(past_end.at("result").at("files") == nlohmann::json::array()
                && past_end.at("result").at("filesTruncated") == false,
            "task.files past-end page must be empty");

        const auto peers = dispatch_rpc(engine, "task.peers", {{"id", task_id}});
        expect(peers.at("result").at("peers") == nlohmann::json::array()
                && peers.at("result").at("totalPeers") == 0
                && peers.at("result").at("peersTruncated") == false,
            "task.peers returned an unexpected peer page");

        expect_error(dispatch_rpc(engine, "task.files",
            {{"id", task_id}, {"offset", -1}}), "INVALID_PAGINATION");
        expect_error(dispatch_rpc(engine, "task.files",
            {{"id", task_id}, {"limit", 0}}), "INVALID_PAGINATION");
        expect_error(dispatch_rpc(engine, "task.files",
            {{"id", task_id}, {"limit", 2001}}), "INVALID_PAGINATION");
        expect_error(dispatch_rpc(engine, "task.peers",
            {{"id", task_id}, {"limit", 501}}), "INVALID_PAGINATION");
        expect_error(dispatch_rpc(engine, "task.files",
            {{"id", "missing"}}), "TASK_NOT_FOUND");

        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }
}
