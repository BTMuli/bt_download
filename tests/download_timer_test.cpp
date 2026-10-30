#include "bt_download/download_timer.hpp"

#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

void run_download_timer_tests() {
    using Timer = bt::DownloadTimer;
    using namespace std::chrono_literals;
    const auto start = Timer::Clock::time_point{};
    Timer timer;
    expect(timer.sample(false, start) == 0, "idle task started counting");
    expect(timer.sample(true, start + 10s) == 0, "zero-speed interval was backfilled");
    expect(timer.sample(true, start + 11s) == 1000, "active interval was not counted");
    expect(timer.sample(true, start + 11s) == 0, "repeated reads double-counted time");
    expect(timer.sample(false, start + 12s) == 1000, "last active interval was lost");
    expect(timer.sample(false, start + 1h) == 0, "stalled task kept counting");
    expect(timer.sample(true, start + 2h) == 0, "resume counted stalled time");
    expect(timer.sample(true, start + 2h + 400us) == 0, "fraction rounded too early");
    expect(timer.sample(false, start + 2h + 1400us) == 1, "fractional milliseconds were discarded");
    expect(timer.sample(false, start + 2h + 1s) == 0, "inactive fraction grew");
    expect(timer.sample(true, start + 2h + 2s) == 0, "inactive interval was counted");
    expect(timer.sample(true, start + 2h + 2600ms) == 600, "active remainder was not preserved");
    expect(timer.sample(false, start) == 0, "backward timestamp duplicated time");

    Timer restored;
    expect(restored.sample(true, start + 24h) == 0, "restart counted time while the app was closed");
    expect(restored.sample(false, start + 24h + 1s) == 1000, "restored task did not start a fresh interval");

    Timer frequent, sparse;
    frequent.sample(true, start);
    sparse.sample(true, start);
    std::uint64_t total = 0;
    for (int i = 1; i <= 10000; ++i) total += frequent.sample(true, start + i * 100us);
    expect(total == sparse.sample(true, start + 1s) && total == 1000,
        "polling frequency changed accumulated time");
}
