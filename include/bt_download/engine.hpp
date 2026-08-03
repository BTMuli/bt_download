#pragma once

#include "bt_download/config.hpp"
#include "bt_download/task.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
    nlohmann::json add_task(const nlohmann::json& params);
    nlohmann::json list_tasks();
    nlohmann::json get_task(const nlohmann::json& params);
    nlohmann::json get_task_details(const nlohmann::json& params);
    nlohmann::json set_file_priorities(const nlohmann::json& params);
    nlohmann::json pause_task(const nlohmann::json& params);
    nlohmann::json resume_task(const nlohmann::json& params);
    nlohmann::json retry_task(const nlohmann::json& params);
    nlohmann::json recheck_task(const nlohmann::json& params);
    nlohmann::json remove_task(const nlohmann::json& params);
    nlohmann::json shutdown();

    void require_initialized() const;
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
    bool effective_seeding_enabled() const noexcept;
    void fail_task_locked(const std::string& id, std::string code, std::string message, bool retryable);
    bool load_resume_data_locked(const TaskSnapshot& task, libtorrent::add_torrent_params& add);
    void write_resume_data_locked(const std::string& id, const libtorrent::add_torrent_params& add);
    std::optional<std::string> task_id_for_handle_locked(const libtorrent::torrent_handle& handle) const;
    TaskSnapshot& require_task_locked(const std::string& id);
    void emit_task(const std::string& event, const TaskSnapshot& task);
    bool update_snapshots_locked(bool emit_events);
    void worker_loop(std::stop_token stop_token);
    std::string new_task_id() const;

    EventSink event_sink_;
    mutable std::mutex mutex_;
    std::unique_ptr<libtorrent::session> session_;
    std::unordered_map<std::string, TaskSnapshot> tasks_;
    std::unordered_map<std::string, libtorrent::torrent_handle> handles_;
    std::unordered_map<std::string, std::vector<libtorrent::download_priority_t>> file_priorities_;
    std::unordered_set<std::string> pending_resume_saves_;
    std::unordered_set<std::string> deferred_resume_saves_;
    std::unordered_set<std::string> pending_task_updates_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> metadata_started_;
    std::filesystem::path state_path_;
    EngineConfig config_;
    std::chrono::steady_clock::time_point started_at_;
    std::chrono::steady_clock::time_point next_resume_save_;
    std::uint64_t sequence_{0};
    std::jthread worker_;
    bool initialized_{false};
    bool shutdown_requested_{false};
    bool protocol_v1_1_features_{false};
};

} // namespace bt
