#include "bt_download/engine.hpp"

#include "bt_download/path_safety.hpp"
#include "bt_download/protocol.hpp"
#include "error_mapping.hpp"
#include "seeding_policy.hpp"
#include "tracker_config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/announce_entry.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/write_resume_data.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bt {
namespace lt = libtorrent;
namespace {

constexpr auto resume_save_interval = std::chrono::seconds(30);
constexpr auto final_resume_timeout = std::chrono::seconds(10);
constexpr auto payload_probe_interval = std::chrono::seconds(1);
constexpr std::uintmax_t max_resume_file_bytes = 64U * 1024U * 1024U;
constexpr std::size_t max_detail_files = 2000;
constexpr std::size_t max_detail_peers = 500;
constexpr std::uint64_t http_progress_piece_min_bytes = 256U * 1024U;
constexpr std::uint64_t http_progress_piece_max_count = 256;
constexpr std::string_view default_user_agent = "bt_download/" BT_DOWNLOAD_VERSION;
constexpr std::size_t max_user_agent_bytes = 255;

struct HttpProgressPieces {
    std::uint64_t piece_length{0};
    std::uint64_t piece_count{0};
    std::string completed_pieces;
};

std::uint64_t divide_rounding_up(std::uint64_t value, std::uint64_t divisor) {
    return value / divisor + (value % divisor == 0 ? 0 : 1);
}

HttpProgressPieces http_progress_pieces(const TaskSnapshot& task,
    const HttpTransferProgress* transfer_progress = nullptr) {
    if (task.total_bytes == 0) return {};

    HttpProgressPieces result;
    result.piece_length = std::max(http_progress_piece_min_bytes,
        divide_rounding_up(task.total_bytes, http_progress_piece_max_count));
    result.piece_count = divide_rounding_up(task.total_bytes, result.piece_length);
    result.completed_pieces.reserve(static_cast<std::size_t>(result.piece_count));

    const auto downloaded = std::min(task.downloaded_bytes, task.total_bytes);
    for (std::uint64_t index = 0; index < result.piece_count; ++index) {
        const auto start = index * result.piece_length;
        const auto end = index + 1 == result.piece_count
            ? task.total_bytes
            : result.piece_length * (index + 1);
        bool complete = downloaded >= end;
        if (transfer_progress != nullptr && !transfer_progress->ranges.empty()) {
            auto covered_until = start;
            for (const auto& range : transfer_progress->ranges) {
                const auto range_end = range.start
                    + std::min(range.downloaded, range.end - range.start);
                if (range_end <= covered_until) continue;
                if (range.start > covered_until) break;
                covered_until = range_end;
                if (covered_until >= end) break;
            }
            complete = covered_until >= end;
        }
        result.completed_pieces.push_back(complete ? '1' : '0');
    }
    return result;
}

std::optional<std::uint64_t> page_value(const nlohmann::json& value) {
    if (!value.is_number_integer()) return std::nullopt;
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(signed_value);
}

std::pair<std::size_t, std::size_t> parse_task_page(
    const nlohmann::json& params, std::size_t max_limit) {
    std::size_t offset = 0;
    std::size_t limit = max_limit;
    if (params.contains("offset")) {
        const auto value = page_value(params.at("offset"));
        if (!value
            || *value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            fail(-32602, "INVALID_PAGINATION", "offset must be a non-negative integer");
        }
        offset = static_cast<std::size_t>(*value);
    }
    if (params.contains("limit")) {
        const auto value = page_value(params.at("limit"));
        if (!value || *value < 1 || *value > max_limit) {
            fail(-32602, "INVALID_PAGINATION",
                "limit must be an integer between 1 and " + std::to_string(max_limit));
        }
        limit = static_cast<std::size_t>(*value);
    }
    return {offset, limit};
}

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

std::string suffixed_file_name(const std::string& value, std::size_t suffix) {
    if (suffix == 0) return value;
    const auto path = path_from_utf8(value);
    const auto stem = path_utf8(path.stem());
    const auto extension = path_utf8(path.extension());
    return stem + " (" + std::to_string(suffix) + ")" + extension;
}

bool payload_is_missing(const TaskSnapshot& task, const lt::torrent_handle& handle) {
    const auto info = handle.torrent_file();
    if (!info) return false;

    for (lt::file_index_t index{0}; index < info->files().end_file(); ++index) {
        if (info->files().pad_file_at(index)) continue;
        const auto expected_size = info->files().file_size(index);
        if (expected_size < 0) return true;

        const auto path = task.save_path / path_from_utf8(info->files().file_path(index));
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) return true;
        const auto actual_size = std::filesystem::file_size(path, error);
        if (error || actual_size != static_cast<std::uintmax_t>(expected_size)) return true;
    }
    return false;
}

std::string hash_string(const lt::info_hash_t& hashes) {
    std::ostringstream output;
    output << hashes;
    return output.str();
}

std::string endpoint_string(const lt::tcp::endpoint& endpoint) {
    const auto address = endpoint.address().to_string();
    if (endpoint.address().is_v6()) return "[" + address + "]:" + std::to_string(endpoint.port());
    return address + ":" + std::to_string(endpoint.port());
}

int priority_value(lt::download_priority_t priority) {
    return static_cast<int>(static_cast<std::uint8_t>(priority));
}

TaskState state_from_status(const lt::torrent_status& status) {
    if ((status.flags & lt::torrent_flags::paused) != lt::torrent_flags_t{}) return TaskState::paused;
    if (status.state == lt::torrent_status::finished || status.state == lt::torrent_status::seeding) {
        return TaskState::seeding;
    }
    switch (status.state) {
    case lt::torrent_status::checking_files:
    case lt::torrent_status::checking_resume_data:
        return TaskState::checking;
    case lt::torrent_status::downloading_metadata:
        return TaskState::metadata;
    case lt::torrent_status::downloading:
        return TaskState::downloading;
    default:
        break;
    }
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

lt::settings_pack make_settings(const EngineConfig& config,
    const EngineProxyConfig& proxy, const std::string& user_agent) {
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::active_downloads, config.active_downloads);
    settings.set_int(lt::settings_pack::active_limit, -1);
    settings.set_int(lt::settings_pack::active_seeds, -1);
    settings.set_int(lt::settings_pack::connections_limit, config.connections_limit);
    settings.set_int(lt::settings_pack::download_rate_limit, static_cast<int>(config.download_rate_limit));
    settings.set_int(lt::settings_pack::upload_rate_limit, static_cast<int>(config.upload_rate_limit));
    settings.set_str(lt::settings_pack::user_agent, user_agent);
    const auto alert_mask = lt::alert_category::error | lt::alert_category::storage;
    settings.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<std::uint32_t>(alert_mask)));
    const bool proxy_enabled = proxy.enabled && proxy.peer_proxy.has_value();
    if (proxy_enabled) {
        const auto& endpoint = *proxy.peer_proxy;
        settings.set_str(lt::settings_pack::proxy_hostname, endpoint.host);
        settings.set_int(lt::settings_pack::proxy_port, endpoint.port);
        settings.set_str(lt::settings_pack::proxy_username, endpoint.username);
        settings.set_str(lt::settings_pack::proxy_password, endpoint.password);
        settings.set_int(lt::settings_pack::proxy_type,
            endpoint.username.empty() && endpoint.password.empty()
                ? lt::settings_pack::http : lt::settings_pack::http_pw);
        settings.set_bool(lt::settings_pack::proxy_hostnames, true);
        settings.set_bool(lt::settings_pack::proxy_peer_connections, true);
        settings.set_bool(lt::settings_pack::proxy_tracker_connections, true);
    } else {
        settings.set_str(lt::settings_pack::proxy_hostname, "");
        settings.set_int(lt::settings_pack::proxy_port, 0);
        settings.set_str(lt::settings_pack::proxy_username, "");
        settings.set_str(lt::settings_pack::proxy_password, "");
        settings.set_int(lt::settings_pack::proxy_type, lt::settings_pack::none);
    }
    settings.set_bool(lt::settings_pack::enable_dht, !proxy_enabled);
    settings.set_bool(lt::settings_pack::enable_lsd, !proxy_enabled);
    settings.set_bool(lt::settings_pack::enable_upnp, !proxy_enabled);
    settings.set_bool(lt::settings_pack::enable_natpmp, !proxy_enabled);
    settings.set_bool(lt::settings_pack::auto_sequential, false);
    return settings;
}

void apply_local_rate_limits(lt::session& session, const EngineConfig& config) {
    auto local = session.get_peer_class(lt::session::local_peer_class_id);
    local.download_limit = static_cast<int>(config.download_rate_limit);
    local.upload_limit = static_cast<int>(config.upload_rate_limit);
    session.set_peer_class(lt::session::local_peer_class_id, local);
}

} // namespace

