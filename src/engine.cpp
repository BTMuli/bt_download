#include "bt_download/engine.hpp"

#include "bt_download/path_safety.hpp"
#include "bt_download/protocol.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bt {
namespace lt = libtorrent;
namespace {

std::string path_utf8(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.u8string();
#endif
}

std::filesystem::path path_from_utf8(std::string_view value) {
#if defined(__cpp_lib_char8_t)
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return std::filesystem::u8path(value);
#endif
}

std::string hash_string(const lt::info_hash_t& hashes) {
    std::ostringstream output;
    output << hashes;
    return output.str();
}

TaskState state_from_status(const lt::torrent_status& status) {
    switch (status.state) {
    case lt::torrent_status::checking_files:
    case lt::torrent_status::checking_resume_data:
        return TaskState::checking;
    case lt::torrent_status::downloading_metadata:
        return TaskState::metadata;
    case lt::torrent_status::downloading:
        return TaskState::downloading;
    case lt::torrent_status::finished:
    case lt::torrent_status::seeding:
        return TaskState::completed;
    default:
        break;
    }
    if ((status.flags & lt::torrent_flags::paused) != lt::torrent_flags_t{}) return TaskState::paused;
    return TaskState::queued;
}

bool replace_file(const std::filesystem::path& temporary, const std::filesystem::path& target) {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return !error;
#endif
}

void validate_config(const nlohmann::json& config) {
    const int active = config.value("activeDownloads", 2);
    const int connections = config.value("connectionsLimit", 200);
    const int per_task = config.value("connectionsPerTask", 80);
    const std::int64_t download_limit = config.value("downloadRateLimit", std::int64_t{0});
    const std::int64_t upload_limit = config.value("uploadRateLimit", std::int64_t{1024 * 1024});
    if (active < 1 || active > 64) fail(-32602, "INVALID_CONFIG", "activeDownloads must be between 1 and 64");
    if (connections < 1 || connections > 10000 || per_task < 1 || per_task > connections) {
        fail(-32602, "INVALID_CONFIG", "connection limits are invalid");
    }
    if (download_limit < 0 || upload_limit < 0) fail(-32602, "INVALID_CONFIG", "rate limits cannot be negative");
}

lt::settings_pack make_settings(const nlohmann::json& config) {
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::active_downloads, config.value("activeDownloads", 2));
    settings.set_int(lt::settings_pack::active_limit, config.value("activeDownloads", 2));
    settings.set_int(lt::settings_pack::connections_limit, config.value("connectionsLimit", 200));
    settings.set_int(lt::settings_pack::download_rate_limit, config.value("downloadRateLimit", 0));
    settings.set_int(lt::settings_pack::upload_rate_limit, config.value("uploadRateLimit", 1024 * 1024));
    settings.set_bool(lt::settings_pack::enable_dht, true);
    settings.set_bool(lt::settings_pack::enable_lsd, true);
    settings.set_bool(lt::settings_pack::enable_upnp, true);
    settings.set_bool(lt::settings_pack::enable_natpmp, true);
    settings.set_bool(lt::settings_pack::auto_sequential, false);
    return settings;
}

} // namespace

Engine::Engine(EventSink event_sink)
    : event_sink_(std::move(event_sink)),
      config_({{"activeDownloads", 2}, {"downloadRateLimit", 0}, {"uploadRateLimit", 1024 * 1024},
               {"connectionsLimit", 200}, {"connectionsPerTask", 80}}),
      started_at_(std::chrono::steady_clock::now()) {}

Engine::~Engine() {
    try {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        std::scoped_lock lock(mutex_);
        if (initialized_) persist_catalog_locked();
    } catch (...) {
    }
}

