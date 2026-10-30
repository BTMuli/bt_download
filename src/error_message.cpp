#include "error_message.hpp"

#ifdef _WIN32
#include <memory>

#include <windows.h>
#endif

namespace bt {
namespace {

#ifdef _WIN32
std::string windows_error_message(int code) {
    const auto fallback = "system error " + std::to_string(code);
    wchar_t* raw = nullptr;
    auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    const std::unique_ptr<wchar_t, decltype(&LocalFree)> buffer(raw, &LocalFree);
    if (length == 0) return fallback;

    while (length != 0 && (raw[length - 1] == L'\r' || raw[length - 1] == L'\n'
            || raw[length - 1] == L' ' || raw[length - 1] == L'\t')) {
        --length;
    }
    if (length == 0) return fallback;

    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        raw, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes == 0) return fallback;
    std::string message(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, raw,
            static_cast<int>(length), message.data(), bytes, nullptr, nullptr) != bytes) {
        return fallback;
    }
    return message;
}
#endif

} // namespace

std::string error_message_utf8(const std::error_code& error) {
#ifdef _WIN32
    if (error.category() == std::system_category()) {
        return windows_error_message(error.value());
    }
#endif
    return error.message();
}

std::string error_message_utf8(const boost::system::error_code& error) {
#ifdef _WIN32
    if (error.category() == boost::system::system_category()) {
        return windows_error_message(error.value());
    }
#endif
    return error.message();
}

} // namespace bt
