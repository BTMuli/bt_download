#include "bt_download/engine.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

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
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() / ("bt_download_resume_test_" + suffix);
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

std::filesystem::path create_test_torrent(const std::filesystem::path& directory) {
    constexpr char payload[] = "resume-data-test";
    lt::file_storage files;
    files.add_file("payload.bin", static_cast<std::int64_t>(sizeof(payload) - 1));
    lt::create_torrent torrent(files, 16 * 1024, lt::create_torrent::v1_only);
    torrent.set_hash(lt::piece_index_t{0}, lt::hasher(payload, static_cast<int>(sizeof(payload) - 1)).final());
    const auto bytes = torrent.generate_buf();
    const auto path = directory / "payload.torrent";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("failed to create test torrent");
    return path;
}

} // namespace

void run_resume_data_tests() {
    TemporaryDirectory temporary;
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        bool rejected = false;
        try {
            engine.dispatch("engine.initialize", {
                {"protocolVersion", "1.-1"},
                {"statePath", path_utf8(temporary.path() / "invalid-protocol-state")}});
        } catch (const std::exception& exception) {
            rejected = std::string(exception.what()).find("unsupported protocol version")
                != std::string::npos;
        }
        expect(rejected, "negative protocol minor version was accepted");
    }

    const auto legacy_state_path = temporary.path() / "legacy-state";
    std::filesystem::create_directories(legacy_state_path);
    {
        std::ofstream legacy_catalog(legacy_state_path / "catalog.json", std::ios::binary | std::ios::trunc);
        legacy_catalog << nlohmann::json({
            {"schemaVersion", 1}, {"config", {{"activeDownloads", 3}}},
            {"tasks", nlohmann::json::array()},
        }).dump(2);
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        const auto initialized = engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.1"}, {"statePath", path_utf8(legacy_state_path)}});
        expect(initialized.at("config").at("seedingEnabled") == false,
            "schema 1 migration silently enabled seeding");
        expect(initialized.at("config").at("additionalTrackers").empty(),
            "schema 1 migration invented supplemental Trackers");
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }
    {
        std::ifstream migrated_catalog(legacy_state_path / "catalog.json", std::ios::binary);
        const auto migrated = nlohmann::json::parse(migrated_catalog);
        expect(migrated.at("schemaVersion") == 2, "schema 1 catalog was not migrated to schema 2");
    }

    const auto protocol_1_1_state_path = temporary.path() / "protocol-1-1-state";
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.1"}, {"statePath", path_utf8(protocol_1_1_state_path)},
            {"config", {{"seedingEnabled", true}, {"seedRatioLimit", 2.0},
                {"seedTimeLimitMinutes", 60}}}});
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        bool rejected = false;
        try {
            engine.dispatch("engine.initialize", {
                {"protocolVersion", "1.0"}, {"statePath", path_utf8(protocol_1_1_state_path)}});
        } catch (const std::exception& exception) {
            rejected = std::string(exception.what()).find("persisted state requires protocol 1.1")
                != std::string::npos;
        }
        expect(rejected, "protocol 1.0 silently accepted persisted protocol 1.1 settings");
    }

    const auto state_path = temporary.path() / "state";
    const auto save_path = temporary.path() / "downloads";
    std::filesystem::create_directories(save_path);
    const auto torrent_path = create_test_torrent(temporary.path());

    std::string task_id;
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.0"}, {"statePath", path_utf8(state_path)}});
        const auto added = engine.dispatch("task.add", {
            {"source", {{"kind", "torrentFile"}, {"path", path_utf8(torrent_path)}}},
            {"savePath", path_utf8(save_path)}, {"start", false}});
        task_id = added.at("task").at("id").get<std::string>();
        const auto details = engine.dispatch("task.details", {{"id", task_id}});
        expect(details.at("pieceCount") == 1 && details.at("pieceLength") == 16 * 1024,
            "task details did not expose torrent pieces");
        expect(details.at("files").size() == 1
                && details.at("files").front().at("path") == "payload.bin",
            "task details did not expose torrent files");
        expect(details.at("peers").is_array() && details.at("completedPieces").is_string(),
            "task details returned invalid dynamic sections");
        const auto resume_path = state_path / "resume" / (task_id + ".fastresume");
        engine.dispatch("task.pause", {{"id", task_id}});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!std::filesystem::is_regular_file(resume_path) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        expect(std::filesystem::is_regular_file(resume_path), "critical state change did not save resume data");
        std::filesystem::remove(resume_path);
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }

    const auto resume_path = state_path / "resume" / (task_id + ".fastresume");
    expect(std::filesystem::is_regular_file(resume_path), "final resume data was not saved");
    expect(std::filesystem::file_size(resume_path) > 0, "final resume data is empty");

    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        const auto initialized = engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.0"}, {"statePath", path_utf8(state_path)}});
        expect(initialized.at("restoredTasks") == 1, "resume task was not restored");
        const auto task = engine.dispatch("task.get", {{"id", task_id}}).at("task");
        expect(task.at("state") == "paused", "restored task did not preserve paused state");
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }

    {
        std::ofstream damaged(resume_path, std::ios::binary | std::ios::trunc);
        damaged << "invalid resume data";
    }
    {
        bt::Engine engine([](const std::string&, const nlohmann::json&) {});
        const auto initialized = engine.dispatch("engine.initialize", {
            {"protocolVersion", "1.0"}, {"statePath", path_utf8(state_path)}});
        expect(initialized.at("restoredTasks") == 1, "damaged resume data prevented source fallback");
        engine.dispatch("engine.shutdown", nlohmann::json::object());
    }
    bool quarantined = false;
    for (const auto& entry : std::filesystem::directory_iterator(state_path / "resume")) {
        if (entry.path().filename().string().starts_with(task_id + ".fastresume.corrupt.")) {
            quarantined = true;
            break;
        }
    }
    expect(quarantined, "damaged resume data was not quarantined");
}