nlohmann::json Engine::dispatch(const std::string& method, const nlohmann::json& params) {
    if (method == "engine.initialize") return initialize(params);
    if (method == "engine.status") return status();
    if (method == "engine.configure") return configure(params);
    if (method == "engine.shutdown") return shutdown();
    if (method == "task.add") return add_task(params);
    if (method == "task.list") return list_tasks();
    if (method == "task.get") return get_task(params);
    if (method == "task.pause") return pause_task(params);
    if (method == "task.resume") return resume_task(params);
    if (method == "task.retry") return retry_task(params);
    if (method == "task.recheck") return recheck_task(params);
    if (method == "task.remove") return remove_task(params);
    fail(-32601, "METHOD_NOT_FOUND", "unknown method: " + method);
}

bool Engine::shutdown_requested() const noexcept {
    std::scoped_lock lock(mutex_);
    return shutdown_requested_;
}

nlohmann::json Engine::initialize(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    if (initialized_) fail(-32000, "ALREADY_INITIALIZED", "engine is already initialized");
    const auto protocol = params.value("protocolVersion", std::string{});
    if (protocol.empty() || protocol.substr(0, protocol.find('.')) != std::string(BT_DOWNLOAD_PROTOCOL_VERSION).substr(0, 1)) {
        fail(-32000, "PROTOCOL_MISMATCH", "unsupported protocol version");
    }
    const auto raw_state_path = params.at("statePath").get<std::string>();
    state_path_ = path_from_utf8(raw_state_path);
    if (!state_path_.is_absolute()) fail(-32602, "STATE_PATH_INVALID", "statePath must be absolute");
    std::error_code error;
    std::filesystem::create_directories(state_path_, error);
    if (error) fail(-32000, "STATE_PATH_UNAVAILABLE", "cannot create statePath: " + error.message());

    if (params.contains("config")) config_.update(params["config"]);
    validate_config(config_);
    session_ = std::make_unique<lt::session>(make_settings(config_));
    initialized_ = true;
    load_catalog_locked();
    worker_ = std::jthread([this](std::stop_token token) { worker_loop(token); });
    return {{"protocolVersion", BT_DOWNLOAD_PROTOCOL_VERSION}, {"engineVersion", BT_DOWNLOAD_VERSION},
            {"libtorrentVersion", LIBTORRENT_VERSION}, {"restoredTasks", tasks_.size()}, {"config", config_}};
}

nlohmann::json Engine::status() const {
    std::scoped_lock lock(mutex_);
    std::size_t active = 0;
    for (const auto& [id, task] : tasks_) {
        (void)id;
        if (task.state == TaskState::downloading || task.state == TaskState::metadata || task.state == TaskState::checking) ++active;
    }
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_).count();
    return {{"initialized", initialized_}, {"protocolVersion", BT_DOWNLOAD_PROTOCOL_VERSION},
            {"engineVersion", BT_DOWNLOAD_VERSION}, {"libtorrentVersion", LIBTORRENT_VERSION},
            {"uptimeSeconds", uptime}, {"taskCount", tasks_.size()}, {"activeTasks", active}, {"config", config_}};
}

nlohmann::json Engine::configure(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    auto next = config_;
    next.update(params);
    validate_config(next);
    config_ = std::move(next);
    session_->apply_settings(make_settings(config_));
    persist_catalog_locked();
    return {{"config", config_}};
}