Engine::Engine(EventSink event_sink)
    : event_sink_(std::move(event_sink)), started_at_(std::chrono::steady_clock::now()),
      next_resume_save_(started_at_ + resume_save_interval) {}

Engine::~Engine() {
    try {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        std::scoped_lock lock(mutex_);
        if (initialized_) finalize_locked();
    } catch (...) {
    }
}

nlohmann::json Engine::dispatch(const std::string& method, const nlohmann::json& params) {
    if (method == "engine.initialize") return initialize(params);
    if (method == "engine.status") return status();
    if (method == "engine.configure") return configure(params);
    if (method == "engine.configureProxy") return configure_proxy(params);
    if (method == "engine.shutdown") return shutdown();
    if (method == "task.add") return add_task(params);
    if (method == "task.list") return list_tasks();
    if (method == "task.get") return get_task(params);
    if (method == "task.details") return get_task_details(params);
    if (method == "task.files") return get_task_files(params);
    if (method == "task.peers") return get_task_peers(params);
    if (method == "task.setFilePriorities") return set_file_priorities(params);
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
    if (params.contains("userAgent")) {
        if (!params.at("userAgent").is_string()) {
            fail(-32602, "INVALID_USER_AGENT", "userAgent must be a string");
        }
        const auto user_agent = params.at("userAgent").get<std::string>();
        if (user_agent.empty() || user_agent.size() > max_user_agent_bytes) {
            fail(-32602, "INVALID_USER_AGENT",
                "userAgent must contain between 1 and 255 characters");
        }
        user_agent_ = user_agent;
    }
    else {
        user_agent_ = std::string(default_user_agent);
    }
    // 客户端与引擎始终同版本发布，协议版本必须严格一致。
    if (params.value("protocolVersion", std::string{}) != BT_DOWNLOAD_PROTOCOL_VERSION) {
        fail(-32000, "PROTOCOL_MISMATCH", "unsupported protocol version");
    }
    const auto raw_state_path = params.at("statePath").get<std::string>();
    state_path_ = path_from_utf8(raw_state_path);
    if (!state_path_.is_absolute()) fail(-32602, "STATE_PATH_INVALID", "statePath must be absolute");
    std::error_code error;
    std::filesystem::create_directories(state_path_, error);
    if (error) fail(-32000, "STATE_PATH_UNAVAILABLE", "cannot create statePath: " + error.message());

    const bool has_initial_config = params.contains("config");
    if (has_initial_config) {
        try {
            config_ = apply_config_patch(config_, params["config"]);
        } catch (const std::invalid_argument& exception) {
            fail(-32602, "INVALID_CONFIG", exception.what());
        }
    }
    if (params.contains("proxy")) {
        try {
            proxy_ = parse_proxy_config(params.at("proxy"));
        } catch (const std::invalid_argument& exception) {
            fail(-32602, "INVALID_PROXY", exception.what());
        }
    }
    session_ = std::make_unique<lt::session>(make_settings(config_, proxy_, user_agent_));
    apply_local_rate_limits(*session_, config_);
    initialized_ = true;
    load_catalog_locked();
    if (has_initial_config) {
        try {
            config_ = apply_config_patch(config_, params["config"]);
        } catch (const std::invalid_argument& exception) {
            handles_.clear();
            tasks_.clear();
            session_.reset();
            initialized_ = false;
            fail(-32602, "INVALID_CONFIG", exception.what());
        }
        session_->apply_settings(make_settings(config_, proxy_, user_agent_));
        apply_local_rate_limits(*session_, config_);
        apply_additional_trackers_to_all_locked(false);
        persist_catalog_locked();
    }
    next_resume_save_ = std::chrono::steady_clock::now() + resume_save_interval;
    worker_ = std::jthread([this](std::stop_token token) { worker_loop(token); });
    return {{"protocolVersion", BT_DOWNLOAD_PROTOCOL_VERSION}, {"engineVersion", BT_DOWNLOAD_VERSION},
            {"libtorrentVersion", LIBTORRENT_VERSION}, {"restoredTasks", tasks_.size()},
            {"features", nlohmann::json::array({"additionalTrackers", "limitedSeeding",
                "taskDetails", "tabbedDetails", "httpDownloads", "httpMultiConnection", "systemProxy"})},
            {"config", config_json(config_)}, {"proxy", proxy_status_json(proxy_)}};
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
            {"uptimeSeconds", uptime}, {"taskCount", tasks_.size()}, {"activeTasks", active},
            {"features", nlohmann::json::array({"additionalTrackers", "limitedSeeding",
                "taskDetails", "tabbedDetails", "httpDownloads", "httpMultiConnection", "systemProxy"})},
            {"config", config_json(config_)}, {"proxy", proxy_status_json(proxy_)}};
}

nlohmann::json Engine::configure(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    try {
        config_ = apply_config_patch(config_, params);
    } catch (const std::invalid_argument& exception) {
        fail(-32602, "INVALID_CONFIG", exception.what());
    }
    session_->apply_settings(make_settings(config_, proxy_, user_agent_));
    apply_local_rate_limits(*session_, config_);
    apply_shared_download_budget_locked();
    apply_additional_trackers_to_all_locked(true);
    update_snapshots_locked(true);
    persist_catalog_locked();
    return {{"config", config_json(config_)}};
}

nlohmann::json Engine::configure_proxy(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    EngineProxyConfig next;
    try {
        next = parse_proxy_config(params);
    } catch (const std::invalid_argument& exception) {
        fail(-32602, "INVALID_PROXY", exception.what());
    }

    for (auto& [id, task] : tasks_) {
        if (task.source_kind != "http" || !http_downloads_.contains(id)) continue;
        http_downloads_.cancel(id);
        task.state = TaskState::queued;
        task.download_rate = 0;
        task.last_error.reset();
        emit_task("event.taskUpdated", task);
    }
    proxy_ = std::move(next);
    session_->pause();
    session_->apply_settings(make_settings(config_, proxy_, user_agent_));
    session_->resume();
    apply_local_rate_limits(*session_, config_);
    schedule_http_tasks_locked();
    apply_shared_download_budget_locked();
    return {{"proxy", proxy_status_json(proxy_)}};
}

bool Engine::effective_seeding_enabled() const noexcept {
    return config_.seeding_enabled;
}

void Engine::apply_additional_trackers_locked(const lt::torrent_handle& handle, bool reannounce) {
    if (!handle.is_valid()) return;
    const auto info = handle.torrent_file();
    if (!info) return;

    auto current = handle.trackers();
    std::vector<lt::announce_entry> next;
    next.reserve(current.size() + config_.additional_trackers.size());
    int highest_tier = -1;
    std::unordered_set<std::string> urls;
    constexpr auto client_source = static_cast<std::uint8_t>(lt::announce_entry::source_client);
    for (auto entry : current) {
        if ((entry.source & client_source) != 0) {
            entry.source = static_cast<std::uint8_t>(entry.source & ~client_source);
            if (entry.source == 0) continue;
        }
        highest_tier = std::max(highest_tier, static_cast<int>(entry.tier));
        urls.insert(entry.url);
        next.push_back(std::move(entry));
    }

    if (!info->priv()) {
        const auto supplemental_tier = static_cast<std::uint8_t>(std::min(255, highest_tier + 1));
        for (const auto& url : config_.additional_trackers) {
            if (!urls.insert(url).second) continue;
            lt::announce_entry entry(url);
            entry.tier = supplemental_tier;
            entry.source = client_source;
            next.push_back(std::move(entry));
        }
    }

    handle.replace_trackers(next);
    if (reannounce && (handle.flags() & lt::torrent_flags::paused) == lt::torrent_flags_t{}) {
        handle.force_reannounce();
    }
}

void Engine::apply_additional_trackers_to_all_locked(bool reannounce) {
    for (const auto& [id, handle] : handles_) {
        apply_additional_trackers_locked(handle, reannounce);
        request_resume_save_locked(id, false);
    }
}

void Engine::apply_shared_download_budget_locked() {
    if (!session_) return;
    const auto active_http = static_cast<int>(std::min<std::size_t>(
        http_downloads_.active_count(), static_cast<std::size_t>(config_.active_downloads)));
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::active_downloads,
        std::max(0, config_.active_downloads - active_http));
    session_->apply_settings(settings);
}

std::filesystem::path Engine::http_target_path_locked(const std::string& id) const {
    const auto task = tasks_.find(id);
    const auto file_name = http_file_names_.find(id);
    if (task == tasks_.end() || file_name == http_file_names_.end()) {
        throw std::runtime_error("HTTP task target is unavailable");
    }
    return task->second.save_path / path_from_utf8(file_name->second);
}

