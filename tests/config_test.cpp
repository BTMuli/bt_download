#include "bt_download/config.hpp"
#include "seeding_policy.hpp"
#include "tracker_config.hpp"

#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expect_invalid(Function function, const char* message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

} // namespace

void run_config_tests() {
    expect(bt::normalize_tracker_url("HTTP://Tracker.Example:80/announce?pass=AbC")
            == "http://tracker.example/announce?pass=AbC",
        "HTTP tracker normalization failed");
    expect(bt::normalize_tracker_url("udp://[2001:DB8::1]:6969/announce")
            == "udp://[2001:db8::1]:6969/announce",
        "IPv6 tracker normalization failed");
    const auto trackers = bt::normalize_tracker_urls(nlohmann::json::array({
        "https://tracker.example:443/announce", "HTTPS://TRACKER.EXAMPLE/announce"}));
    expect(trackers.size() == 1 && trackers.front() == "https://tracker.example/announce",
        "tracker deduplication failed");
    expect_invalid([] { bt::normalize_tracker_url("udp://tracker.example/announce"); },
        "UDP tracker without port was accepted");
    expect_invalid([] { bt::normalize_tracker_url("https://user:secret@tracker.example/announce"); },
        "tracker credentials were accepted");
    expect_invalid([] { bt::normalize_tracker_url("https://tracker.example/announce#fragment"); },
        "tracker fragment was accepted");

    bt::EngineConfig config;
    config = bt::apply_config_patch(config, {
        {"additionalTrackers", nlohmann::json::array({"udp://tracker.example:6969/announce"})},
        {"seedingEnabled", true}, {"seedRatioLimit", 2.0}, {"seedTimeLimitMinutes", 60}});
    expect(config.seeding_enabled && config.additional_trackers.size() == 1,
        "valid Tracker/seeding config was not applied");
    expect_invalid([&] {
        bt::apply_config_patch(config,
            {{"seedingEnabled", true}, {"seedRatioLimit", 0.0}, {"seedTimeLimitMinutes", 0}});
    }, "unbounded seeding config was accepted");

    expect(bt::evaluate_seed_stop(true, 2.0, 60, 199, 100, 3599) == std::nullopt,
        "seeding stopped before either limit");
    expect(bt::evaluate_seed_stop(true, 2.0, 60, 200, 100, 1) == bt::SeedStopReason::ratio,
        "ratio stop condition failed");
    expect(bt::evaluate_seed_stop(true, 2.0, 60, 0, 100, 3600) == bt::SeedStopReason::time,
        "time stop condition failed");
    expect(bt::evaluate_seed_stop(false, 2.0, 60, 0, 100, 0) == bt::SeedStopReason::disabled,
        "disabled seeding stop condition failed");
}
