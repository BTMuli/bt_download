#pragma once

#include "bt_download/config.hpp"
#include "bt_download/http_download.hpp"
#include "bt_download/proxy_config.hpp"
#include "bt_download/task.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <libtorrent/session.hpp>
#include <nlohmann/json.hpp>

namespace bt {

class Engine {
public:
    using EventSink = std::function<void(const std::string&, const nlohmann::json&)>;

    explicit Engine(EventSink event_sink);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    nlohmann::json dispatch(const std::string& method, const nlohmann::json& params);
    bool shutdown_requested() const noexcept;

private:
    nlohmann::json initialize(const nlohmann::json& params);
    nlohmann::json status() const;
    nlohmann::json configure(const nlohmann::json& params);
    nlohmann::json configure_proxy(const nlohmann::json& params);
    nlohmann::json add_task(const nlohmann::json& params);
    nlohmann::json list_tasks();
    nlohmann::json get_task(const nlohmann::json& params);
    nlohmann::json get_task_details(const nlohmann::json& params);
    nlohmann::json get_task_files(const nlohmann::json& params);
    nlohmann::json get_task_peers(const nlohmann::json& params);
    nlohmann::json set_file_priorities(const nlohmann::json& params);
    nlohmann::json pause_task(const nlohmann::json& params, bool stop = false);
    nlohmann::json resume_task(const nlohmann::json& params);
    nlohmann::json retry_task(const nlohmann::json& params);
    nlohmann::json recheck_task(const nlohmann::json& params);
    nlohmann::json remove_task(const nlohmann::json& params);
    nlohmann::json shutdown();

    void require_initialized() const;
    nlohmann::json task_overview_locked(const std::string& id);
    nlohmann::json task_files_locked(const std::string& id, std::size_t offset, std::size_t limit);
    nlohmann::json task_peers_locked(const std::string& id, std::size_t offset, std::size_t limit);
    void finalize_locked();
    void persist_catalog_locked();
    bool load_catalog_locked();
    void request_resume_save_locked(const std::string& id, bool only_if_modified);
    void request_periodic_resume_saves_locked();
    void process_alerts_locked();
    void save_final_resume_data_locked();
    bool validate_magnet_metadata_locked(const std::string& id,
        const libtorrent::torrent_handle& handle,
        const std::shared_ptr<const libtorrent::torrent_info>& info);
    void apply_additional_trackers_locked(const libtorrent::torrent_handle& handle, bool reannounce);
    void apply_additional_trackers_to_all_locked(bool reannounce);
    void apply_shared_download_budget_locked();
    bool effective_seeding_enabled() const noexcept;
    std::optional<SeedStopReason> seed_stop_reason_for(const TaskSnapshot& task) const;
    EngineConfig effective_config() const;
    void refresh_system_cost_locked();
    bool schedule_http_tasks_locked();
    bool update_http_tasks_locked(bool emit_events);
    std::filesystem::path http_target_path_locked(const std::string& id) const;
    std::filesystem::path http_partial_path_locked(const std::string& id) const;
    std::string reserve_http_file_name_locked(const std::filesystem::path& save_path,
        const std::string& suggested, const std::string& except_id = {}) const;
    std::filesystem::path apply_resolved_http_file_name_locked(
        const std::string& id, std::string_view suggested);
    void fail_task_locked(const std::string& id, std::string code, std::string message, bool retryable);
    bool load_resume_data_locked(const TaskSnapshot& task, libtorrent::add_torrent_params& add);
    void write_resume_data_locked(const std::string& id, const libtorrent::add_torrent_params& add);
    void start_recheck_locked(TaskSnapshot& task, libtorrent::torrent_handle& handle);
    std::optional<std::string> task_id_for_handle_locked(const libtorrent::torrent_handle& handle) const;
    TaskSnapshot& require_task_locked(const std::string& id);
    void emit_task(const std::string& event, const TaskSnapshot& task);
    bool update_snapshots_locked(bool emit_events);
    void worker_loop(std::stop_token stop_token);
    std::string new_task_id() const;

    EventSink event_sink_;
    mutable std::mutex mutex_;
    std::unique_ptr<libtorrent::session> session_;
    HttpDownloadManager http_downloads_;
    std::unordered_map<std::string, TaskSnapshot> tasks_;
    std::unordered_map<std::string, std::string> http_file_names_;
    std::unordered_map<std::string, libtorrent::torrent_handle> handles_;
    std::unordered_map<std::string, std::vector<libtorrent::download_priority_t>> file_priorities_;
    std::unordered_set<std::string> pending_resume_saves_;
    std::unordered_set<std::string> deferred_resume_saves_;
    std::unordered_set<std::string> pending_task_updates_;
    std::unordered_set<std::string> resume_after_recheck_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> metadata_started_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> next_payload_probe_;
    std::filesystem::path state_path_;
    EngineConfig config_;
    EngineProxyConfig proxy_;
    std::string user_agent_;
    std::chrono::steady_clock::time_point started_at_;
    std::chrono::steady_clock::time_point next_resume_save_;
    std::chrono::steady_clock::time_point next_system_cost_check_;
    std::uint64_t sequence_{0};
    std::jthread worker_;
    bool initialized_{false};
    bool shutdown_requested_{false};
    bool constrained_mode_{false};
};

} // namespace bt
