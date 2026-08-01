#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace bt {

enum class TaskState {
    metadata,
    checking,
    queued,
    downloading,
    paused,
    completed,
    error,
};

struct TaskError {
    std::string code;
    std::string message;
    bool retryable{false};
};

struct TaskSnapshot {
    std::string id;
    TaskState state{TaskState::queued};
    std::string source_kind;
    std::string source;
    std::filesystem::path save_path;
    std::string display_name;
    std::string info_hash;
    std::uint64_t total_bytes{0};
    std::uint64_t downloaded_bytes{0};
    std::uint64_t verified_bytes{0};
    std::uint64_t download_rate{0};
    std::uint64_t upload_rate{0};
    int peers{0};
    int seeds{0};
    bool private_torrent{false};
    std::optional<TaskError> last_error;
};

std::string_view to_string(TaskState state);
TaskState task_state_from_string(std::string_view state);

void to_json(nlohmann::json& json, const TaskError& error);
void from_json(const nlohmann::json& json, TaskError& error);
void to_json(nlohmann::json& json, const TaskSnapshot& task);
void from_json(const nlohmann::json& json, TaskSnapshot& task);

bool can_transition(TaskState from, TaskState to);

} // namespace bt
