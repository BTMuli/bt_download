#include "bt_download/task.hpp"

#include <stdexcept>

namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

void run_task_tests() {
    expect(bt::can_transition(bt::TaskState::queued, bt::TaskState::downloading), "queued to downloading rejected");
    expect(!bt::can_transition(bt::TaskState::completed, bt::TaskState::downloading), "completed to downloading accepted");
    expect(bt::task_state_from_string("metadata") == bt::TaskState::metadata, "state parsing failed");
    expect(bt::task_state_from_string("seeding") == bt::TaskState::seeding, "seeding state parsing failed");
    expect(bt::can_transition(bt::TaskState::downloading, bt::TaskState::seeding),
        "downloading to seeding rejected");
    bt::TaskSnapshot task;
    task.id = "test";
    task.save_path = "C:\\Anime";
    task.uploaded_bytes = 2048;
    task.seeding_seconds = 30;
    task.download_milliseconds = 123456;
    task.seed_stop_reason = bt::SeedStopReason::ratio;
    const nlohmann::json json = task;
    expect(json["id"] == "test", "task serialization failed");
    expect(json["progress"] == 0.0, "zero-size progress must be zero");
    expect(json["uploadedBytes"] == 2048, "uploaded bytes serialization failed");
    expect(json["downloadMilliseconds"] == 123456, "per-task download duration serialization failed");
    const auto restored = nlohmann::json::parse(json.dump()).get<bt::TaskSnapshot>();
    expect(restored.download_milliseconds == task.download_milliseconds,
        "persisted download duration changed during restore");
    expect(json["seedStopReason"] == "ratio", "seed stop reason serialization failed");
}
