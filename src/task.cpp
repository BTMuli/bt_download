#include "bt_download/task.hpp"

#include <stdexcept>

namespace bt {
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

} // namespace

std::string_view to_string(TaskState state) {
    switch (state) {
    case TaskState::metadata: return "metadata";
    case TaskState::checking: return "checking";
    case TaskState::queued: return "queued";
    case TaskState::downloading: return "downloading";
    case TaskState::seeding: return "seeding";
    case TaskState::paused: return "paused";
    case TaskState::completed: return "completed";
    case TaskState::error: return "error";
    }
    return "error";
}

TaskState task_state_from_string(std::string_view state) {
    if (state == "metadata") return TaskState::metadata;
    if (state == "checking") return TaskState::checking;
    if (state == "queued") return TaskState::queued;
    if (state == "downloading") return TaskState::downloading;
    if (state == "seeding") return TaskState::seeding;
    if (state == "paused") return TaskState::paused;
    if (state == "completed") return TaskState::completed;
    if (state == "error") return TaskState::error;
    throw std::invalid_argument("unknown task state");
}

std::string_view to_string(SeedStopReason reason) {
    switch (reason) {
    case SeedStopReason::disabled: return "disabled";
    case SeedStopReason::ratio: return "ratio";
    case SeedStopReason::time: return "time";
    }
    return "disabled";
}

SeedStopReason seed_stop_reason_from_string(std::string_view reason) {
    if (reason == "disabled") return SeedStopReason::disabled;
    if (reason == "ratio") return SeedStopReason::ratio;
    if (reason == "time") return SeedStopReason::time;
    throw std::invalid_argument("unknown seed stop reason");
}

void to_json(nlohmann::json& json, const TaskError& error) {
    json = {{"code", error.code}, {"message", error.message}, {"retryable", error.retryable}};
}

void from_json(const nlohmann::json& json, TaskError& error) {
    json.at("code").get_to(error.code);
    json.at("message").get_to(error.message);
    error.retryable = json.value("retryable", false);
}

void to_json(nlohmann::json& json, const TaskSnapshot& task) {
    json = {
        {"id", task.id}, {"state", to_string(task.state)}, {"sourceKind", task.source_kind},
        {"savePath", path_utf8(task.save_path)}, {"displayName", task.display_name},
        {"infoHash", task.info_hash.empty() ? nlohmann::json(nullptr) : nlohmann::json(task.info_hash)},
        {"totalBytes", task.total_bytes}, {"downloadedBytes", task.downloaded_bytes},
        {"verifiedBytes", task.verified_bytes}, {"uploadedBytes", task.uploaded_bytes},
        {"shareRatio", task.share_ratio}, {"seedingSeconds", task.seeding_seconds},
        {"seedRatioLimit", task.seed_ratio_limit}, {"seedTimeLimitMinutes", task.seed_time_limit_minutes},
        {"seedStopReason", task.seed_stop_reason
                ? nlohmann::json(to_string(*task.seed_stop_reason)) : nlohmann::json(nullptr)},
        {"progress", task.total_bytes == 0 ? 0.0 : static_cast<double>(task.downloaded_bytes) / task.total_bytes},
        {"downloadRate", task.download_rate}, {"uploadRate", task.upload_rate},
        {"peers", task.peers}, {"seeds", task.seeds}, {"private", task.private_torrent},
        {"lastError", task.last_error ? nlohmann::json(*task.last_error) : nlohmann::json(nullptr)},
    };
}

void from_json(const nlohmann::json& json, TaskSnapshot& task) {
    json.at("id").get_to(task.id);
    task.state = task_state_from_string(json.at("state").get<std::string>());
    json.at("sourceKind").get_to(task.source_kind);
    task.source = json.value("source", std::string{});
    task.save_path = path_from_utf8(json.at("savePath").get<std::string>());
    task.display_name = json.value("displayName", std::string{});
    if (json.contains("infoHash") && !json["infoHash"].is_null()) json.at("infoHash").get_to(task.info_hash);
    task.total_bytes = json.value("totalBytes", std::uint64_t{0});
    task.downloaded_bytes = json.value("downloadedBytes", std::uint64_t{0});
    task.verified_bytes = json.value("verifiedBytes", std::uint64_t{0});
    task.uploaded_bytes = json.value("uploadedBytes", std::uint64_t{0});
    task.share_ratio = json.value("shareRatio", 0.0);
    task.seeding_seconds = json.value("seedingSeconds", std::uint64_t{0});
    task.seed_ratio_limit = json.value("seedRatioLimit", 2.0);
    task.seed_time_limit_minutes = json.value("seedTimeLimitMinutes", 60);
    if (json.contains("seedStopReason") && !json["seedStopReason"].is_null()) {
        task.seed_stop_reason = seed_stop_reason_from_string(json.at("seedStopReason").get<std::string>());
    }
    task.private_torrent = json.value("private", false);
    if (json.contains("lastError") && !json["lastError"].is_null()) task.last_error = json["lastError"].get<TaskError>();
}

bool can_transition(TaskState from, TaskState to) {
    if (from == to) return true;
    if (to == TaskState::error || to == TaskState::paused) return from != TaskState::completed;
    switch (from) {
    case TaskState::metadata: return to == TaskState::checking || to == TaskState::queued;
    case TaskState::checking: return to == TaskState::queued || to == TaskState::downloading
        || to == TaskState::seeding || to == TaskState::completed;
    case TaskState::queued: return to == TaskState::metadata || to == TaskState::checking || to == TaskState::downloading;
    case TaskState::downloading: return to == TaskState::checking || to == TaskState::seeding || to == TaskState::completed;
    case TaskState::seeding: return to == TaskState::checking || to == TaskState::completed;
    case TaskState::paused: return to == TaskState::metadata || to == TaskState::checking
        || to == TaskState::queued || to == TaskState::downloading || to == TaskState::seeding;
    case TaskState::error: return to == TaskState::metadata || to == TaskState::checking || to == TaskState::queued;
    case TaskState::completed: return to == TaskState::checking;
    }
    return false;
}

} // namespace bt
