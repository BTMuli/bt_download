#pragma once

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
    nlohmann::json pause_task(const nlohmann::json& params);
    nlohmann::json resume_task(const nlohmann::json& params);
    nlohmann::json retry_task(const nlohmann::json& params);
    nlohmann::json recheck_task(const nlohmann::json& params);
    nlohmann::json remove_task(const nlohmann::json& params);
    nlohmann::json shutdown();

    void require_initialized() const;
    void persist_catalog_locked();
    void load_catalog_locked();
    TaskSnapshot& require_task_locked(const std::string& id);
    void emit_task(const std::string& event, const TaskSnapshot& task);
    bool update_snapshots_locked();
    void worker_loop(std::stop_token stop_token);
    std::string new_task_id() const;

    EventSink event_sink_;
    mutable std::mutex mutex_;
    std::unique_ptr<libtorrent::session> session_;
    std::unordered_map<std::string, TaskSnapshot> tasks_;
    std::unordered_map<std::string, libtorrent::torrent_handle> handles_;
    std::filesystem::path state_path_;
    nlohmann::json config_;
    std::chrono::steady_clock::time_point started_at_;
    std::uint64_t sequence_{0};
    std::jthread worker_;
    bool initialized_{false};
    bool shutdown_requested_{false};
};

} // namespace bt
