#include "peer_identity.hpp"

#include <charconv>
#include <string_view>
#include <utility>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/peer_connection_handle.hpp>
#include <libtorrent/peer_info.hpp>
#include <nlohmann/json.hpp>

namespace bt {
namespace lt = libtorrent;
namespace {

class PeerIdentityPlugin final : public lt::peer_plugin {
public:
    PeerIdentityPlugin(std::shared_ptr<PeerIdentityStore> store,
        lt::peer_connection_handle connection)
        : store_(std::move(store)), connection_(std::move(connection)),
          endpoint_(connection_.remote()) {
        store_->connected(endpoint_, this);
    }

    ~PeerIdentityPlugin() override {
        store_->disconnected(endpoint_, this);
    }

    bool on_extension_handshake(const lt::bdecode_node& handshake) override {
        const auto value = handshake.dict_find_string_value("v");
        if (!value.empty()) {
            // This is untrusted peer text. Preserve valid UTF-8 and replace
            // malformed bytes rather than breaking the JSON-RPC response.
            const auto encoded = nlohmann::json(value.to_string()).dump(
                -1, ' ', false, nlohmann::json::error_handler_t::replace);
            store_->reported(endpoint_, this, connection_.pid(),
                nlohmann::json::parse(encoded).get<std::string>());
        }
        // Observe only; keep all other extensions and normal handling active.
        return true;
    }

private:
    std::shared_ptr<PeerIdentityStore> store_;
    lt::peer_connection_handle connection_;
    lt::tcp::endpoint endpoint_;
};

class TorrentIdentityPlugin final : public lt::torrent_plugin {
public:
    explicit TorrentIdentityPlugin(std::shared_ptr<PeerIdentityStore> store)
        : store_(std::move(store)) {}

    std::shared_ptr<lt::peer_plugin> new_connection(
        const lt::peer_connection_handle& connection) override {
        if (connection.type() != lt::connection_type::bittorrent) return {};
        return std::make_shared<PeerIdentityPlugin>(store_, connection);
    }

private:
    std::shared_ptr<PeerIdentityStore> store_;
};

} // namespace

void PeerIdentityStore::connected(const lt::tcp::endpoint& endpoint, const void* owner) {
    std::scoped_lock lock(mutex_);
    peers_.insert_or_assign(endpoint, Entry{owner, {}, {}});
}

void PeerIdentityStore::disconnected(const lt::tcp::endpoint& endpoint, const void* owner) {
    std::scoped_lock lock(mutex_);
    const auto found = peers_.find(endpoint);
    if (found != peers_.end() && found->second.owner == owner) peers_.erase(found);
}

void PeerIdentityStore::reported(const lt::tcp::endpoint& endpoint, const void* owner,
    const lt::peer_id& pid, std::string user_agent) {
    std::scoped_lock lock(mutex_);
    const auto found = peers_.find(endpoint);
    if (found == peers_.end() || found->second.owner != owner) return;
    found->second.pid = pid;
    found->second.user_agent = std::move(user_agent);
}

std::string PeerIdentityStore::user_agent(const lt::tcp::endpoint& endpoint,
    const lt::peer_id& pid) const {
    std::scoped_lock lock(mutex_);
    const auto found = peers_.find(endpoint);
    if (found == peers_.end() || found->second.pid != pid) return {};
    return found->second.user_agent;
}

std::shared_ptr<PeerIdentityStore> track_peer_identities(lt::add_torrent_params& params) {
    auto store = std::make_shared<PeerIdentityStore>();
    params.extensions.push_back([store](const lt::torrent_handle&, lt::client_data_t) {
        return std::make_shared<TorrentIdentityPlugin>(store);
    });
    return store;
}

std::string identify_peer_client(const lt::peer_id& pid) {
    const std::string_view bytes(pid.data(), pid.size());
    // aria2 uses A2-MAJOR-MINOR-PATCH-, independently of its HTTP UA.
    if (bytes.starts_with("A2-") || bytes.starts_with("M")) {
        const bool aria2 = bytes.starts_with("A2-");
        auto remaining = bytes.substr(aria2 ? 3 : 1);
        std::string version;
        for (int index = 0; index < 3; ++index) {
            const auto separator = remaining.find('-');
            if (separator == std::string_view::npos || separator == 0) return "unknown";
            const auto token = remaining.substr(0, separator);
            unsigned int number = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return "unknown";
            if (index != 0) version += '.';
            version += std::to_string(number);
            remaining.remove_prefix(separator + 1);
        }
        return std::string(aria2 ? "aria2 " : "Mainline ") + version;
    }
    if (bytes[0] != '-' || bytes[7] != '-') return "unknown";
    const auto code = bytes.substr(1, 2);
    // Codes follow BEP 20 and the clients' published conventions. Names do not
    // assert the peer's real identity.
    constexpr std::pair<std::string_view, std::string_view> clients[] = {
        {"AZ", "Azureus"}, {"BC", "BitComet"}, {"BT", "BitTorrent"},
        {"BI", "BiglyBT"}, {"BP", "BitTorrent Pro"},
        {"DE", "Deluge"}, {"LT", "libtorrent"}, {"lt", "libTorrent"},
        {"qB", "qBittorrent"}, {"QD", "QQDownload"}, {"TR", "Transmission"},
        {"UT", "uTorrent"}, {"UM", "uTorrent Mac"}, {"UW", "uTorrent Web"},
        {"WD", "WebTorrent Desktop"}, {"WW", "WebTorrent"},
        {"XL", "Xunlei"}, {"XF", "Xfplay"}, {"FG", "FlashGet"},
    };
    std::string name;
    for (const auto& [id, label] : clients) {
        if (code == id) { name = label; break; }
    }
    if (name.empty()) return "unknown";
    auto digit = [](char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'A' && value <= 'Z') return value - 'A' + 10;
        return -1;
    };
    if (code == "BC") {
        // BitComet's four decimal digits encode two two-digit components.
        for (int index = 3; index < 7; ++index) {
            if (bytes[index] < '0' || bytes[index] > '9') return name;
        }
        const auto major = (bytes[3] - '0') * 10 + bytes[4] - '0';
        return name + ' ' + std::to_string(major) + '.' + std::string(bytes.substr(5, 2));
    }
    if (code != "qB" && code != "LT" && code != "TR" && code != "DE" && code != "UT") {
        return name;
    }
    std::string version;
    for (int index = 3; index < 6; ++index) {
        const auto value = digit(bytes[index]);
        if (value < 0) return name;
        if (index != 3) version += '.';
        version += std::to_string(value);
    }
    // Transmission used two-component releases before 4.x (e.g. 2.94).
    if (code == "TR" && bytes[3] >= '0' && bytes[3] < '4'
        && bytes[4] >= '0' && bytes[4] <= '9'
        && bytes[5] >= '0' && bytes[5] <= '9') {
        version = std::string(bytes.substr(3, 1)) + '.' + std::string(bytes.substr(4, 2));
    }
    const auto tag = bytes[6];
    if (code == "DE") {
        if (tag == 'D') version += "dev";
        else if (tag == 'a' || tag == 'b' || tag == 'r') version += tag;
        else if (tag != 's' && tag != '0') return name;
    } else if (code == "qB" || code == "LT") {
        const auto build = digit(tag);
        if (build < 0) return name;
        if (build != 0) version += '.' + std::to_string(build);
    } else if (tag != '0') {
        if (tag < ' ' || tag > '~') return name;
        version += " (tag ";
        version += tag;
        version += ')';
    }
    return name + ' ' + version;
}

} // namespace bt