std::filesystem::path Engine::http_partial_path_locked(const std::string& id) const {
    auto path = http_target_path_locked(id);
    path += path_from_utf8("." + id + ".part");
    return path;
}

std::string Engine::reserve_http_file_name_locked(const std::filesystem::path& save_path,
    const std::string& suggested) const {
    for (std::size_t suffix = 0; suffix < 10000; ++suffix) {
        const auto candidate = suffixed_file_name(suggested, suffix);
        bool reserved = false;
        for (const auto& [id, file_name] : http_file_names_) {
            const auto task = tasks_.find(id);
            if (task != tasks_.end() && task->second.save_path == save_path
                && file_name == candidate) {
                reserved = true;
                break;
            }
        }
        if (reserved) continue;
        std::error_code error;
        if (!std::filesystem::exists(save_path / path_from_utf8(candidate), error) && !error) {
            return candidate;
        }
    }
    fail(-32010, "TARGET_FILE_EXISTS", "cannot reserve a unique HTTP target filename");
}

bool Engine::schedule_http_tasks_locked() {
    std::size_t active_torrents = 0;
    for (const auto& [id, task] : tasks_) {
        (void)id;
        if (task.source_kind == "http") continue;
        if (task.state == TaskState::metadata || task.state == TaskState::checking
            || task.state == TaskState::downloading) {
            ++active_torrents;
        }
    }
    auto active = active_torrents + http_downloads_.active_count();
    const auto limit = static_cast<std::size_t>(config_.active_downloads);
    bool changed = false;
    for (auto& [id, task] : tasks_) {
        if (active >= limit) break;
        if (task.source_kind != "http" || task.state != TaskState::queued
            || http_downloads_.contains(id)) {
            continue;
        }
        std::optional<HttpTransferFailure> failure;
        std::error_code target_error;
        if (std::filesystem::exists(http_target_path_locked(id), target_error)) {
            failure = HttpTransferFailure{
                "TARGET_FILE_EXISTS", "HTTP target file already exists", false, false};
        } else if (target_error) {
            failure = HttpTransferFailure{"SAVE_PATH_UNAVAILABLE",
                "cannot inspect HTTP target path: " + target_error.message(), true, false};
        } else {
            failure = http_downloads_.start(
                id, task.source, http_partial_path_locked(id), user_agent_,
                static_cast<std::size_t>(config_.connections_per_task), proxy_);
        }
        if (failure) {
            task.state = TaskState::error;
            task.download_rate = 0;
            task.last_error = TaskError{failure->code, failure->message, failure->retryable};
        } else {
            task.state = TaskState::downloading;
            task.last_error.reset();
            ++active;
        }
        emit_task("event.taskUpdated", task);
        changed = true;
    }
    apply_shared_download_budget_locked();
    return changed;
}

bool Engine::update_http_tasks_locked(bool emit_events) {
    bool catalog_changed = false;
    for (auto update : http_downloads_.poll(config_.download_rate_limit)) {
        const auto found = tasks_.find(update.id);
        if (found == tasks_.end()) continue;
        auto& task = found->second;
        const auto previous_state = task.state;
        const auto previous_total = task.total_bytes;
        const auto previous_downloaded = task.downloaded_bytes;
        const auto previous_rate = task.download_rate;
        if (update.total_bytes > 0) task.total_bytes = update.total_bytes;
        task.downloaded_bytes = update.downloaded_bytes;
        task.download_rate = update.download_rate;

        if (update.finished && update.succeeded) {
            const auto partial = http_partial_path_locked(update.id);
            const auto target = http_target_path_locked(update.id);
            std::error_code error;
            if (std::filesystem::exists(target, error) || error) {
                task.state = TaskState::error;
                task.download_rate = 0;
                task.last_error = TaskError{
                    "TARGET_FILE_EXISTS", "HTTP target file already exists", false};
            } else {
                const auto size = std::filesystem::file_size(partial, error);
                if (error) {
                    task.state = TaskState::error;
                    task.download_rate = 0;
                    task.last_error = TaskError{
                        "STORAGE_ERROR", "cannot read completed HTTP file size: " + error.message(), true};
                } else {
                    std::filesystem::rename(partial, target, error);
                    if (error) {
                        task.state = TaskState::error;
                        task.download_rate = 0;
                        task.last_error = TaskError{
                            "STORAGE_ERROR", "cannot finalize HTTP file: " + error.message(), true};
                    } else {
                        task.total_bytes = static_cast<std::uint64_t>(size);
                        task.downloaded_bytes = task.total_bytes;
                        task.verified_bytes = task.total_bytes;
                        task.download_rate = 0;
                        task.state = TaskState::completed;
                        task.last_error.reset();
                        std::error_code state_error;
                        remove_http_transfer_state(partial, state_error);
                    }
                }
            }
        } else if (update.finished && update.failure) {
            if (update.failure->restart_without_range) {
                std::error_code error;
                std::filesystem::remove(http_partial_path_locked(update.id), error);
                if (!error) {
                    remove_http_transfer_state(
                        http_partial_path_locked(update.id), error);
                }
                if (error) {
                    task.state = TaskState::error;
                    task.last_error = TaskError{
                        "STORAGE_ERROR", "cannot reset HTTP partial file: " + error.message(), true};
                } else {
                    task.state = TaskState::queued;
                    task.total_bytes = 0;
                    task.downloaded_bytes = 0;
                    task.verified_bytes = 0;
                    task.last_error.reset();
                }
            } else {
                task.state = TaskState::error;
                task.last_error = TaskError{
                    update.failure->code, update.failure->message, update.failure->retryable};
            }
            task.download_rate = 0;
        }

        const bool state_changed = task.state != previous_state;
        const bool progress_changed = task.total_bytes != previous_total
            || task.downloaded_bytes != previous_downloaded
            || task.download_rate != previous_rate;
        if (state_changed || progress_changed) {
            if (emit_events) emit_task("event.taskUpdated", task);
            else pending_task_updates_.insert(update.id);
        } else if (emit_events && pending_task_updates_.contains(update.id)) {
            emit_task("event.taskUpdated", task);
        }
        catalog_changed = catalog_changed || state_changed;
    }
    apply_shared_download_budget_locked();
    return catalog_changed;
}

nlohmann::json Engine::add_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto& source_json = params.at("source");
    const auto kind = source_json.at("kind").get<std::string>();
    std::string source;
    if (kind == "torrentFile") source = source_json.at("path").get<std::string>();
    else if (kind == "magnet") source = source_json.at("uri").get<std::string>();
    else if (kind == "http") {
        const auto normalized = normalize_http_url(source_json.at("url").get<std::string>());
        if (!normalized) fail(-32010, "SOURCE_INVALID", "invalid HTTP(S) URL");
        source = *normalized;
    }
    else fail(-32602, "SOURCE_UNSUPPORTED", "source.kind must be torrentFile, magnet, or http");
    if (source.size() > max_frame_bytes / 2) fail(-32602, "SOURCE_TOO_LARGE", "source is too large");

    const auto save_validation = validate_save_path(path_from_utf8(params.at("savePath").get<std::string>()));
    if (!save_validation.valid) fail(-32010, save_validation.error_code, save_validation.message);
    const bool start = params.value("start", true);

    if (kind == "http") {
        std::error_code error;
        for (const auto& [id, existing] : tasks_) {
            if (existing.source_kind != "http" || existing.source != source) continue;
            const bool same_path = std::filesystem::equivalent(
                existing.save_path, save_validation.normalized, error);
            error.clear();
            if (same_path) {
                fail(-32011, "DUPLICATE_TASK", "the HTTP URL already exists at this save path",
                    false, {{"taskId", id}, {"savePath", path_utf8(existing.save_path)}});
            }
        }

        TaskSnapshot task;
        task.id = new_task_id();
        task.state = start ? TaskState::queued : TaskState::paused;
        task.source_kind = kind;
        task.source = source;
        task.save_path = save_validation.normalized;
        const auto file_name = reserve_http_file_name_locked(
            task.save_path, http_file_name_from_url(source));
        task.display_name = params.value("displayName", std::string{});
        if (task.display_name.empty()) task.display_name = file_name;
        const auto id = task.id;
        tasks_.emplace(id, task);
        http_file_names_.emplace(id, file_name);
        persist_catalog_locked();
        emit_task("event.taskAdded", tasks_.at(id));
        if (start && schedule_http_tasks_locked()) persist_catalog_locked();
        return {{"task", tasks_.at(id)}};
    }

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
        // Keep payload requests disabled until the untrusted metadata has passed validation.
        add.flags |= lt::torrent_flags::upload_mode;
        add.flags &= ~lt::torrent_flags::auto_managed;
    }
    add.save_path = path_utf8(save_validation.normalized);
    add.max_connections = config_.connections_per_task;
    add.flags |= lt::torrent_flags::paused;

    for (const auto& [id, task] : tasks_) {
        if (task.info_hash == info_hash) {
            const bool same_path = std::filesystem::equivalent(task.save_path, save_validation.normalized, error);
            fail(-32011, "DUPLICATE_TASK",
                same_path ? "the torrent already exists at this save path"
                          : "the torrent is already active at another save path",
                false, {{"taskId", id}, {"savePath", path_utf8(task.save_path)}});
        }
        error.clear();
    }

    auto handle = session_->add_torrent(std::move(add), error);
    if (error) fail(-32000, "ENGINE_ADD_FAILED", error.message(), true);
    apply_additional_trackers_locked(handle, false);
    if (start) handle.resume();
    TaskSnapshot task;
    task.id = new_task_id();
    task.state = kind == "magnet" && start ? TaskState::metadata : (start ? TaskState::queued : TaskState::paused);
    task.source_kind = kind;
    task.source = source;
    task.save_path = save_validation.normalized;
    task.display_name = params.value("displayName", std::string{});
    task.info_hash = info_hash;
    task.private_torrent = private_torrent;
    handles_.emplace(task.id, handle);
    tasks_.emplace(task.id, task);
    if (kind == "magnet" && start) metadata_started_.emplace(task.id, std::chrono::steady_clock::now());
    persist_catalog_locked();
    emit_task("event.taskAdded", task);
    return {{"task", task}};
}