nlohmann::json Engine::add_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto& source_json = params.at("source");
    const auto kind = source_json.at("kind").get<std::string>();
    std::string source;
    if (kind == "torrentFile") source = source_json.at("path").get<std::string>();
    else if (kind == "magnet") source = source_json.at("uri").get<std::string>();
    else fail(-32602, "SOURCE_UNSUPPORTED", "source.kind must be torrentFile or magnet");
    if (source.size() > max_frame_bytes / 2) fail(-32602, "SOURCE_TOO_LARGE", "source is too large");

    const auto save_validation = validate_save_path(path_from_utf8(params.at("savePath").get<std::string>()));
    if (!save_validation.valid) fail(-32010, save_validation.error_code, save_validation.message);

    lt::error_code error;
    lt::add_torrent_params add;
    std::string info_hash;
    bool private_torrent = false;
    if (kind == "torrentFile") {
        const auto torrent_path = path_from_utf8(source);
        if (!torrent_path.is_absolute() || !std::filesystem::is_regular_file(torrent_path)) {
            fail(-32010, "SOURCE_INVALID", "torrent file must be an existing absolute path");
        }
        auto info = std::make_shared<lt::torrent_info>(source, error);
        if (error) fail(-32010, "SOURCE_INVALID", "invalid torrent metadata: " + error.message());
        for (lt::file_index_t index{0}; index < info->files().end_file(); ++index) {
            if (!is_safe_relative_torrent_path(path_from_utf8(info->files().file_path(index)))) {
                fail(-32010, "UNSAFE_TORRENT_PATH", "torrent contains an unsafe file path");
            }
        }
        const auto space = std::filesystem::space(save_validation.normalized, error);
        if (!error && static_cast<std::uint64_t>(info->total_size()) > space.available) {
            fail(-32012, "DISK_FULL", "not enough free space for torrent payload", true,
                 {{"requiredBytes", info->total_size()}, {"availableBytes", space.available}});
        }
        error.clear();
        info_hash = hash_string(info->info_hashes());
        private_torrent = info->priv();
        add.ti = std::move(info);
    } else {
        add = lt::parse_magnet_uri(source, error);
        if (error || !add.info_hashes.has_v1() && !add.info_hashes.has_v2()) {
            fail(-32010, "SOURCE_INVALID", "invalid magnet URI");
        }
        info_hash = hash_string(add.info_hashes);
    }
    add.save_path = path_utf8(save_validation.normalized);
    add.max_connections = config_.value("connectionsPerTask", 80);
    const bool start = params.value("start", true);
    if (!start) add.flags |= lt::torrent_flags::paused;

    for (const auto& [id, task] : tasks_) {
        if (task.info_hash == info_hash && std::filesystem::equivalent(task.save_path, save_validation.normalized, error)) {
            fail(-32011, "DUPLICATE_TASK", "the torrent already exists at this save path", false, {{"taskId", id}});
        }
        error.clear();
    }

    auto handle = session_->add_torrent(std::move(add), error);
    if (error) fail(-32000, "ENGINE_ADD_FAILED", error.message(), true);
    TaskSnapshot task;
    task.id = new_task_id();
    task.state = kind == "magnet" ? TaskState::metadata : (start ? TaskState::queued : TaskState::paused);
    task.source_kind = kind;
    task.source = source;
    task.save_path = save_validation.normalized;
    task.display_name = params.value("displayName", std::string{});
    task.info_hash = info_hash;
    task.private_torrent = private_torrent;
    handles_.emplace(task.id, handle);
    tasks_.emplace(task.id, task);
    persist_catalog_locked();
    emit_task("event.taskAdded", task);
    return {{"task", task}};
}

nlohmann::json Engine::list_tasks() {
    std::scoped_lock lock(mutex_);
    require_initialized();
    update_snapshots_locked();
    nlohmann::json result = nlohmann::json::array();
    for (const auto& [id, task] : tasks_) { (void)id; result.push_back(task); }
    return {{"tasks", std::move(result)}, {"sequence", sequence_}};
}

nlohmann::json Engine::get_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    update_snapshots_locked();
    return {{"task", require_task_locked(params.at("id").get<std::string>())}, {"sequence", sequence_}};
}

nlohmann::json Engine::pause_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    auto& task = require_task_locked(params.at("id").get<std::string>());
    if (task.state != TaskState::completed) {
        const auto handle = handles_.find(task.id);
        if (handle == handles_.end()) fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
        handle->second.pause();
        task.state = TaskState::paused;
        persist_catalog_locked();
        emit_task("event.taskUpdated", task);
    }
    return {{"task", task}};
}

nlohmann::json Engine::resume_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    auto& task = require_task_locked(params.at("id").get<std::string>());
    if (task.state == TaskState::paused || task.state == TaskState::error) {
        const auto handle = handles_.find(task.id);
        if (handle == handles_.end()) fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
        handle->second.resume();
        task.state = task.info_hash.empty() ? TaskState::metadata : TaskState::queued;
        task.last_error.reset();
        persist_catalog_locked();
        emit_task("event.taskUpdated", task);
    }
    return {{"task", task}};
}

