#pragma once

#include <string>
#include <system_error>

#include <boost/system/error_code.hpp>

namespace bt {

// Windows system-category messages use the active ANSI code page by default.
// Read those messages as UTF-16 and convert them before exposing them as JSON.
std::string error_message_utf8(const std::error_code& error);
std::string error_message_utf8(const boost::system::error_code& error);

} // namespace bt