nlohmann::json Engine::list_tasks() {
    std::scoped_lock lock(mutex_);
    require_initialized();
    if (update_snapshots_locked(false)) persist_catalog_locked();
    nlohmann::json result = nlohmann::json::array();
    for (const auto& [id, task] : tasks_) { (void)id; result.push_back(task); }
    return {{"tasks", std::move(result)}, {"sequence", sequence_}};
}

nlohmann::json Engine::get_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    if (update_snapshots_locked(false)) persist_catalog_locked();
    return {{"task", require_task_locked(params.at("id").get<std::string>())}, {"sequence", sequence_}};
}

nlohmann::json Engine::task_overview_locked(const std::string& id) {
    const auto& task = require_task_locked(id);
    if (task.source_kind == "http") {
        auto progress = http_downloads_.progress(id);
        if (!progress) {
            progress = read_http_transfer_progress(http_partial_path_locked(id));
        }
        const auto pieces = http_progress_pieces(
            task, progress ? &*progress : nullptr);
        return {
            {"task", task},
            {"pieceLength", pieces.piece_length},
            {"pieceCount", pieces.piece_count},
            {"completedPieces", pieces.completed_pieces},
            {"totalFiles", 1},
            {"contentFiles", 1},
            {"totalPeers", 0},
            {"httpConnections", progress ? progress->active_connections : 0},
        };
    }
    const auto found = handles_.find(id);
    if (found == handles_.end() || !found->second.is_valid()) {
        fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
    }

    const auto& handle = found->second;
    const auto info = handle.torrent_file();
    nlohmann::json overview = {
        {"task", task},
        {"pieceLength", 0},
        {"pieceCount", 0},
        {"completedPieces", ""},
        {"totalFiles", 0},
        {"contentFiles", 0},
        {"totalPeers", 0},
        {"httpConnections", 0},
    };
    if (!info) return overview;

    const auto status = handle.status(lt::torrent_handle::query_name | lt::torrent_handle::query_pieces);
    overview["pieceLength"] = info->piece_length();
    overview["pieceCount"] = info->num_pieces();
    std::string completed_pieces;
    completed_pieces.reserve(status.pieces.size());
    for (const auto index : status.pieces.range()) {
        completed_pieces.push_back(status.pieces[index] ? '1' : '0');
    }
    overview["completedPieces"] = std::move(completed_pieces);
    overview["totalFiles"] = static_cast<std::uint64_t>(info->num_files());
    std::size_t content_files = 0;
    for (lt::file_index_t index{0}; index < info->files().end_file(); ++index) {
        if (!info->files().pad_file_at(index)) ++content_files;
    }
    overview["contentFiles"] = static_cast<std::uint64_t>(content_files);
    // 概览只需要连接数；完整 Peer 列表由 task.peers 按需提供。
    overview["totalPeers"] = static_cast<std::uint64_t>(
        std::max(0, status.num_peers));
    return overview;
}

nlohmann::json Engine::task_files_locked(const std::string& id, std::size_t offset, std::size_t limit) {
    const auto& task = require_task_locked(id);
    if (task.source_kind == "http") {
        nlohmann::json files = nlohmann::json::array();
        if (offset == 0 && limit > 0) {
            files.push_back({
                {"path", http_file_names_.at(id)},
                {"size", task.total_bytes},
                {"completedBytes", task.downloaded_bytes},
                {"priority", 4},
                {"isPadding", false},
            });
        }
        return {
            {"files", std::move(files)},
            {"filesTruncated", false},
            {"totalFiles", 1},
            {"contentFiles", 1},
            {"offset", offset},
            {"nextOffset", nlohmann::json()},
        };
    }
    const auto found = handles_.find(id);
    if (found == handles_.end() || !found->second.is_valid()) {
        fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
    }
    const auto& handle = found->second;
    const auto info = handle.torrent_file();
    nlohmann::json result = {
        {"files", nlohmann::json::array()},
        {"filesTruncated", false},
        {"totalFiles", 0},
        {"contentFiles", 0},
        {"offset", offset},
        {"nextOffset", nlohmann::json()},
    };
    if (!info) return result;

    const auto total_files = static_cast<std::size_t>(info->num_files());
    result["totalFiles"] = total_files;
    std::size_t content_files = 0;
    for (lt::file_index_t index{0}; index < info->files().end_file(); ++index) {
        if (!info->files().pad_file_at(index)) ++content_files;
    }
    result["contentFiles"] = content_files;
    if (offset >= total_files || limit == 0) return result;

    const auto file_progress = handle.file_progress(lt::torrent_handle::piece_granularity);
    std::vector<lt::download_priority_t> priorities;
    const auto cached_priorities = file_priorities_.find(id);
    if (cached_priorities != file_priorities_.end()) {
        priorities = cached_priorities->second;
    } else {
        priorities = handle.get_file_priorities();
    }
    const auto end = std::min(total_files, offset + limit);
    for (std::size_t index = offset; index < end; ++index) {
        const auto file_index = lt::file_index_t{static_cast<int>(index)};
        const auto size = std::max<std::int64_t>(0, info->files().file_size(file_index));
        const auto completed = index < file_progress.size()
            ? std::clamp<std::int64_t>(file_progress[index], 0, size)
            : 0;
        const auto priority = index < priorities.size()
            ? priority_value(priorities[index])
            : priority_value(lt::default_priority);
        result["files"].push_back({
            {"path", info->files().file_path(file_index)},
            {"size", size},
            {"completedBytes", completed},
            {"priority", priority},
            {"isPadding", info->files().pad_file_at(file_index)},
        });
    }
    result["filesTruncated"] = end < total_files;
    if (end < total_files) result["nextOffset"] = end;
    return result;
}

nlohmann::json Engine::task_peers_locked(const std::string& id, std::size_t offset, std::size_t limit) {
    const auto& task = require_task_locked(id);
    if (task.source_kind == "http") {
        return {
            {"peers", nlohmann::json::array()},
            {"peersTruncated", false},
            {"totalPeers", 0},
            {"offset", offset},
            {"nextOffset", nlohmann::json()},
        };
    }
    const auto found = handles_.find(id);
    if (found == handles_.end() || !found->second.is_valid()) {
        fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
    }
    const auto& handle = found->second;
    const auto info = handle.torrent_file();
    nlohmann::json result = {
        {"peers", nlohmann::json::array()},
        {"peersTruncated", false},
        {"totalPeers", 0},
        {"offset", offset},
        {"nextOffset", nlohmann::json()},
    };
    if (!info) return result;

    std::vector<lt::peer_info> peers;
    handle.get_peer_info(peers);
    const auto total_peers = peers.size();
    result["totalPeers"] = total_peers;
    if (offset >= total_peers || limit == 0) return result;

    const auto end = std::min(total_peers, offset + limit);
    for (std::size_t index = offset; index < end; ++index) {
        const auto& peer = peers[index];
        result["peers"].push_back({
            {"endpoint", endpoint_string(peer.ip)},
            {"client", peer.client.empty() ? "unknown" : peer.client},
            {"progress", std::clamp(peer.progress, 0.0F, 1.0F)},
            {"downloadRate", std::max(0, peer.payload_down_speed)},
            {"uploadRate", std::max(0, peer.payload_up_speed)},
        });
    }
    result["peersTruncated"] = end < total_peers;
    if (end < total_peers) result["nextOffset"] = end;
    return result;
}

