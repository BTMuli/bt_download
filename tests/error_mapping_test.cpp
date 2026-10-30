#include "error_mapping.hpp"
#include "error_message.hpp"

#include <stdexcept>
#include <string>
#include <system_error>

#include <boost/system/errc.hpp>
#include <libtorrent/error_code.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// UTF-8 for a Chinese error, independent of the compiler's execution charset.
const std::string utf8_error = "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xa2\xab\xe6\x8b\x92\xe7\xbb\x9d";

class Utf8BoostCategory final : public boost::system::error_category {
public:
    const char* name() const noexcept override { return "utf8-test"; }
    std::string message(int) const override { return utf8_error; }
};

class Utf8StdCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "utf8-test"; }
    std::string message(int) const override { return utf8_error; }
};

#ifdef _WIN32
void expect_windows_message(int code, const std::string& message) {
    // Reverse the UTF-8 result and compare to the OS's original Unicode text.
    wchar_t expected[2048]{};
    const auto expected_length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, expected, 2048, nullptr);
    expect(expected_length != 0, "Windows error fixture is unavailable");
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        message.data(), static_cast<int>(message.size()), nullptr, 0);
    expect(length > 0, "Windows error message is not valid UTF-8");
    std::wstring decoded(static_cast<std::size_t>(length), L'\0');
    expect(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
               static_cast<int>(message.size()), decoded.data(), length) == length,
        "Windows error message could not be decoded");
    const std::wstring original(expected, expected_length);
    expect(original.starts_with(decoded)
            && original.find_first_not_of(L"\r\n \t", decoded.size()) == std::wstring::npos,
        "Windows error message lost Unicode content");
    expect(message.find("\xef\xbf\xbd") == std::string::npos,
        "Windows error message contains replacement characters");
    const auto encoded = nlohmann::json({{"lastError", message}}).dump();
    expect(nlohmann::json::parse(encoded).at("lastError") == message,
        "Windows error message changed during JSON serialization");
}
#endif

void expect_mapping(const libtorrent::error_code& error,
    bt::LibtorrentErrorContext context,
    const char* code,
    bool retryable) {
    const auto mapped = bt::map_libtorrent_error(error, context);
    expect(mapped.code == code, "libtorrent business error code mismatch");
    expect(mapped.retryable == retryable, "libtorrent retryable flag mismatch");
    expect(!mapped.message.empty(), "libtorrent error message must be retained");
    expect(nlohmann::json::parse(nlohmann::json(mapped.message).dump()) == mapped.message,
        "mapped error message must survive strict UTF-8 JSON serialization");
}

} // namespace

void run_error_mapping_tests() {
    namespace errc = boost::system::errc;
    namespace errors = libtorrent::errors;

    const Utf8BoostCategory boost_category;
    const Utf8StdCategory std_category;
    expect(bt::error_message_utf8(boost::system::error_code(10060, boost_category)) == utf8_error,
        "custom Boost category's UTF-8 message must remain unchanged");
    expect(bt::error_message_utf8(std::error_code(10060, std_category)) == utf8_error,
        "custom std category's UTF-8 message must remain unchanged");
    const auto generic = errc::make_error_code(errc::permission_denied);
    expect(bt::error_message_utf8(generic) == generic.message(),
        "generic error condition must not be interpreted as a Windows error number");
    const auto torrent = errors::make_error_code(errors::timed_out);
    expect(bt::error_message_utf8(torrent) == torrent.message(),
        "libtorrent category's message must remain unchanged");

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
    const int system_codes[] = {ERROR_ACCESS_DENIED, ERROR_DISK_FULL, 10060, 10061};
    for (const auto code : system_codes) {
        expect_windows_message(code,
            bt::error_message_utf8(boost::system::error_code(code, boost::system::system_category())));
        expect_windows_message(code,
            bt::error_message_utf8(std::error_code(code, std::system_category())));
    }
    expect_mapping(libtorrent::error_code(10060, boost::system::system_category()),
        bt::LibtorrentErrorContext::torrent, "NETWORK_UNAVAILABLE", true);
    expect(bt::error_message_utf8(std::error_code(0x20000000, std::system_category()))
            == "system error 536870912",
        "unknown Windows errors must retain the numeric code without ANSI text");
    expect_mapping(libtorrent::error_code(ERROR_DISK_FULL, boost::system::system_category()),
        bt::LibtorrentErrorContext::storage, "DISK_FULL", true);
    expect_mapping(libtorrent::error_code(ERROR_ACCESS_DENIED, boost::system::system_category()),
        bt::LibtorrentErrorContext::storage, "SAVE_PATH_NOT_WRITABLE", true);
#endif
}
