#pragma once

#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <libtorrent/fwd.hpp>

namespace bt {

// Runtime observations of real tracker responses, never inferred from timing
// or a peer's generic tracker source flag. One peer can have several trackers.
class TrackerPeerStore {
public:
    using Clock = std::chrono::steady_clock;
    explicit TrackerPeerStore(std::size_t capacity = 20000,
        Clock::duration retention = std::chrono::minutes(30));

    void observe(std::string endpoint, std::string url,
        Clock::time_point now = Clock::now());
    std::vector<std::string> urls(const std::string& endpoint,
        Clock::time_point now = Clock::now());

private:
    using Key = std::pair<std::string, std::string>;
    struct Entry {
        Clock::time_point seen;
        std::list<Key>::iterator order;
    };
    void prune_locked(Clock::time_point now);

    const std::size_t capacity_;
    const Clock::duration retention_;
    std::mutex mutex_;
    std::map<Key, Entry> entries_;
    std::list<Key> order_;
};

std::shared_ptr<TrackerPeerStore> track_tracker_peers(libtorrent::add_torrent_params& params);

} // namespace bt