nlohmann::json Engine::get_task_details(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto id = params.at("id").get<std::string>();
    auto details = task_overview_locked(id);
    details["files"] = nlohmann::json::array();
    details["filesTruncated"] = false;
    details["peers"] = nlohmann::json::array();
    details["peersTruncated"] = false;
    return details;
}

nlohmann::json Engine::get_task_files(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto id = params.at("id").get<std::string>();
    const auto [offset, limit] = parse_task_page(params, max_detail_files);
    return task_files_locked(id, offset, limit);
}

nlohmann::json Engine::get_task_peers(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto id = params.at("id").get<std::string>();
    const auto [offset, limit] = parse_task_page(params, max_detail_peers);
    return task_peers_locked(id, offset, limit);
}

nlohmann::json Engine::set_file_priorities(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto id = params.at("id").get<std::string>();
    const auto found = tasks_.find(id);
    if (found == tasks_.end()) fail(-32004, "TASK_NOT_FOUND", "task not found");
    const auto& task = found->second;
    if (task.source_kind == "http") {
        fail(-32005, "TASK_UNAVAILABLE", "HTTP single-file tasks do not support file priorities");
    }

    const auto& patch = params.at("priorities");
    if (!patch.is_object() || patch.empty()) {
        fail(-32602, "INVALID_FILE_PRIORITY",
            "priorities must be a non-empty object mapping file indices to priorities");
    }
    if (patch.size() > max_detail_files) {
        fail(-32602, "INVALID_FILE_PRIORITY", "too many file priorities in one request");
    }

    const auto handle_found = handles_.find(id);
    if (handle_found == handles_.end() || !handle_found->second.is_valid()) {
        fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
    }
    const auto& handle = handle_found->second;
    const auto info = handle.torrent_file();
    if (!info) {
        fail(-32006, "METADATA_UNAVAILABLE",
            "file priorities require torrent metadata");
    }
    if (task.state == TaskState::completed || task.state == TaskState::seeding) {
        fail(-32005, "TASK_UNAVAILABLE",
            "file priorities cannot be changed on a completed or seeding task");
    }

    const auto file_count = static_cast<std::size_t>(info->num_files());
    auto& priorities = file_priorities_[id];
    if (priorities.size() != file_count) {
        priorities = handle.get_file_priorities();
        if (priorities.size() != file_count) {
            priorities.assign(file_count, lt::default_priority);
        }
    }
    for (const auto& [key, value] : patch.items()) {
        int index = 0;
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index < 0) {
            fail(-32602, "INVALID_FILE_PRIORITY", "file priority key must be a numeric file index");
        }
        const auto file_index = static_cast<std::size_t>(index);
        if (file_index >= file_count) {
            fail(-32602, "INVALID_FILE_PRIORITY", "file index out of range");
        }
        if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 7) {
            fail(-32602, "INVALID_FILE_PRIORITY",
                "priority must be an integer in the range 0..7");
        }
        priorities[file_index] = lt::download_priority_t{
            static_cast<std::uint8_t>(value.get<int>())};
    }

    handle.prioritize_files(priorities);
    // File priorities are applied asynchronously by the disk thread. Wait for
    // the change to take effect so the immediate resume save and the caller
    // both observe the applied state.
    const auto priority_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    for (;;) {
        const auto applied = handle.get_file_priorities();
        if (applied.size() == file_count && applied == priorities) break;
        if (std::chrono::steady_clock::now() >= priority_deadline) {
            fail(-32000, "INTERNAL_ERROR", "file priority update did not take effect", true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    request_resume_save_locked(id, false);
    nlohmann::json response_priorities = nlohmann::json::array();
    for (const auto priority : priorities) {
        response_priorities.push_back(priority_value(priority));
    }
    return {{"priorities", std::move(response_priorities)}};
}

nlohmann::json Engine::pause_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    auto& task = require_task_locked(params.at("id").get<std::string>());
    if (task.state != TaskState::completed) {
        if (task.source_kind == "http") {
            if (const auto progress = http_downloads_.progress(task.id)) {
                if (progress->total_bytes > 0) task.total_bytes = progress->total_bytes;
                task.downloaded_bytes = progress->downloaded_bytes;
            }
            http_downloads_.cancel(task.id);
            task.download_rate = 0;
            task.state = TaskState::paused;
            persist_catalog_locked();
            emit_task("event.taskUpdated", task);
            apply_shared_download_budget_locked();
            if (schedule_http_tasks_locked()) persist_catalog_locked();
            return {{"task", task}};
        }
        const auto handle = handles_.find(task.id);
        if (handle == handles_.end()) fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
        handle->second.pause();
        task.state = TaskState::paused;
        metadata_started_.erase(task.id);
        request_resume_save_locked(task.id, false);
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
        if (task.source_kind == "http") {
            task.last_error.reset();
            task.state = TaskState::queued;
            persist_catalog_locked();
            emit_task("event.taskUpdated", task);
            if (schedule_http_tasks_locked()) persist_catalog_locked();
            return {{"task", task}};
        }
        const auto handle = handles_.find(task.id);
        if (handle == handles_.end()) fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
        task.last_error.reset();
        bool metadata_available = task.source_kind != "magnet";
        if (task.source_kind == "magnet") {
            if (const auto info = handle->second.torrent_file()) {
                if (!validate_magnet_metadata_locked(task.id, handle->second, info)) return {{"task", task}};
                task.state = TaskState::queued;
                metadata_available = true;
            } else {
                handle->second.set_flags(lt::torrent_flags::upload_mode);
                handle->second.unset_flags(lt::torrent_flags::auto_managed);
                task.state = TaskState::metadata;
                metadata_started_.insert_or_assign(task.id, std::chrono::steady_clock::now());
            }
        } else {
            task.state = TaskState::queued;
        }
        if (metadata_available) {
            const auto status = handle->second.status();
            const bool payload_complete = status.state == lt::torrent_status::finished
                || status.state == lt::torrent_status::seeding;
            if (payload_complete) {
                task.uploaded_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.all_time_upload));
                task.total_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.total_wanted));
                task.seeding_seconds = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.finished_duration.count()));
                task.share_ratio = task.total_bytes == 0 ? 0.0
                    : static_cast<double>(task.uploaded_bytes) / static_cast<double>(task.total_bytes);
                const auto reason = task.seed_stop_reason ? task.seed_stop_reason
                    : evaluate_seed_stop(effective_seeding_enabled(), config_.seed_ratio_limit,
                        config_.seed_time_limit_minutes, task.uploaded_bytes, task.total_bytes,
                        task.seeding_seconds);
                if (reason) {
                    task.seed_stop_reason = reason;
                    task.state = TaskState::completed;
                    handle->second.pause();
                } else {
                    task.state = TaskState::seeding;
                    handle->second.resume();
                }
            } else {
                handle->second.resume();
            }
        } else {
            handle->second.resume();
        }
        request_resume_save_locked(task.id, false);
        persist_catalog_locked();
        emit_task("event.taskUpdated", task);
    }
    return {{"task", task}};
}

nlohmann::json Engine::retry_task(const nlohmann::json& params) { return resume_task(params); }

void Engine::start_recheck_locked(TaskSnapshot& task, lt::torrent_handle& handle) {
    const bool was_completed = task.state == TaskState::completed;
    if (was_completed) {
        handle.pause();
        // libtorrent may keep a completed, paused torrent in the auto-managed
        // queue. Take it out of that queue while the explicit recheck starts,
        // otherwise the asynchronous check can remain paused indefinitely.
        handle.unset_flags(lt::torrent_flags::auto_managed);
    }
    // Drop peer entries retained from the previous complete/seeding session.
    // Rechecking is a new download attempt; keeping the old reconnect backoff
    // can prevent the freshly announced seed from being tried again.
    handle.clear_peers();
    handle.force_recheck();
    // A completed handle can retain upload-only state from its previous
    // lifecycle. Rechecking is an explicit request to make missing data
    // downloadable again, so clear that mode first.
    handle.unset_flags(lt::torrent_flags::upload_mode);
    // A completed task is paused after its seeding limit is reached. Resume it
    // immediately so libtorrent can run the asynchronous check. Auto-managed
    // mode is restored after the check has completed.
    if (was_completed) {
        handle.resume();
        resume_after_recheck_.insert(task.id);
    } else {
        handle.set_flags(lt::torrent_flags::auto_managed);
    }
    task.state = TaskState::checking;
    // A stop reason belongs to the previous complete payload. Keeping it while
    // rechecking missing data would make update_snapshots_locked turn the task
    // back into completed before it can download the missing pieces.
    task.seed_stop_reason.reset();
    task.last_error.reset();
    request_resume_save_locked(task.id, false);
    persist_catalog_locked();
    emit_task("event.taskUpdated", task);
}

