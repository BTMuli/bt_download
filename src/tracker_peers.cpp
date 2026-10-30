#include "tracker_peers.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/extensions.hpp>

#if !defined(BT_DOWNLOAD_LIBTORRENT_TRACKER_PEERS_VERSION) || BT_DOWNLOAD_LIBTORRENT_TRACKER_PEERS_VERSION != 1
#error "bt_download requires the libtorrent tracker-peer observer overlay; rebuild dependencies using vcpkg-configuration.json"
#endif

namespace bt {
namespace lt = libtorrent;
namespace {

class TrackerPeerPlugin final : public lt::torrent_plugin {
public:
    explicit TrackerPeerPlugin(std::shared_ptr<TrackerPeerStore> store) : store_(std::move(store)) {}

    void on_tracker_peer(lt::string_view url, const lt::tcp::endpoint& endpoint) override {
        const auto host = endpoint.address().to_string();
        store_->observe((endpoint.address().is_v6() ? "[" + host + "]" : host)
                + ":" + std::to_string(endpoint.port()),
            url.to_string());
    }

private:
    std::shared_ptr<TrackerPeerStore> store_;
};

} // namespace

std::shared_ptr<TrackerPeerStore> track_tracker_peers(lt::add_torrent_params& params) {
    auto store = std::make_shared<TrackerPeerStore>();
    params.extensions.push_back([store](const lt::torrent_handle&, lt::client_data_t) {
        return std::make_shared<TrackerPeerPlugin>(store);
    });
    return store;
}

} // namespace bt
