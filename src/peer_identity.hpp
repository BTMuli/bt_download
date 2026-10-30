#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <libtorrent/fwd.hpp>
#include <libtorrent/peer_id.hpp>
#include <libtorrent/socket.hpp>

namespace bt {

// One store per torrent. Entries belong to individual connections so a late
// disconnect cannot erase the identity of a replacement connection.
class PeerIdentityStore {
public:
    void connected(const libtorrent::tcp::endpoint& endpoint, const void* owner);
    void disconnected(const libtorrent::tcp::endpoint& endpoint, const void* owner);
    void reported(const libtorrent::tcp::endpoint& endpoint, const void* owner,
        const libtorrent::peer_id& pid, std::string user_agent);
    std::string user_agent(const libtorrent::tcp::endpoint& endpoint,
        const libtorrent::peer_id& pid) const;

private:
    struct Entry {
        const void* owner;
        libtorrent::peer_id pid;
        std::string user_agent;
    };

    mutable std::mutex mutex_;
    std::map<libtorrent::tcp::endpoint, Entry> peers_;
};

std::shared_ptr<PeerIdentityStore> track_peer_identities(
    libtorrent::add_torrent_params& params);

// Recognize documented Peer ID conventions without depending on deprecated
// identify_client(), which our libtorrent ABI 2 build does not export.
std::string identify_peer_client(const libtorrent::peer_id& pid);

} // namespace bt