nlohmann::json Engine::recheck_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    auto& task = require_task_locked(params.at("id").get<std::string>());
    if (task.source_kind == "http") {
        fail(-32005, "TASK_UNAVAILABLE",
            "HTTP tasks do not have cryptographic metadata for rechecking");
    }
    const auto handle = handles_.find(task.id);
    if (handle == handles_.end()) fail(-32005, "TASK_UNAVAILABLE", "task has no active torrent handle", true);
    start_recheck_locked(task, handle->second);
    return {{"task", task}};
}

nlohmann::json Engine::remove_task(const nlohmann::json& params) {
    std::scoped_lock lock(mutex_);
    require_initialized();
    const auto id = params.at("id").get<std::string>();
    auto& task = require_task_locked(id);
    const bool delete_data = params.value("deleteData", false);
    if (task.source_kind == "http") {
        http_downloads_.cancel(id);
        if (delete_data) {
            const auto partial = http_partial_path_locked(id);
            for (const auto& path : {partial, http_target_path_locked(id)}) {
                std::error_code error;
                std::filesystem::remove(path, error);
                if (error) {
                    task.state = TaskState::error;
                    task.download_rate = 0;
                    task.last_error = TaskError{
                        "STORAGE_ERROR", "cannot delete HTTP task data: " + error.message(), true};
                    persist_catalog_locked();
                    emit_task("event.taskUpdated", task);
                    apply_shared_download_budget_locked();
                    fail(-32000, "STORAGE_ERROR",
                        "cannot delete HTTP task data: " + error.message(), true);
                }
            }
            std::error_code state_error;
            remove_http_transfer_state(partial, state_error);
            if (state_error) {
                fail(-32000, "STORAGE_ERROR",
                    "cannot delete HTTP range state: " + state_error.message(), true);
            }
        }
    } else {
        const auto handle = handles_.find(id);
        if (handle != handles_.end()) {
            session_->remove_torrent(handle->second,
                delete_data ? lt::session::delete_files : lt::remove_flags_t{});
        } else if (delete_data) {
            fail(-32005, "TASK_UNAVAILABLE",
                "cannot safely delete data without torrent metadata", false);
        }
    }
    const auto snapshot = task;
    handles_.erase(id);
    pending_resume_saves_.erase(id);
    deferred_resume_saves_.erase(id);
    pending_task_updates_.erase(id);
    resume_after_recheck_.erase(id);
    metadata_started_.erase(id);
    next_payload_probe_.erase(id);
    file_priorities_.erase(id);
    http_file_names_.erase(id);
    tasks_.erase(id);
    std::error_code resume_error;
    std::filesystem::remove(state_path_ / "resume" / (id + ".fastresume"), resume_error);
    persist_catalog_locked();
    event_sink_("event.taskRemoved", {{"sequence", ++sequence_}, {"task", snapshot}, {"dataDeleted", delete_data}});
    apply_shared_download_budget_locked();
    if (schedule_http_tasks_locked()) persist_catalog_locked();
    return {{"removed", true}, {"dataDeleted", delete_data}};
}

nlohmann::json Engine::shutdown() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    std::scoped_lock lock(mutex_);
    if (initialized_) {
        finalize_locked();
    }
    shutdown_requested_ = true;
    initialized_ = false;
    return {{"shutdown", true}};
}

void Engine::require_initialized() const {
    if (!initialized_) fail(-32000, "NOT_INITIALIZED", "call engine.initialize first");
}

void Engine::finalize_locked() {
    for (auto& [id, task] : tasks_) {
        if (task.source_kind != "http") continue;
        if (const auto progress = http_downloads_.progress(id)) {
            if (progress->total_bytes > 0) task.total_bytes = progress->total_bytes;
            task.downloaded_bytes = progress->downloaded_bytes;
        }
        http_downloads_.cancel(id);
        task.download_rate = 0;
        if (task.state != TaskState::completed) task.state = TaskState::paused;
    }
    for (auto& [id, handle] : handles_) {
        handle.pause();
        auto& task = tasks_.at(id);
        if (task.state != TaskState::completed) task.state = TaskState::paused;
    }
    save_final_resume_data_locked();
    persist_catalog_locked();
    pending_resume_saves_.clear();
    deferred_resume_saves_.clear();
    metadata_started_.clear();
    resume_after_recheck_.clear();
    next_payload_probe_.clear();
    file_priorities_.clear();
    handles_.clear();
    session_.reset();
}

