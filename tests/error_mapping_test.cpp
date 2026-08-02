#include "error_mapping.hpp"

#include <stdexcept>

#include <boost/system/errc.hpp>
#include <libtorrent/error_code.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_mapping(const libtorrent::error_code& error,
    bt::LibtorrentErrorContext context,
    const char* code,
    bool retryable) {
    const auto mapped = bt::map_libtorrent_error(error, context);
    expect(mapped.code == code, "libtorrent business error code mismatch");
    expect(mapped.retryable == retryable, "libtorrent retryable flag mismatch");
    expect(!mapped.message.empty(), "libtorrent error message must be retained");
}

} // namespace

void run_error_mapping_tests() {
    namespace errc = boost::system::errc;
    namespace errors = libtorrent::errors;

    expect_mapping(errc::make_error_code(errc::no_space_on_device),
        bt::LibtorrentErrorContext::storage, "DISK_FULL", true);
    expect_mapping(errc::make_error_code(errc::permission_denied),
        bt::LibtorrentErrorContext::storage, "SAVE_PATH_NOT_WRITABLE", true);
    expect_mapping(errc::make_error_code(errc::no_such_device),
        bt::LibtorrentErrorContext::storage, "SAVE_PATH_UNAVAILABLE", true);
    expect_mapping(errc::make_error_code(errc::network_unreachable),
        bt::LibtorrentErrorContext::torrent, "NETWORK_UNAVAILABLE", true);
    expect_mapping(errors::make_error_code(errors::timed_out),
        bt::LibtorrentErrorContext::torrent, "NETWORK_UNAVAILABLE", true);
    expect_mapping(errors::make_error_code(errors::failed_hash_check),
        bt::LibtorrentErrorContext::torrent, "DATA_VERIFICATION_FAILED", true);
    expect_mapping(errors::make_error_code(errors::invalid_swarm_metadata),
        bt::LibtorrentErrorContext::metadata, "SOURCE_INVALID", false);
    expect_mapping(errors::make_error_code(errors::unsupported_url_protocol),
        bt::LibtorrentErrorContext::torrent, "SOURCE_UNSUPPORTED", false);
    expect_mapping(errors::make_error_code(errors::duplicate_torrent),
        bt::LibtorrentErrorContext::torrent, "DUPLICATE_TASK", false);
    expect_mapping(errc::make_error_code(errc::io_error),
        bt::LibtorrentErrorContext::storage, "STORAGE_ERROR", true);
    expect_mapping(errors::make_error_code(errors::no_memory),
        bt::LibtorrentErrorContext::torrent, "INTERNAL_ERROR", true);

#ifdef _WIN32
    expect_mapping(libtorrent::error_code(ERROR_DISK_FULL, boost::system::system_category()),
        bt::LibtorrentErrorContext::storage, "DISK_FULL", true);
    expect_mapping(libtorrent::error_code(ERROR_ACCESS_DENIED, boost::system::system_category()),
        bt::LibtorrentErrorContext::storage, "SAVE_PATH_NOT_WRITABLE", true);
#endif
}
