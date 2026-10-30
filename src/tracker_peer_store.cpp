#include "tracker_peers.hpp"

#include <iterator>

namespace bt {

TrackerPeerStore::TrackerPeerStore(std::size_t capacity, Clock::duration retention)
    : capacity_(capacity), retention_(retention) {}

void TrackerPeerStore::prune_locked(Clock::time_point now) {
    while (!order_.empty()) {
        const auto entry = entries_.find(order_.front());
        if (entries_.size() <= capacity_ && now - entry->second.seen < retention_) break;
        entries_.erase(entry);
        order_.pop_front();
    }
}

void TrackerPeerStore::observe(std::string endpoint, std::string url, Clock::time_point now) {
    if (endpoint.empty() || url.empty()) return;
    std::scoped_lock lock(mutex_);
    prune_locked(now);
    Key key{std::move(endpoint), std::move(url)};
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        found->second.seen = now;
        order_.splice(order_.end(), order_, found->second.order);
    } else {
        order_.push_back(key);
        try {
            entries_.emplace(std::move(key), Entry{now, std::prev(order_.end())});
        } catch (...) {
            order_.pop_back();
            throw;
        }
    }
    prune_locked(now);
}

std::vector<std::string> TrackerPeerStore::urls(const std::string& endpoint, Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    prune_locked(now);
    std::vector<std::string> result;
    for (auto entry = entries_.lower_bound({endpoint, {}});
         entry != entries_.end() && entry->first.first == endpoint; ++entry) {
        result.push_back(entry->first.second);
    }
    return result;
}

} // namespace bt