void Engine::persist_catalog_locked() {
    if (state_path_.empty()) return;
    nlohmann::json task_json = nlohmann::json::array();
    for (const auto& [id, task] : tasks_) {
        (void)id;
        nlohmann::json item = task;
        item["source"] = task.source;
        if (task.source_kind == "http") item["fileName"] = http_file_names_.at(id);
        task_json.push_back(std::move(item));
    }
    const nlohmann::json catalog = {
        {"schemaVersion", 3}, {"config", config_json(config_)}, {"tasks", std::move(task_json)}};
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

bool Engine::load_catalog_locked() {
    const auto catalog_path = state_path_ / "catalog.json";
    if (!std::filesystem::exists(catalog_path)) return false;
    try {
        std::ifstream input(catalog_path, std::ios::binary);
        const auto catalog = nlohmann::json::parse(input);
        const int schema_version = catalog.value("schemaVersion", 0);
        if (schema_version != 1 && schema_version != 2 && schema_version != 3) return false;
        if (catalog.contains("config")) {
            config_ = apply_config_patch(config_, catalog["config"]);
            if (schema_version == 1) {
                config_.additional_trackers.clear();
                config_.seeding_enabled = false;
                config_.seed_ratio_limit = 2.0;
                config_.seed_time_limit_minutes = 60;
            }
            session_->apply_settings(make_settings(config_, proxy_, user_agent_));
            apply_local_rate_limits(*session_, config_);
        }
        for (const auto& item : catalog.value("tasks", nlohmann::json::array())) {
            try {
                auto task = item.get<TaskSnapshot>();
                if (schema_version == 1 && task.state == TaskState::completed) {
                    task.seed_stop_reason = SeedStopReason::disabled;
                }
                if (task.source_kind == "http") {
                    if (schema_version < 3 || task.source.empty()) {
                        throw std::runtime_error("HTTP task catalog fields are missing");
                    }
                    const auto normalized = normalize_http_url(task.source);
                    if (!normalized) throw std::runtime_error("HTTP task URL is invalid");
                    task.source = *normalized;
                    const auto file_name = item.at("fileName").get<std::string>();
                    const auto relative = path_from_utf8(file_name);
                    if (relative.empty() || relative.is_absolute() || relative.has_root_name()
                        || relative.has_root_directory() || !relative.parent_path().empty()
                        || relative == "." || relative == "..") {
                        throw std::runtime_error("HTTP task filename is unsafe");
                    }

                    const auto target = task.save_path / relative;
                    auto partial = target;
                    partial += path_from_utf8("." + task.id + ".part");
                    std::error_code error;
                    const bool target_is_regular = std::filesystem::is_regular_file(target, error);
                    if (error) throw std::runtime_error(error.message());
                    if (target_is_regular && task.state == TaskState::completed) {
                        const auto size = std::filesystem::file_size(target, error);
                        if (error) throw std::runtime_error(error.message());
                        task.total_bytes = static_cast<std::uint64_t>(size);
                        task.downloaded_bytes = task.total_bytes;
                        task.verified_bytes = task.total_bytes;
                        task.download_rate = 0;
                        task.state = TaskState::completed;
                        task.last_error.reset();
                    } else if (std::filesystem::exists(target, error)) {
                        if (error) throw std::runtime_error(error.message());
                        task.download_rate = 0;
                        task.state = TaskState::error;
                        task.last_error = TaskError{
                            "TARGET_FILE_EXISTS", "HTTP target file already exists", false};
                    } else {
                        if (error) throw std::runtime_error(error.message());
                        error.clear();
                        if (const auto progress = read_http_transfer_progress(partial)) {
                            task.total_bytes = progress->total_bytes;
                            task.downloaded_bytes = progress->downloaded_bytes;
                        } else if (std::filesystem::is_regular_file(partial, error)) {
                            const auto size = std::filesystem::file_size(partial, error);
                            if (!error) {
                                task.downloaded_bytes = static_cast<std::uint64_t>(size);
                            }
                        }
                        task.verified_bytes = 0;
                        task.download_rate = 0;
                        if (task.state == TaskState::completed) {
                            task.state = TaskState::error;
                            task.last_error = TaskError{
                                "DATA_MISSING", "completed HTTP target file is missing", true};
                        } else if (task.state != TaskState::paused && task.state != TaskState::error) {
                            task.state = TaskState::queued;
                        }
                    }
                    http_file_names_.emplace(task.id, file_name);
                    tasks_.emplace(task.id, std::move(task));
                    continue;
                }
                if (task.source.empty()) {
                    tasks_.emplace(task.id, std::move(task));
                    continue;
                }
                lt::error_code error;
                lt::add_torrent_params add;
                if (!load_resume_data_locked(task, add)) {
                    if (task.source_kind == "torrentFile") {
                        add.ti = std::make_shared<lt::torrent_info>(task.source, error);
                    } else if (task.source_kind == "magnet") {
                        add = lt::parse_magnet_uri(task.source, error);
                    }
                }
                if (error) throw std::runtime_error(error.message());
                add.save_path = path_utf8(task.save_path);
                add.max_connections = config_.connections_per_task;
                const bool stage_magnet_metadata = task.source_kind == "magnet"
                    && task.state != TaskState::paused && task.state != TaskState::completed
                    && task.state != TaskState::error;
                if (stage_magnet_metadata) {
                    add.flags |= lt::torrent_flags::upload_mode;
                    add.flags &= ~lt::torrent_flags::auto_managed;
                } else if (task.source_kind == "magnet" && !add.ti) {
                    add.flags |= lt::torrent_flags::upload_mode;
                    add.flags &= ~lt::torrent_flags::auto_managed;
                }
                const bool should_resume = task.state != TaskState::paused
                    && task.state != TaskState::completed && task.state != TaskState::error;
                add.flags |= lt::torrent_flags::paused;
                auto handle = session_->add_torrent(std::move(add), error);
                if (error) throw std::runtime_error(error.message());
                apply_additional_trackers_locked(handle, false);
                if (should_resume) handle.resume();
                handles_.emplace(task.id, handle);
                if (stage_magnet_metadata) {
                    metadata_started_.insert_or_assign(task.id, std::chrono::steady_clock::now());
                }
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
    return false;
}

void Engine::request_resume_save_locked(const std::string& id, bool only_if_modified) {
    const auto found = handles_.find(id);
    if (found == handles_.end() || !found->second.is_valid()) return;
    if (pending_resume_saves_.contains(id)) {
        if (!only_if_modified) deferred_resume_saves_.insert(id);
        return;
    }
    auto flags = lt::torrent_handle::save_info_dict;
    if (only_if_modified) flags |= lt::torrent_handle::only_if_modified;
    found->second.save_resume_data(flags);
    pending_resume_saves_.insert(id);
}

void Engine::request_periodic_resume_saves_locked() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_resume_save_) return;
    for (const auto& [id, handle] : handles_) {
        (void)handle;
        request_resume_save_locked(id, true);
    }
    next_resume_save_ = now + resume_save_interval;
}

std::optional<std::string> Engine::task_id_for_handle_locked(const lt::torrent_handle& handle) const {
    for (const auto& [id, candidate] : handles_) {
        if (candidate == handle) return id;
    }
    return std::nullopt;
}

void Engine::process_alerts_locked() {
    std::vector<lt::alert*> alerts;
    session_->pop_alerts(&alerts);
    for (const auto* alert : alerts) {
        if (const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {
            const auto id = task_id_for_handle_locked(saved->handle);
            if (!id) continue;
            pending_resume_saves_.erase(*id);
            try {
                write_resume_data_locked(*id, saved->params);
                if (const auto task = tasks_.find(*id); task != tasks_.end()
                    && task->second.last_error && task->second.last_error->code == "RESUME_SAVE_FAILED") {
                    task->second.last_error.reset();
                    emit_task("event.taskUpdated", task->second);
                }
            } catch (const std::exception& exception) {
                if (const auto task = tasks_.find(*id); task != tasks_.end()) {
                    task->second.last_error = TaskError{"RESUME_SAVE_FAILED", exception.what(), true};
                    emit_task("event.taskUpdated", task->second);
                }
            }
            if (deferred_resume_saves_.erase(*id) != 0) request_resume_save_locked(*id, false);
            continue;
        }
        if (const auto* failed = lt::alert_cast<lt::save_resume_data_failed_alert>(alert)) {
            const auto id = task_id_for_handle_locked(failed->handle);
            if (!id) continue;
            pending_resume_saves_.erase(*id);
            if (failed->error != lt::errors::make_error_code(lt::errors::resume_data_not_modified)) {
                if (const auto task = tasks_.find(*id); task != tasks_.end()) {
                    task->second.last_error = TaskError{"RESUME_SAVE_FAILED", failed->error.message(), true};
                    emit_task("event.taskUpdated", task->second);
                }
            }
            if (deferred_resume_saves_.erase(*id) != 0) request_resume_save_locked(*id, false);
            continue;
        }
        if (const auto* failed = lt::alert_cast<lt::file_error_alert>(alert)) {
            const auto id = task_id_for_handle_locked(failed->handle);
            if (!id || tasks_.at(*id).state == TaskState::error) continue;
            const auto mapped = map_libtorrent_error(failed->error, LibtorrentErrorContext::storage);
            fail_task_locked(*id, mapped.code, mapped.message, mapped.retryable);
            continue;
        }
        if (const auto* file_priority = lt::alert_cast<lt::file_prio_alert>(alert)) {
            const auto id = task_id_for_handle_locked(file_priority->handle);
            if (id) request_resume_save_locked(*id, false);
            continue;
        }
        if (const auto* failed = lt::alert_cast<lt::torrent_error_alert>(alert)) {
            const auto id = task_id_for_handle_locked(failed->handle);
            if (!id || tasks_.at(*id).state == TaskState::error) continue;
            const auto mapped = map_libtorrent_error(failed->error, LibtorrentErrorContext::torrent);
            fail_task_locked(*id, mapped.code, mapped.message, mapped.retryable);
        }
    }
}

void Engine::save_final_resume_data_locked() {
    if (!session_ || handles_.empty()) return;
    process_alerts_locked();
    for (const auto& [id, handle] : handles_) {
        (void)handle;
        request_resume_save_locked(id, false);
    }
    const auto deadline = std::chrono::steady_clock::now() + final_resume_timeout;
    while ((!pending_resume_saves_.empty() || !deferred_resume_saves_.empty())
           && std::chrono::steady_clock::now() < deadline) {
        session_->wait_for_alert(std::chrono::milliseconds(100));
        process_alerts_locked();
    }
}

void Engine::fail_task_locked(const std::string& id, std::string code, std::string message, bool retryable) {
    const auto handle = handles_.find(id);
    if (handle != handles_.end()) handle->second.pause();
    auto& task = tasks_.at(id);
    if (task.source_kind == "http") http_downloads_.cancel(id);
    task.state = TaskState::error;
    task.download_rate = 0;
    task.upload_rate = 0;
    task.last_error = TaskError{std::move(code), std::move(message), retryable};
    metadata_started_.erase(id);
    request_resume_save_locked(id, false);
    persist_catalog_locked();
    emit_task("event.taskUpdated", task);
}

bool Engine::validate_magnet_metadata_locked(const std::string& id,
    const lt::torrent_handle& handle, const std::shared_ptr<const lt::torrent_info>& info) {
    auto& task = tasks_.at(id);
    for (lt::file_index_t index{0}; index < info->files().end_file(); ++index) {
        if (!is_safe_relative_torrent_path(path_from_utf8(info->files().file_path(index)))) {
            fail_task_locked(id, "UNSAFE_TORRENT_PATH", "torrent contains an unsafe file path", false);
            return false;
        }
    }

    const auto save_validation = validate_save_path(task.save_path);
    if (!save_validation.valid) {
        fail_task_locked(id, save_validation.error_code, save_validation.message,
            save_validation.error_code != "SAVE_PATH_INVALID");
        return false;
    }

    std::error_code error;
    const auto space = std::filesystem::space(save_validation.normalized, error);
    if (error) {
        fail_task_locked(id, "SAVE_PATH_UNAVAILABLE", "cannot query savePath free space: " + error.message(), true);
        return false;
    }
    const auto required = static_cast<std::uint64_t>(std::max<std::int64_t>(0, info->total_size()));
    if (required > space.available) {
        fail_task_locked(id, "DISK_FULL",
            "not enough free space for torrent payload (required " + std::to_string(required)
                + " bytes, available " + std::to_string(space.available) + " bytes)",
            true);
        return false;
    }

    handle.unset_flags(lt::torrent_flags::upload_mode);
    handle.set_flags(lt::torrent_flags::auto_managed);
    task.save_path = save_validation.normalized;
    task.total_bytes = required;
    task.private_torrent = info->priv();
    if (task.display_name.empty()) task.display_name = info->name();
    task.last_error.reset();
    apply_additional_trackers_locked(handle, false);
    metadata_started_.erase(id);
    return true;
}

bool Engine::load_resume_data_locked(const TaskSnapshot& task, lt::add_torrent_params& add) {
    const auto path = state_path_ / "resume" / (task.id + ".fastresume");
    if (!std::filesystem::exists(path)) return false;
    try {
        const auto size = std::filesystem::file_size(path);
        if (size == 0 || size > max_resume_file_bytes) throw std::runtime_error("resume data size is invalid");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open resume data");
        std::vector<char> bytes(static_cast<std::size_t>(size));
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input) throw std::runtime_error("cannot read resume data");
        lt::error_code error;
        auto restored = lt::read_resume_data(lt::span<char const>(bytes.data(), bytes.size()), error);
        if (error) throw std::runtime_error(error.message());
        const auto restored_hash = hash_string(restored.info_hashes);
        if (!task.info_hash.empty() && restored_hash != task.info_hash) {
            throw std::runtime_error("resume data info-hash does not match the task catalog");
        }
        if (restored.ti) {
            for (lt::file_index_t index{0}; index < restored.ti->files().end_file(); ++index) {
                if (!is_safe_relative_torrent_path(path_from_utf8(restored.ti->files().file_path(index)))) {
                    throw std::runtime_error("resume data contains an unsafe torrent path");
                }
            }
        }
        add = std::move(restored);
        return true;
    } catch (const std::exception&) {
        const auto suffix = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        auto quarantined = path;
        quarantined += ".corrupt." + suffix;
        std::error_code error;
        std::filesystem::rename(path, quarantined, error);
        return false;
    }
}

void Engine::write_resume_data_locked(const std::string& id, const lt::add_torrent_params& add) {
    const auto directory = state_path_ / "resume";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) throw std::runtime_error("cannot create resume directory: " + error.message());
    const auto bytes = lt::write_resume_data_buf(add);
    const auto target = directory / (id + ".fastresume");
    const auto temporary = directory / (id + ".fastresume.tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write resume data");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) throw std::runtime_error("cannot flush resume data");
    }
    if (!replace_file(temporary, target)) throw std::runtime_error("cannot atomically replace resume data");
}

