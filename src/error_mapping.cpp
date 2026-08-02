#include "error_mapping.hpp"

#include <initializer_list>
#include <string>
#include <utility>

#include <boost/system/errc.hpp>

namespace bt {
namespace lt = libtorrent;
namespace {

bool matches(const lt::error_code& error, boost::system::errc::errc_t condition) {
    return error.default_error_condition() == boost::system::errc::make_error_condition(condition);
}

bool matches_any(const lt::error_code& error,
    std::initializer_list<boost::system::errc::errc_t> conditions) {
    for (const auto condition : conditions) {
        if (matches(error, condition)) return true;
    }
    return false;
}

bool is_invalid_source_error(const lt::error_code& error) {
    if (error.category() != lt::libtorrent_category()) return false;
    switch (static_cast<lt::errors::error_code_enum>(error.value())) {
    case lt::errors::torrent_is_no_dict:
    case lt::errors::torrent_missing_info:
    case lt::errors::torrent_info_no_dict:
    case lt::errors::torrent_missing_piece_length:
    case lt::errors::torrent_missing_name:
    case lt::errors::torrent_invalid_name:
    case lt::errors::torrent_invalid_length:
    case lt::errors::torrent_file_parse_failed:
    case lt::errors::torrent_missing_pieces:
    case lt::errors::torrent_invalid_hashes:
    case lt::errors::too_many_pieces_in_torrent:
    case lt::errors::invalid_swarm_metadata:
    case lt::errors::invalid_bencoding:
    case lt::errors::no_files_in_torrent:
    case lt::errors::parse_failed:
    case lt::errors::metadata_too_large:
    case lt::errors::invalid_metadata_size:
    case lt::errors::invalid_metadata_offset:
    case lt::errors::invalid_metadata_message:
    case lt::errors::torrent_unknown_version:
    case lt::errors::torrent_missing_file_tree:
    case lt::errors::torrent_missing_meta_version:
    case lt::errors::torrent_inconsistent_files:
    case lt::errors::torrent_missing_piece_layer:
    case lt::errors::torrent_missing_pieces_root:
    case lt::errors::torrent_invalid_pad_file:
        return true;
    default:
        return false;
    }
}

bool is_verification_error(const lt::error_code& error) {
    if (error.category() != lt::libtorrent_category()) return false;
    switch (static_cast<lt::errors::error_code_enum>(error.value())) {
    case lt::errors::failed_hash_check:
    case lt::errors::file_too_short:
    case lt::errors::torrent_invalid_piece_layer:
    case lt::errors::torrent_inconsistent_hashes:
        return true;
    default:
        return false;
    }
}

bool is_network_error(const lt::error_code& error) {
    if (error.category() != lt::libtorrent_category()) return false;
    switch (static_cast<lt::errors::error_code_enum>(error.value())) {
    case lt::errors::timed_out:
    case lt::errors::timed_out_no_interest:
    case lt::errors::timed_out_inactivity:
    case lt::errors::timed_out_no_handshake:
    case lt::errors::timed_out_no_request:
    case lt::errors::http_error:
    case lt::errors::no_router:
    case lt::errors::tracker_failure:
        return true;
    default:
        return false;
    }
}

TaskError mapped(std::string code, const lt::error_code& error, bool retryable) {
    return {std::move(code), error.message(), retryable};
}

} // namespace

TaskError map_libtorrent_error(const lt::error_code& error, LibtorrentErrorContext context) {
    if (matches(error, boost::system::errc::no_space_on_device)) {
        return mapped("DISK_FULL", error, true);
    }
    if (matches_any(error, {
            boost::system::errc::permission_denied,
            boost::system::errc::operation_not_permitted,
            boost::system::errc::read_only_file_system,
        })) {
        return mapped("SAVE_PATH_NOT_WRITABLE", error, true);
    }
    if (matches_any(error, {
            boost::system::errc::no_such_file_or_directory,
            boost::system::errc::not_a_directory,
            boost::system::errc::no_such_device,
            boost::system::errc::no_such_device_or_address,
            boost::system::errc::device_or_resource_busy,
        })) {
        return mapped("SAVE_PATH_UNAVAILABLE", error, true);
    }
    if (is_network_error(error) || matches_any(error, {
            boost::system::errc::network_down,
            boost::system::errc::network_reset,
            boost::system::errc::network_unreachable,
            boost::system::errc::host_unreachable,
            boost::system::errc::connection_aborted,
            boost::system::errc::connection_refused,
            boost::system::errc::connection_reset,
            boost::system::errc::not_connected,
            boost::system::errc::timed_out,
        })) {
        return mapped("NETWORK_UNAVAILABLE", error, true);
    }
    if (error == lt::errors::make_error_code(lt::errors::duplicate_torrent)
        || error == lt::errors::make_error_code(lt::errors::file_collision)) {
        return mapped("DUPLICATE_TASK", error, false);
    }
    if (error == lt::errors::make_error_code(lt::errors::unsupported_url_protocol)) {
        return mapped("SOURCE_UNSUPPORTED", error, false);
    }
    if (error == lt::errors::make_error_code(lt::errors::invalid_save_path)) {
        return mapped("SAVE_PATH_INVALID", error, false);
    }
    if (is_verification_error(error)) {
        return mapped("DATA_VERIFICATION_FAILED", error, true);
    }
    if (is_invalid_source_error(error)) {
        return mapped("SOURCE_INVALID", error, false);
    }
    if (error == lt::errors::make_error_code(lt::errors::no_memory)
        || matches(error, boost::system::errc::not_enough_memory)) {
        return mapped("INTERNAL_ERROR", error, true);
    }
    if (context == LibtorrentErrorContext::storage
        || matches(error, boost::system::errc::io_error)) {
        return mapped("STORAGE_ERROR", error, true);
    }
    if (context == LibtorrentErrorContext::metadata) {
        return mapped("SOURCE_INVALID", error, false);
    }
    return mapped("TORRENT_ERROR", error, true);
}

} // namespace bt