nlohmann::json Engine::retry_task(const nlohmann::json& params) { return resume_task(params); }

nlohmann::json Engine::recheck_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    auto& task = require_task_locked(params.at("id").get<std::string>());
    const auto handle = handles_.find(task.id);
    if (handle == handles_.end()) fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
    handle->second.force_recheck();
    task.state = TaskState::checking;
    task.last_error.reset();
    persist_catalog_locked();
    emit_task("event.taskUpdated", task);
    return {{"task", task}};
}

nlohmann::json Engine::remove_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto id = params.at("id").get<std::string>();
    auto& task = require_task_locked(id);
    const bool delete_data = params.value("deleteData", false);
    const auto handle = handles_.find(id);
    if (handle != handles_.end()) {
        session_->remove_torrent(handle->second, delete_data ? lt::session::delete_files : lt::remove_flags_t{});
    } else if (delete_data) {
        fail(-32005, "TASK_UNAVAILABLE", "cannot safely delete data without torrent metadata", false);
    }
    const auto snapshot = task;
    handles_.erase(id);
    tasks_.erase(id);
    persist_catalog_locked();
    event_sink_("event.taskRemoved", {{"sequence", ++sequence_}, {"task", snapshot}, {"dataDeleted", delete_data}});
    return {{"removed", true}, {"dataDeleted", delete_data}};
}

nlohmann::json Engine::shutdown() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    std::scoped_lock lock(mutex_);
    if (initialized_) {
        for (auto& [id, handle] : handles_) { (void)id; handle.pause(); }
        persist_catalog_locked();
        session_.reset();
    }
    shutdown_requested_ = true;
    initialized_ = false;
    return {{"shutdown", true}};
}

void Engine::require_initialized() const {
    if (!initialized_) fail(-32000, "NOT_INITIALIZED", "call engine.initialize first");
}

void Engine::persist_catalog_locked() {
    if (state_path_.empty()) return;
    nlohmann::json task_json = nlohmann::json::array();
    for (const auto& [id, task] : tasks_) {
        (void)id;
        nlohmann::json item = task;
        item["source"] = task.source;
        task_json.push_back(std::move(item));
    }
    const nlohmann::json catalog = {{"schemaVersion", 1}, {"config", config_}, {"tasks", std::move(task_json)}};
    const auto target = state_path_ / "catalog.json";
    const auto temporary = state_path_ / "catalog.json.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) fail(-32000, "PERSISTENCE_ERROR", "cannot write task catalog", true);
        output << catalog.dump(2);
        output.flush();
        if (!output) fail(-32000, "PERSISTENCE_ERROR", "cannot flush task catalog", true);
    }
    if (!replace_file(temporary, target)) {
        fail(-32000, "PERSISTENCE_ERROR", "cannot atomically replace task catalog", true);
    }
}