TaskSnapshot& Engine::require_task_locked(const std::string& id) {
    const auto found = tasks_.find(id);
    if (found == tasks_.end()) fail(-32004, "TASK_NOT_FOUND", "task does not exist");
    return found->second;
}

void Engine::emit_task(const std::string& event, const TaskSnapshot& task) {
    pending_task_updates_.erase(task.id);
    event_sink_(event, {{"sequence", ++sequence_}, {"task", task}});
}

bool Engine::update_snapshots_locked(bool emit_events) {
    bool catalog_changed = false;
    for (auto& [id, handle] : handles_) {
        auto& task = tasks_.at(id);
        if (task.source_kind == "magnet" && metadata_started_.contains(id)) {
            if (const auto info = handle.torrent_file()) {
                if (!validate_magnet_metadata_locked(id, handle, info)) {
                    catalog_changed = true;
                    continue;
                }
                task.state = TaskState::queued;
                request_resume_save_locked(id, false);
                persist_catalog_locked();
                emit_task("event.taskUpdated", task);
                catalog_changed = true;
                continue;
            }
            const auto timeout = std::chrono::seconds(config_.metadata_timeout_seconds);
            if (std::chrono::steady_clock::now() - metadata_started_.at(id) >= timeout) {
                fail_task_locked(id, "METADATA_TIMEOUT", "timed out while fetching magnet metadata", true);
                catalog_changed = true;
                continue;
            }
        }
        const auto status = handle.status(lt::torrent_handle::query_name);
        bool checking = status.state == lt::torrent_status::checking_files
            || status.state == lt::torrent_status::checking_resume_data;
#if TORRENT_ABI_VERSION == 1
        checking = checking || status.state == lt::torrent_status::queued_for_checking;
#endif
        if (resume_after_recheck_.contains(id) && !checking) {
            handle.set_flags(lt::torrent_flags::auto_managed);
            resume_after_recheck_.erase(id);
            continue;
        }
        if (task.state == TaskState::seeding
            && (status.state == lt::torrent_status::finished || status.state == lt::torrent_status::seeding)) {
            const auto now = std::chrono::steady_clock::now();
            const auto probe = next_payload_probe_.find(id);
            if (probe == next_payload_probe_.end() || now >= probe->second) {
                next_payload_probe_[id] = now + payload_probe_interval;
                if (payload_is_missing(task, handle)) {
                    start_recheck_locked(task, handle);
                    catalog_changed = true;
                    continue;
                }
            }
        } else {
            next_payload_probe_.erase(id);
        }
        const auto previous_state = task.state;
        const auto previous_downloaded = task.downloaded_bytes;
        const auto previous_uploaded = task.uploaded_bytes;
        const auto previous_seeding_seconds = task.seeding_seconds;
        const auto previous_download_rate = task.download_rate;
        const auto previous_upload_rate = task.upload_rate;
        const auto previous_peers = task.peers;
        const auto previous_seeds = task.seeds;
        auto next_state = state_from_status(status);
        task.total_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.total_wanted));
        task.downloaded_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.total_wanted_done));
        task.verified_bytes = task.downloaded_bytes;
        task.uploaded_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, status.all_time_upload));
        task.seeding_seconds = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, status.finished_duration.count()));
        task.share_ratio = task.total_bytes == 0 ? 0.0
            : static_cast<double>(task.uploaded_bytes) / static_cast<double>(task.total_bytes);
        task.seed_ratio_limit = config_.seed_ratio_limit;
        task.seed_time_limit_minutes = config_.seed_time_limit_minutes;
        task.download_rate = static_cast<std::uint64_t>(std::max(0, status.download_payload_rate));
        task.upload_rate = static_cast<std::uint64_t>(std::max(0, status.upload_payload_rate));
        task.peers = status.num_peers;
        task.seeds = status.num_seeds;
        if (task.display_name.empty()) task.display_name = status.name;
        if (task.info_hash.empty()) task.info_hash = hash_string(status.info_hashes);
        if (const auto info = handle.torrent_file()) task.private_torrent = info->priv();

        if (task.state == TaskState::error) next_state = TaskState::error;
        else if (status.errc) {
            next_state = TaskState::error;
            task.last_error = map_libtorrent_error(status.errc, LibtorrentErrorContext::torrent);
        } else if (task.seed_stop_reason && next_state != TaskState::checking) {
            next_state = TaskState::completed;
        } else if (previous_state == TaskState::completed && next_state != TaskState::checking) {
            next_state = TaskState::completed;
        } else if (next_state == TaskState::seeding) {
            if (const auto reason = evaluate_seed_stop(effective_seeding_enabled(), config_.seed_ratio_limit,
                    config_.seed_time_limit_minutes, task.uploaded_bytes, task.total_bytes,
                    task.seeding_seconds)) {
                task.seed_stop_reason = reason;
                next_state = TaskState::completed;
            }
        }

        const bool state_changed = next_state != previous_state;
        task.state = next_state;
        if (previous_state == TaskState::checking
            && (task.state == TaskState::downloading || task.state == TaskState::seeding)) {
            // force_recheck stops announcing while it queues the check. Start a
            // fresh announce once the payload state is known so a repaired task
            // can find peers without waiting for the tracker's next interval.
            handle.force_reannounce(0, -1, lt::torrent_handle::ignore_min_interval);
        }
        if (task.state == TaskState::completed
            && (previous_state != TaskState::completed
                || (handle.flags() & lt::torrent_flags::paused) == lt::torrent_flags_t{})) {
            handle.pause();
            task.download_rate = 0;
            task.upload_rate = 0;
        }
        const bool progress_changed = previous_downloaded != task.downloaded_bytes
            || previous_uploaded != task.uploaded_bytes
            || previous_seeding_seconds != task.seeding_seconds
            || previous_download_rate != task.download_rate || previous_upload_rate != task.upload_rate
            || previous_peers != task.peers || previous_seeds != task.seeds;
        if (state_changed || progress_changed) {
            if (emit_events) emit_task("event.taskUpdated", task);
            else pending_task_updates_.insert(id);
        } else if (emit_events && pending_task_updates_.contains(id)) {
            emit_task("event.taskUpdated", task);
        }
        if (state_changed) request_resume_save_locked(id, false);
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
            if (initialized_ && session_) {
                process_alerts_locked();
                request_periodic_resume_saves_locked();
                bool catalog_changed = update_snapshots_locked(true);
                catalog_changed = update_http_tasks_locked(true) || catalog_changed;
                catalog_changed = schedule_http_tasks_locked() || catalog_changed;
                if (catalog_changed) persist_catalog_locked();
            }
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
