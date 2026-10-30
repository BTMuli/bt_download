#include "tracker_peers.hpp"

#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

void run_tracker_peers_tests() {
    using Store = bt::TrackerPeerStore;
    using namespace std::chrono_literals;
    const auto start = Store::Clock::now();
    Store store(3, 30min);
    store.observe("127.0.0.1:6881", "https://a/announce", start);
    store.observe("127.0.0.1:6881", "udp://b:80/announce", start + 1min);
    store.observe("[::1]:6881", "https://a/announce", start + 2min);
    expect(store.urls("127.0.0.1:6881", start + 2min)
            == std::vector<std::string>({"https://a/announce", "udp://b:80/announce"}),
        "one peer must retain multiple exact tracker URLs");
    expect(store.urls("127.0.0.1:6882", start + 2min).empty(),
        "port changes must not inherit another peer's provenance");
    expect(store.urls("[::1]:6881", start + 2min).size() == 1,
        "IPv6 endpoints must remain independent");
    store.observe("127.0.0.1:6881", "https://a/announce", start + 3min);
    store.observe("127.0.0.2:6881", "https://c/announce", start + 4min);
    expect(store.urls("127.0.0.1:6881", start + 4min)
            == std::vector<std::string>({"https://a/announce"}),
        "capacity must evict the oldest association, not the refreshed one");
    expect(store.urls("[::1]:6881", start + 32min).empty(),
        "an expired association must disappear even if another peer was refreshed");
    expect(store.urls("127.0.0.1:6881", start + 32min).size() == 1,
        "a refreshed association expired too early");
    expect(store.urls("127.0.0.1:6881", start + 33min).empty(),
        "association must expire at its retention boundary");
    Store another;
    expect(another.urls("127.0.0.1:6881").empty(),
        "task/session stores must not share provenance");
    Store disabled(0);
    disabled.observe("127.0.0.1:6881", "https://a/announce");
    expect(disabled.urls("127.0.0.1:6881").empty(), "capacity zero must retain nothing");
}
