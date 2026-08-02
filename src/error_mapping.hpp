#pragma once

#include "bt_download/task.hpp"

#include <libtorrent/error_code.hpp>

namespace bt {

enum class LibtorrentErrorContext {
    torrent,
    storage,
    metadata,
};

TaskError map_libtorrent_error(
    const libtorrent::error_code& error,
    LibtorrentErrorContext context);

} // namespace bt
