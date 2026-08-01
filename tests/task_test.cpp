#include "bt_download/task.hpp"

#include <stdexcept>

namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

void run_task_tests() {
    expect(bt::can_transition(bt::TaskState::queued, bt::TaskState::downloading), "queued to downloading rejected");
    expect(!bt::can_transition(bt::TaskState::completed, bt::TaskState::downloading), "completed to downloading accepted");
    expect(bt::task_state_from_string("metadata") == bt::TaskState::metadata, "state parsing failed");
    bt::TaskSnapshot task;
    task.id = "test";
    task.save_path = "C:\\Anime";
    const nlohmann::json json = task;
    expect(json["id"] == "test", "task serialization failed");
    expect(json["progress"] == 0.0, "zero-size progress must be zero");
}