void Engine::load_catalog_locked() {
    const auto catalog_path = state_path_ / "catalog.json";
    if (!std::filesystem::exists(catalog_path)) return;
    try {
        std::ifstream input(catalog_path, std::ios::binary);
        const auto catalog = nlohmann::json::parse(input);
        if (catalog.value("schemaVersion", 0) != 1) return;
        if (catalog.contains("config")) {
            auto persisted_config = config_;
            persisted_config.update(catalog["config"]);
            validate_config(persisted_config);
            config_ = std::move(persisted_config);
            session_->apply_settings(make_settings(config_));
        }
        for (const auto& item : catalog.value("tasks", nlohmann::json::array())) {
            try {
                auto task = item.get<TaskSnapshot>();
                if (task.source.empty()) {
                    tasks_.emplace(task.id, std::move(task));
                    continue;
                }
                lt::error_code error;
                lt::add_torrent_params add;
                if (task.source_kind == "torrentFile") {
                    add.ti = std::make_shared<lt::torrent_info>(task.source, error);
                } else if (task.source_kind == "magnet") {
                    add = lt::parse_magnet_uri(task.source, error);
                }
                if (error) throw std::runtime_error(error.message());
                add.save_path = path_utf8(task.save_path);
                if (task.state == TaskState::paused || task.state == TaskState::completed) {
                    add.flags |= lt::torrent_flags::paused;
                }
                auto handle = session_->add_torrent(std::move(add), error);
                if (error) throw std::runtime_error(error.message());
                handles_.emplace(task.id, handle);
                tasks_.emplace(task.id, std::move(task));
            } catch (const std::exception& exception) {
                TaskSnapshot damaged;
                damaged.id = item.value("id", new_task_id());
                damaged.state = TaskState::error;
                damaged.last_error = TaskError{"RESTORE_FAILED", exception.what(), true};
                tasks_.insert_or_assign(damaged.id, std::move(damaged));
            }
        }
    } catch (const std::exception&) {
        const auto damaged = state_path_ / "catalog.json.corrupt";
        std::error_code error;
        std::filesystem::rename(catalog_path, damaged, error);
    }
}

TaskSnapshot& Engine::require_task_locked(const std::string& id) {
    const auto found = tasks_.find(id);
    if (found == tasks_.end()) fail(-32004, "TASK_NOT_FOUND", "task does not exist");
    return found->second;
}

void Engine::emit_task(const std::string& event, const TaskSnapshot& task) {
    event_sink_(event, {{"sequence", ++sequence_}, {"task", task}});
}

bool Engine::update_snapshots_locked() {
    bool catalog_changed = false;
    for (auto& [id, handle] : handles_) {
        auto& task = tasks_.at(id);
        const auto status = handle.status(lt::torrent_handle::query_name);
        const auto previous_state = task.state;
        const auto previous_downloaded = task.downloaded_bytes;
        const auto previous_download_rate = task.download_rate;
        const auto previous_upload_rate = task.upload_rate;
        const auto previous_peers = task.peers;
        const auto previous_seeds = task.seeds;
        auto next_state = state_from_status(status);
        if (status.errc) {
            next_state = TaskState::error;
            task.last_error = TaskError{"TORRENT_ERROR", status.errc.message(), true};
        }
        const bool state_changed = next_state != previous_state;
        task.state = next_state;
        task.total_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.total_wanted));
        task.downloaded_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.total_wanted_done));
        task.verified_bytes = task.downloaded_bytes;
        task.download_rate = static_cast<std::uint64_t>(std::max(0, status.download_payload_rate));
        task.upload_rate = static_cast<std::uint64_t>(std::max(0, status.upload_payload_rate));
        task.peers = status.num_peers;
        task.seeds = status.num_seeds;
        if (task.display_name.empty()) task.display_name = status.name;
        if (task.info_hash.empty()) task.info_hash = hash_string(status.info_hashes);
        if (const auto info = handle.torrent_file()) task.private_torrent = info->priv();
        if (task.state == TaskState::completed && previous_state != TaskState::completed) {
            handle.pause();
            task.download_rate = 0;
            task.upload_rate = 0;
        }
        const bool progress_changed = previous_downloaded != task.downloaded_bytes
            || previous_download_rate != task.download_rate || previous_upload_rate != task.upload_rate
            || previous_peers != task.peers || previous_seeds != task.seeds;
        if (state_changed || progress_changed) emit_task("event.taskUpdated", task);
        catalog_changed = catalog_changed || state_changed;
    }
    return catalog_changed;
}

void Engine::worker_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (stop_token.stop_requested()) break;
        try {
            std::scoped_lock lock(mutex_);
            if (initialized_ && session_ && update_snapshots_locked()) persist_catalog_locked();
        } catch (...) {
            // Request handlers surface persistent failures. The monitor must never terminate the process.
        }
    }
}

std::string Engine::new_task_id() const {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) output << '-';
        output << std::setw(2) << static_cast<int>(bytes[index]);
    }
    return output.str();
}

} // namespace bt
