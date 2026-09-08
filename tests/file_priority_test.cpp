#include "bt_download/engine.hpp"
#include "bt_download/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
        path_ = std::filesystem::temp_directory_path() / ("bt_download_priority_test_" + suffix);
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
    expect(server.run() == 0, "file priority JSON-RPC request failed to run");
    return nlohmann::json::parse(output.str());
}

void expect_error(const nlohmann::json& response, const char* expected_code) {
    expect(response.contains("error"), "file priority request was expected to fail");
    expect(response.at("error").at("data").at("code") == expected_code,
        "file priority request returned the wrong business code");
}

} // namespace

void run_file_priority_tests() {
    TemporaryDirectory temporary;
    const auto state_path = temporary.path() / "state";
    const auto save_path = temporary.path() / "downloads";
    std::filesystem::create_directories(save_path);
    const auto torrent_path = create_multi_file_torrent(temporary.path());

    std::string task_id;
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.5"}, {"statePath", path_utf8(state_path)}});

        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", "missing"}, {"priorities", {{"0", 0}}}}), "TASK_NOT_FOUND");

        const auto added = engine.dispatch("task.add", {
            {"source", {{"kind", "torrentFile"}, {"path", path_utf8(torrent_path)}}},
            {"savePath", path_utf8(save_path)}, {"start", false}});
        task_id = added.at("task").at("id").get<std::string>();

        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", "0"}}), "INVALID_FILE_PRIORITY");
        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", nlohmann::json::object()}}),
            "INVALID_FILE_PRIORITY");
        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", {{"first", 0}}}}), "INVALID_FILE_PRIORITY");
        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", {{"2", 0}}}}), "INVALID_FILE_PRIORITY");
        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", {{"-1", 0}}}}), "INVALID_FILE_PRIORITY");
        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", {{"0", 8}}}}), "INVALID_FILE_PRIORITY");
        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", {{"0", -1}}}}), "INVALID_FILE_PRIORITY");
        expect_error(dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", {{"0", 1.5}}}}), "INVALID_FILE_PRIORITY");

        const auto set = dispatch_rpc(engine, "task.setFilePriorities",
            {{"id", task_id}, {"priorities", {{"1", 0}}}});
        expect(set.contains("result"), "valid file priority update was rejected");
        expect(set.at("result").at("priorities") == nlohmann::json::array({4, 0}),
            "file priority update returned the wrong full vector");

        const auto details = dispatch_rpc(engine, "task.files", {{"id", task_id}});
        const auto& files = details.at("result").at("files");
        expect(files.size() == 2
                && files[0].at("priority") == 4 && files[1].at("priority") == 0,
            "task.files did not expose updated file priorities");

        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }

    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.5"}, {"statePath", path_utf8(state_path)}});
        const auto details = dispatch_rpc(engine, "task.files", {{"id", task_id}});
        const auto& files = details.at("result").at("files");
        expect(files.size() == 2
                && files[1].at("priority") == 0,
            "file priorities were not restored from resume data");
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }
}
