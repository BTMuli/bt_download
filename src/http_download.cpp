#include "bt_download/http_download.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bt {
namespace {

class CurlRuntime {
public:
    CurlRuntime() : result_(curl_global_init(CURL_GLOBAL_DEFAULT)) {}
    ~CurlRuntime() {
        if (result_ == CURLE_OK) curl_global_cleanup();
    }

    CURLcode result() const noexcept { return result_; }

private:
    CURLcode result_;
};

CurlRuntime& curl_runtime() {
    static CurlRuntime runtime;
    return runtime;
}

std::string lowercase_ascii(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::string uppercase_ascii(std::string value) {
    for (auto& character : value) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return value;
}

std::optional<std::string> url_part(CURLU* handle, CURLUPart part, unsigned int flags = 0) {
    char* raw = nullptr;
    if (curl_url_get(handle, part, &raw, flags) != CURLUE_OK || raw == nullptr) {
        return std::nullopt;
    }
    std::string value(raw);
    curl_free(raw);
    return value;
}

bool wildcard_match(std::string_view pattern, std::string_view value) {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t wildcard_index = std::string_view::npos;
    std::size_t wildcard_value_index = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size()
            && pattern[pattern_index] == value[value_index]) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            wildcard_index = pattern_index++;
            wildcard_value_index = value_index;
        } else if (wildcard_index != std::string_view::npos) {
            pattern_index = wildcard_index + 1;
            value_index = ++wildcard_value_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') ++pattern_index;
    return pattern_index == pattern.size();
}

bool proxy_bypassed(std::string host, const std::vector<std::string>& rules) {
    host = lowercase_ascii(std::move(host));
    while (!host.empty() && host.back() == '.') host.pop_back();
    for (auto rule : rules) {
        rule = lowercase_ascii(std::move(rule));
        if (rule == "<local>" && host.find('.') == std::string::npos) return true;
        if (rule == "*" || wildcard_match(rule, host)) return true;
    }
    return false;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = hex_value(value[index + 1]);
            const auto low = hex_value(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    return result;
}

std::size_t utf8_sequence_length(std::string_view value, std::size_t index) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7fU) return 1;
    const auto continuation = [&](std::size_t offset) {
        return index + offset < value.size()
            && (static_cast<unsigned char>(value[index + offset]) & 0xc0U) == 0x80U;
    };
    if (first >= 0xc2U && first <= 0xdfU && continuation(1)) return 2;
    if (first >= 0xe0U && first <= 0xefU && continuation(1) && continuation(2)) {
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if ((first != 0xe0U || second >= 0xa0U)
            && (first != 0xedU || second <= 0x9fU)) {
            return 3;
        }
    }
    if (first >= 0xf0U && first <= 0xf4U && continuation(1)
        && continuation(2) && continuation(3)) {
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if ((first != 0xf0U || second >= 0x90U)
            && (first != 0xf4U || second <= 0x8fU)) {
            return 4;
        }
    }
    return 0;
}

std::string replace_invalid_utf8(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto length = utf8_sequence_length(value, index);
        if (length == 0) {
            result.push_back('_');
            ++index;
        } else {
            result.append(value.substr(index, length));
            index += length;
        }
    }
    return result;
}

std::size_t utf8_prefix_size(std::string_view value, std::size_t byte_limit) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto length = utf8_sequence_length(value, index);
        if (length == 0 || index + length > byte_limit) break;
        index += length;
    }
    return index;
}

bool is_reserved_windows_name(const std::string& value) {
    const auto dot = value.find('.');
    const auto base = uppercase_ascii(value.substr(0, dot));
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return true;
    if (base.size() != 4) return false;
    const auto prefix = base.substr(0, 3);
    return (prefix == "COM" || prefix == "LPT") && base[3] >= '1' && base[3] <= '9';
}

std::string safe_http_file_name(std::string value) {
    value = replace_invalid_utf8(value);
    for (auto& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || character == '<' || character == '>' || character == ':'
            || character == '"' || character == '/' || character == '\\'
            || character == '|' || character == '?' || character == '*') {
            character = '_';
        }
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    if (value.empty() || value == "." || value == "..") value = "download";
    if (is_reserved_windows_name(value)) value.insert(value.begin(), '_');

    constexpr std::size_t max_name_bytes = 180;
    if (value.size() > max_name_bytes) {
        const auto dot = value.find_last_of('.');
        const bool preserve_extension = dot != std::string::npos && dot != 0
            && value.size() - dot <= 32;
        if (preserve_extension) {
            const auto extension = value.substr(dot);
            const auto prefix_size = utf8_prefix_size(
                std::string_view(value).substr(0, dot), max_name_bytes - extension.size());
            value = value.substr(0, prefix_size) + extension;
        } else {
            value.resize(utf8_prefix_size(value, max_name_bytes));
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    }
    return value.empty() ? "download" : value;
}

std::FILE* open_partial_file(const std::filesystem::path& path, bool append) {
#ifdef _WIN32
    return _wfopen(path.c_str(), append ? L"ab" : L"wb");
#else
    return std::fopen(path.c_str(), append ? "ab" : "wb");
#endif
}

std::FILE* open_range_file(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"r+b");
#else
    return std::fopen(path.c_str(), "r+b");
#endif
}

bool seek_file(std::FILE* file, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
#ifdef _WIN32
    return _fseeki64(file, static_cast<std::int64_t>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

std::filesystem::path transfer_state_path(const std::filesystem::path& partial_path) {
    auto path = partial_path;
    path += ".ranges.json";
    return path;
}

bool replace_file(const std::filesystem::path& temporary,
    const std::filesystem::path& target) {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return !error;
#endif
}

struct StoredTransferState {
    HttpTransferProgress progress;
    std::string etag;
    std::string last_modified;
};

std::optional<StoredTransferState> load_transfer_state(
    const std::filesystem::path& partial_path) {
    try {
        const auto state_path = transfer_state_path(partial_path);
        if (!std::filesystem::is_regular_file(state_path)
            || !std::filesystem::is_regular_file(partial_path)) {
            return std::nullopt;
        }
        std::ifstream input(state_path, std::ios::binary);
        const auto json = nlohmann::json::parse(input);
        if (json.at("version") != 1) return std::nullopt;

        StoredTransferState state;
        state.progress.total_bytes = json.at("totalBytes").get<std::uint64_t>();
        state.etag = json.value("etag", std::string{});
        state.last_modified = json.value("lastModified", std::string{});
        if (state.progress.total_bytes == 0) return std::nullopt;

        std::uint64_t expected_start = 0;
        for (const auto& item : json.at("ranges")) {
            HttpByteRangeProgress range{
                item.at("start").get<std::uint64_t>(),
                item.at("end").get<std::uint64_t>(),
                item.at("downloaded").get<std::uint64_t>(),
            };
            if (range.start != expected_start || range.end <= range.start
                || range.end > state.progress.total_bytes
                || range.downloaded > range.end - range.start) {
                return std::nullopt;
            }
            expected_start = range.end;
            state.progress.downloaded_bytes += range.downloaded;
            state.progress.ranges.push_back(range);
        }
        if (state.progress.ranges.empty()
            || expected_start != state.progress.total_bytes) {
            return std::nullopt;
        }
        std::error_code error;
        if (std::filesystem::file_size(partial_path, error)
                != state.progress.total_bytes || error) {
            return std::nullopt;
        }
        return state;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string_view trim_header_value(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'
            || value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool header_name_is(std::string_view line, std::string_view name) {
    if (line.size() < name.size()) return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        auto character = line[index];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        if (character != name[index]) return false;
    }
    return true;
}

struct ContentRange {
    std::uint64_t start{0};
    std::uint64_t end{0};
    std::uint64_t total{0};
};

std::optional<ContentRange> parse_content_range(std::string_view value) {
    value = trim_header_value(value);
    constexpr std::string_view prefix = "bytes ";
    if (!value.starts_with(prefix)) return std::nullopt;
    value.remove_prefix(prefix.size());
    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos
        || dash >= slash) {
        return std::nullopt;
    }
    ContentRange result;
    const auto parse = [](std::string_view part, std::uint64_t& output) {
        const auto parsed = std::from_chars(
            part.data(), part.data() + part.size(), output);
        return parsed.ec == std::errc{}
            && parsed.ptr == part.data() + part.size();
    };
    if (!parse(value.substr(0, dash), result.start)
        || !parse(value.substr(dash + 1, slash - dash - 1), result.end)
        || !parse(value.substr(slash + 1), result.total)
        || result.end < result.start || result.end >= result.total) {
        return std::nullopt;
    }
    return result;
}

HttpTransferFailure curl_failure(CURLcode code, long status_code,
    const std::string& detail, bool resumed) {
    if (code == CURLE_HTTP_RETURNED_ERROR) {
        if (resumed && status_code == 416) {
            return {"HTTP_RESUME_UNSUPPORTED", "HTTP server rejected the byte range", true, true};
        }
        const bool retryable = status_code == 408 || status_code == 425 || status_code == 429
            || status_code >= 500;
        return {"HTTP_STATUS_ERROR",
            "HTTP request failed with status " + std::to_string(status_code), retryable, false};
    }
    switch (code) {
    case CURLE_UNSUPPORTED_PROTOCOL:
        return {"HTTP_REDIRECT_ERROR", "HTTP redirect uses an unsupported protocol", false, false};
    case CURLE_URL_MALFORMAT:
        return {"SOURCE_INVALID", "invalid HTTP(S) URL", false, false};
    case CURLE_TOO_MANY_REDIRECTS:
        return {"HTTP_REDIRECT_ERROR", "too many HTTP redirects", false, false};
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
        return {"NETWORK_UNAVAILABLE", detail, true, false};
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
        return {"TLS_ERROR", detail, false, false};
    case CURLE_RANGE_ERROR:
        return {"HTTP_RESUME_UNSUPPORTED", "HTTP server does not support byte ranges", true, resumed};
    case CURLE_WRITE_ERROR:
        return {"STORAGE_ERROR", detail, true, false};
    default:
        return {"HTTP_TRANSFER_ERROR", detail, true, false};
    }
}

} // namespace

struct HttpDownloadManager::Impl {
    enum class RequestKind { probe, segment, sequential };
    enum class TransferMode { probe, segmented, sequential };

    struct Transfer;

    struct Request {
        Transfer* transfer{nullptr};
        RequestKind kind{RequestKind::probe};
        CURL* easy{nullptr};
        curl_slist* headers{nullptr};
        std::FILE* output{nullptr};
        std::uint64_t start{0};
        std::uint64_t end{0};
        std::uint64_t initial_downloaded{0};
        std::uint64_t downloaded{0};
        std::uint64_t body_written{0};
        std::uint64_t download_rate{0};
        long response_status{0};
        std::optional<ContentRange> content_range;
        std::string etag;
        std::string last_modified;
        std::string range_option;
        bool disk_checked{false};
        std::optional<HttpTransferFailure> callback_failure;
        std::array<char, CURL_ERROR_SIZE> error_buffer{};
    };

    struct Transfer {
        std::string id;
        std::string url;
        std::string user_agent;
        EngineProxyConfig proxy;
        std::filesystem::path partial_path;
        TransferMode mode{TransferMode::probe};
        std::size_t max_connections{1};
        std::uint64_t total_bytes{0};
        std::string etag;
        std::string last_modified;
        std::vector<std::unique_ptr<Request>> requests;
        bool probe_ready{false};
        bool fallback_requested{false};
        bool fallback_used{false};
        std::optional<HttpTransferFailure> failure;
        std::chrono::steady_clock::time_point next_persist{};
    };

    CURLM* multi{nullptr};
    std::unordered_map<std::string, std::unique_ptr<Transfer>> transfers;

    static std::size_t write_callback(char* data, std::size_t size,
        std::size_t count, void* context) {
        auto& request = *static_cast<Request*>(context);
        if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
            request.callback_failure = HttpTransferFailure{
                "STORAGE_ERROR", "HTTP response chunk is too large", false, false};
            return 0;
        }
        const auto bytes = size * count;
        if (request.response_status >= 300 && request.response_status < 400) {
            return bytes;
        }
        if (request.kind == RequestKind::probe) {
            if (request.response_status == 206) return bytes;
            request.callback_failure = HttpTransferFailure{
                "HTTP_RESUME_UNSUPPORTED", "HTTP server ignored the range probe", true, true};
            return 0;
        }
        if ((request.kind == RequestKind::segment || request.start > 0)
            && request.response_status != 206) {
            request.callback_failure = HttpTransferFailure{
                "HTTP_RESUME_UNSUPPORTED", "HTTP server ignored the byte range", true, true};
            return 0;
        }
        if (request.kind == RequestKind::segment) {
            const auto remaining = request.end - request.start - request.downloaded;
            if (bytes > remaining) {
                request.callback_failure = HttpTransferFailure{
                    "HTTP_RESUME_UNSUPPORTED", "HTTP range exceeded its declared boundary", true, true};
                return 0;
            }
        }
        const auto written = std::fwrite(data, 1, bytes, request.output);
        request.body_written += written;
        request.downloaded = request.initial_downloaded + request.body_written;
        return written;
    }

    static std::size_t header_callback(char* data, std::size_t size,
        std::size_t count, void* context) {
        auto& request = *static_cast<Request*>(context);
        const auto bytes = size * count;
        const std::string_view line(data, bytes);
        if (line.starts_with("HTTP/")) {
            request.content_range.reset();
            request.etag.clear();
            request.last_modified.clear();
            const auto first_space = line.find(' ');
            if (first_space != std::string_view::npos) {
                long status = 0;
                const auto begin = line.data() + first_space + 1;
                const auto end = line.data() + line.size();
                const auto parsed = std::from_chars(begin, end, status);
                if (parsed.ec == std::errc{}) request.response_status = status;
            }
        } else if (header_name_is(line, "content-range:")) {
            request.content_range = parse_content_range(line.substr(14));
        } else if (header_name_is(line, "etag:")) {
            request.etag = std::string(trim_header_value(line.substr(5)));
        } else if (header_name_is(line, "last-modified:")) {
            request.last_modified = std::string(trim_header_value(line.substr(14)));
        }
        return bytes;
    }

    static int progress_callback(void* context, curl_off_t download_total,
        curl_off_t downloaded, curl_off_t, curl_off_t) {
        auto& request = *static_cast<Request*>(context);
        if (downloaded >= 0 && request.kind != RequestKind::segment) {
            request.downloaded = (std::max)(request.downloaded,
                request.initial_downloaded + static_cast<std::uint64_t>(downloaded));
        }
        if (request.kind != RequestKind::sequential || download_total <= 0) return 0;

        auto& transfer = *request.transfer;
        transfer.total_bytes = request.start
            + static_cast<std::uint64_t>(download_total);
        if (request.disk_checked) return 0;
        request.disk_checked = true;
        std::error_code error;
        const auto space = std::filesystem::space(
            transfer.partial_path.parent_path(), error);
        if (error) {
            request.callback_failure = HttpTransferFailure{
                "SAVE_PATH_UNAVAILABLE",
                "cannot query savePath free space: " + error.message(), true, false};
            return 1;
        }
        const auto remaining = transfer.total_bytes > request.start
            ? transfer.total_bytes - request.start : 0;
        if (remaining > space.available) {
            request.callback_failure = HttpTransferFailure{
                "DISK_FULL", "not enough free space for HTTP payload", true, false};
            return 1;
        }
        return 0;
    }

    bool configure_request(Request& request) {
        auto& transfer = *request.transfer;
        request.easy = curl_easy_init();
        if (request.easy == nullptr) return false;
        bool proxy_configured = true;
        if (transfer.proxy.enabled) {
            const auto proxy = select_http_proxy(transfer.url, transfer.proxy);
            proxy_configured = curl_easy_setopt(request.easy, CURLOPT_PROXY,
                                   proxy ? proxy->c_str() : "") == CURLE_OK
                && curl_easy_setopt(request.easy, CURLOPT_NOPROXY, "") == CURLE_OK;
        } else {
            proxy_configured = curl_easy_setopt(request.easy, CURLOPT_NOPROXY,
                                   "localhost,127.0.0.1,[::1]") == CURLE_OK;
        }
        const bool common = proxy_configured
            &&
            curl_easy_setopt(request.easy, CURLOPT_URL, transfer.url.c_str()) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_USERAGENT, transfer.user_agent.c_str()) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_PROTOCOLS_STR, "http,https") == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https") == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_MAXREDIRS, 10L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_FAILONERROR, 1L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_CONNECTTIMEOUT, 30L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_LOW_SPEED_LIMIT, 1L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_LOW_SPEED_TIME, 60L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_ACCEPT_ENCODING, "identity") == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_WRITEFUNCTION, write_callback) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_WRITEDATA, &request) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_HEADERFUNCTION, header_callback) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_HEADERDATA, &request) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_XFERINFOFUNCTION, progress_callback) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_XFERINFODATA, &request) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_NOPROGRESS, 0L) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_ERRORBUFFER,
                request.error_buffer.data()) == CURLE_OK
            && curl_easy_setopt(request.easy, CURLOPT_PRIVATE, &request) == CURLE_OK;
        if (!common) return false;

        if (request.kind == RequestKind::probe) {
            request.range_option = "0-0";
            if (curl_easy_setopt(request.easy, CURLOPT_RANGE,
                    request.range_option.c_str()) != CURLE_OK) return false;
        } else if (request.kind == RequestKind::segment) {
            const auto request_start = request.start + request.initial_downloaded;
            request.range_option = std::to_string(request_start) + "-"
                + std::to_string(request.end - 1);
            if (curl_easy_setopt(request.easy, CURLOPT_RANGE,
                    request.range_option.c_str()) != CURLE_OK) return false;
            const auto validator = !transfer.etag.empty()
                    && !std::string_view(transfer.etag).starts_with("W/")
                ? transfer.etag : transfer.last_modified;
            if (!validator.empty()) {
                const auto header = "If-Range: " + validator;
                request.headers = curl_slist_append(request.headers, header.c_str());
                if (request.headers == nullptr
                    || curl_easy_setopt(request.easy, CURLOPT_HTTPHEADER,
                        request.headers) != CURLE_OK) return false;
            }
        } else if (request.start > 0
            && curl_easy_setopt(request.easy, CURLOPT_RESUME_FROM_LARGE,
                static_cast<curl_off_t>(request.start)) != CURLE_OK) {
            return false;
        }
        return curl_multi_add_handle(multi, request.easy) == CURLM_OK;
    }

    void destroy_request(Request& request) {
        if (request.easy != nullptr) {
            curl_multi_remove_handle(multi, request.easy);
            curl_easy_cleanup(request.easy);
            request.easy = nullptr;
        }
        if (request.headers != nullptr) {
            curl_slist_free_all(request.headers);
            request.headers = nullptr;
        }
        if (request.output != nullptr) {
            std::fclose(request.output);
            request.output = nullptr;
        }
    }

    void destroy_requests(Transfer& transfer) {
        for (auto& request : transfer.requests) destroy_request(*request);
        transfer.requests.clear();
    }

    std::optional<HttpTransferFailure> add_probe(Transfer& transfer) {
        transfer.mode = TransferMode::probe;
        auto request = std::make_unique<Request>();
        request->transfer = &transfer;
        request->kind = RequestKind::probe;
        if (!configure_request(*request)) {
            destroy_request(*request);
            return HttpTransferFailure{
                "INTERNAL_ERROR", "cannot configure HTTP range probe", true, false};
        }
        transfer.requests.push_back(std::move(request));
        return std::nullopt;
    }

    std::optional<HttpTransferFailure> add_sequential(Transfer& transfer,
        std::uint64_t offset) {
        destroy_requests(transfer);
        transfer.mode = TransferMode::sequential;
        transfer.total_bytes = 0;
        auto request = std::make_unique<Request>();
        request->transfer = &transfer;
        request->kind = RequestKind::sequential;
        request->start = offset;
        request->initial_downloaded = offset;
        request->downloaded = offset;
        request->output = open_partial_file(transfer.partial_path, offset > 0);
        if (request->output == nullptr || !configure_request(*request)) {
            destroy_request(*request);
            return HttpTransferFailure{
                "STORAGE_ERROR", "cannot start sequential HTTP transfer", true, false};
        }
        transfer.requests.push_back(std::move(request));
        return std::nullopt;
    }

    std::optional<HttpTransferFailure> persist_state(Transfer& transfer) {
        if (transfer.mode != TransferMode::segmented) return std::nullopt;
        nlohmann::json ranges = nlohmann::json::array();
        for (const auto& request : transfer.requests) {
            if (request->output != nullptr && std::fflush(request->output) != 0) {
                return HttpTransferFailure{
                    "STORAGE_ERROR", "cannot flush HTTP range data", true, false};
            }
            ranges.push_back({
                {"start", request->start}, {"end", request->end},
                {"downloaded", (std::min)(request->downloaded,
                    request->end - request->start)},
            });
        }
        const nlohmann::json state = {
            {"version", 1}, {"totalBytes", transfer.total_bytes},
            {"etag", transfer.etag}, {"lastModified", transfer.last_modified},
            {"ranges", std::move(ranges)},
        };
        const auto target = transfer_state_path(transfer.partial_path);
        auto temporary = target;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                return HttpTransferFailure{
                    "STORAGE_ERROR", "cannot write HTTP range state", true, false};
            }
            output << state.dump();
            output.flush();
            if (!output) {
                return HttpTransferFailure{
                    "STORAGE_ERROR", "cannot flush HTTP range state", true, false};
            }
        }
        if (!replace_file(temporary, target)) {
            return HttpTransferFailure{
                "STORAGE_ERROR", "cannot replace HTTP range state", true, false};
        }
        transfer.next_persist = std::chrono::steady_clock::now()
            + std::chrono::seconds(1);
        return std::nullopt;
    }

    std::optional<HttpTransferFailure> activate_pending_ranges(
        Transfer& transfer) {
        std::size_t active = 0;
        for (const auto& request : transfer.requests) {
            if (request->easy != nullptr) ++active;
        }
        for (auto& request : transfer.requests) {
            if (active >= transfer.max_connections) break;
            if (request->easy != nullptr
                || request->downloaded >= request->end - request->start) {
                continue;
            }
            request->output = open_range_file(transfer.partial_path);
            if (request->output == nullptr
                || !seek_file(request->output,
                    request->start + request->downloaded)
                || !configure_request(*request)) {
                destroy_request(*request);
                return HttpTransferFailure{
                    "STORAGE_ERROR", "cannot start HTTP range writer", true, false};
            }
            ++active;
        }
        return std::nullopt;
    }

    std::optional<HttpTransferFailure> start_segmented(Transfer& transfer,
        const std::vector<HttpByteRangeProgress>* restored = nullptr) {
        destroy_requests(transfer);
        transfer.mode = TransferMode::segmented;

        std::vector<HttpByteRangeProgress> ranges;
        if (restored != nullptr) {
            ranges = *restored;
        } else {
            constexpr std::uint64_t target_min_range = 1024U * 1024U;
            const auto useful_connections = std::max<std::uint64_t>(1,
                transfer.total_bytes / target_min_range
                    + (transfer.total_bytes % target_min_range == 0 ? 0 : 1));
            const auto count = std::min<std::uint64_t>(
                transfer.max_connections, useful_connections);
            const auto range_size = transfer.total_bytes / count
                + (transfer.total_bytes % count == 0 ? 0 : 1);
            for (std::uint64_t index = 0; index < count; ++index) {
                const auto start = index * range_size;
                if (start >= transfer.total_bytes) break;
                ranges.push_back({start,
                    (std::min)(transfer.total_bytes, start + range_size), 0});
            }

            std::FILE* output = open_partial_file(transfer.partial_path, false);
            if (output == nullptr) {
                return HttpTransferFailure{
                    "STORAGE_ERROR", "cannot create HTTP range file", true, false};
            }
            std::fclose(output);
            std::error_code error;
            const auto space = std::filesystem::space(
                transfer.partial_path.parent_path(), error);
            if (error) {
                return HttpTransferFailure{"SAVE_PATH_UNAVAILABLE",
                    "cannot query savePath free space: " + error.message(), true, false};
            }
            if (transfer.total_bytes > space.available) {
                return HttpTransferFailure{
                    "DISK_FULL", "not enough free space for HTTP payload", true, false};
            }
            std::filesystem::resize_file(
                transfer.partial_path, transfer.total_bytes, error);
            if (error) {
                return HttpTransferFailure{"STORAGE_ERROR",
                    "cannot allocate HTTP range file: " + error.message(), true, false};
            }
        }

        for (const auto& range : ranges) {
            auto request = std::make_unique<Request>();
            request->transfer = &transfer;
            request->kind = RequestKind::segment;
            request->start = range.start;
            request->end = range.end;
            request->initial_downloaded = range.downloaded;
            request->downloaded = range.downloaded;
            transfer.requests.push_back(std::move(request));
        }
        if (const auto failure = activate_pending_ranges(transfer)) {
            destroy_requests(transfer);
            return failure;
        }
        return persist_state(transfer);
    }

    HttpTransferProgress progress(const Transfer& transfer) const {
        HttpTransferProgress result;
        result.total_bytes = transfer.total_bytes;
        for (const auto& request : transfer.requests) {
            if (request->easy != nullptr) ++result.active_connections;
            if (transfer.mode == TransferMode::segmented) {
                const auto downloaded = (std::min)(
                    request->downloaded, request->end - request->start);
                result.downloaded_bytes += downloaded;
                result.ranges.push_back({request->start, request->end, downloaded});
            } else if (request->kind == RequestKind::sequential) {
                result.downloaded_bytes = request->downloaded;
                if (request->downloaded > 0) {
                    result.ranges.push_back({0,
                        (std::max)(result.total_bytes, request->downloaded),
                        request->downloaded});
                }
            }
        }
        return result;
    }

    void clear() {
        for (auto& [id, transfer] : transfers) {
            (void)id;
            if (transfer->mode == TransferMode::segmented) {
                (void)persist_state(*transfer);
            }
            destroy_requests(*transfer);
        }
        transfers.clear();
    }
};

HttpDownloadManager::HttpDownloadManager() : impl_(std::make_unique<Impl>()) {
    if (curl_runtime().result() != CURLE_OK) {
        throw std::runtime_error("cannot initialize libcurl");
    }
    impl_->multi = curl_multi_init();
    if (impl_->multi == nullptr) throw std::runtime_error("cannot create libcurl multi handle");
}

HttpDownloadManager::~HttpDownloadManager() {
    if (!impl_) return;
    impl_->clear();
    if (impl_->multi != nullptr) curl_multi_cleanup(impl_->multi);
}

std::optional<HttpTransferFailure> HttpDownloadManager::start(const std::string& id,
    const std::string& url, const std::filesystem::path& partial_path,
    const std::string& user_agent, std::size_t max_connections,
    const EngineProxyConfig& proxy) {
    if (impl_->transfers.contains(id)) {
        return HttpTransferFailure{"INTERNAL_ERROR", "HTTP task is already active", false, false};
    }

    auto transfer = std::make_unique<Impl::Transfer>();
    transfer->id = id;
    transfer->url = url;
    transfer->user_agent = user_agent;
    transfer->proxy = proxy;
    transfer->partial_path = partial_path;
    transfer->max_connections = std::clamp<std::size_t>(max_connections, 1, 8);

    std::optional<HttpTransferFailure> failure;
    if (const auto restored = load_transfer_state(partial_path)) {
        transfer->total_bytes = restored->progress.total_bytes;
        transfer->etag = restored->etag;
        transfer->last_modified = restored->last_modified;
        failure = impl_->start_segmented(*transfer, &restored->progress.ranges);
    } else {
        std::error_code error;
        std::uint64_t legacy_offset = 0;
        if (std::filesystem::exists(partial_path, error)) {
            if (error || !std::filesystem::is_regular_file(partial_path, error)) {
                return HttpTransferFailure{"STORAGE_ERROR",
                    "HTTP partial path is not a regular file", false, false};
            }
            legacy_offset = static_cast<std::uint64_t>(
                std::filesystem::file_size(partial_path, error));
            if (error || legacy_offset > static_cast<std::uint64_t>(
                    (std::numeric_limits<curl_off_t>::max)())) {
                return HttpTransferFailure{"STORAGE_ERROR",
                    "cannot read HTTP partial file size", true, false};
            }
        }
        failure = legacy_offset > 0
            ? impl_->add_sequential(*transfer, legacy_offset)
            : impl_->add_probe(*transfer);
    }
    if (failure) return failure;
    impl_->transfers.emplace(id, std::move(transfer));
    return std::nullopt;
}

std::optional<std::string> select_http_proxy(
    std::string_view url, const EngineProxyConfig& proxy) {
    if (!proxy.enabled) return std::nullopt;

    auto* handle = curl_url();
    if (handle == nullptr) return std::nullopt;
    const auto cleanup = [&] { curl_url_cleanup(handle); };
    if (curl_url_set(handle, CURLUPART_URL, std::string(url).c_str(), 0) != CURLUE_OK) {
        cleanup();
        return std::nullopt;
    }
    const auto scheme = url_part(handle, CURLUPART_SCHEME);
    const auto host = url_part(handle, CURLUPART_HOST);
    cleanup();
    if (!scheme || !host || proxy_bypassed(*host, proxy.bypass)) return std::nullopt;
    if (lowercase_ascii(*scheme) == "https") {
        return proxy.https_proxy ? proxy.https_proxy : proxy.http_proxy;
    }
    return proxy.http_proxy ? proxy.http_proxy : proxy.https_proxy;
}

bool HttpDownloadManager::cancel(const std::string& id) {
    const auto found = impl_->transfers.find(id);
    if (found == impl_->transfers.end()) return false;
    if (found->second->mode == Impl::TransferMode::segmented) {
        (void)impl_->persist_state(*found->second);
    }
    impl_->destroy_requests(*found->second);
    impl_->transfers.erase(found);
    return true;
}

bool HttpDownloadManager::contains(const std::string& id) const {
    return impl_->transfers.contains(id);
}

std::size_t HttpDownloadManager::active_count() const noexcept {
    return impl_->transfers.size();
}

std::optional<HttpTransferProgress> HttpDownloadManager::progress(
    const std::string& id) const {
    const auto found = impl_->transfers.find(id);
    if (found == impl_->transfers.end()) return std::nullopt;
    return impl_->progress(*found->second);
}

std::vector<HttpTransferUpdate> HttpDownloadManager::poll(std::uint64_t total_rate_limit) {
    std::vector<HttpTransferUpdate> updates;
    if (impl_->transfers.empty()) return updates;

    std::size_t active_requests = 0;
    for (auto& [id, transfer] : impl_->transfers) {
        (void)id;
        for (const auto& request : transfer->requests) {
            if (request->easy != nullptr) ++active_requests;
        }
    }
    const auto per_request_limit = total_rate_limit == 0 || active_requests == 0
        ? std::uint64_t{0}
        : std::max<std::uint64_t>(1, total_rate_limit / active_requests);
    for (auto& [id, transfer] : impl_->transfers) {
        (void)id;
        const auto limit = per_request_limit > static_cast<std::uint64_t>(
                (std::numeric_limits<curl_off_t>::max)())
            ? (std::numeric_limits<curl_off_t>::max)()
            : static_cast<curl_off_t>(per_request_limit);
        for (const auto& request : transfer->requests) {
            if (request->easy != nullptr) {
                curl_easy_setopt(request->easy, CURLOPT_MAX_RECV_SPEED_LARGE, limit);
            }
        }
    }

    int running = 0;
    const auto multi_result = curl_multi_perform(impl_->multi, &running);
    if (multi_result != CURLM_OK) {
        for (const auto& [id, transfer] : impl_->transfers) {
            const auto progress = impl_->progress(*transfer);
            updates.push_back({id, progress.total_bytes,
                progress.downloaded_bytes, 0, true, false,
                HttpTransferFailure{"HTTP_TRANSFER_ERROR",
                    curl_multi_strerror(multi_result), true, false},
                progress.active_connections, progress.ranges});
        }
        impl_->clear();
        return updates;
    }

    int messages = 0;
    while (const auto* message = curl_multi_info_read(impl_->multi, &messages)) {
        if (message->msg != CURLMSG_DONE) continue;
        Impl::Request* raw_request = nullptr;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &raw_request);
        if (raw_request == nullptr || raw_request->transfer == nullptr) continue;
        auto& request = *raw_request;
        auto& transfer = *request.transfer;
        const auto result = message->data.result;

        curl_off_t speed = 0;
        curl_easy_getinfo(request.easy, CURLINFO_SPEED_DOWNLOAD_T, &speed);
        request.download_rate = static_cast<std::uint64_t>(
            std::max<curl_off_t>(0, speed));
        long status = request.response_status;
        curl_easy_getinfo(request.easy, CURLINFO_RESPONSE_CODE, &status);

        bool flush_failed = false;
        bool close_failed = false;
        if (request.output != nullptr) {
            flush_failed = std::fflush(request.output) != 0;
            close_failed = std::fclose(request.output) != 0;
            request.output = nullptr;
        }
        curl_multi_remove_handle(impl_->multi, request.easy);
        curl_easy_cleanup(request.easy);
        request.easy = nullptr;

        const bool successful_status = status >= 200 && status < 300;
        std::optional<HttpTransferFailure> failure;
        if (flush_failed || close_failed) {
            failure = HttpTransferFailure{
                "STORAGE_ERROR", "cannot flush HTTP partial file", true, false};
        } else if (request.callback_failure) {
            failure = request.callback_failure;
        } else if (result != CURLE_OK) {
            const auto detail = request.error_buffer.front() == '\0'
                ? std::string(curl_easy_strerror(result))
                : std::string(request.error_buffer.data());
            failure = curl_failure(result, status, detail,
                request.start > 0 || request.kind != Impl::RequestKind::sequential);
        } else if (!successful_status) {
            const bool redirect = status >= 300 && status < 400;
            failure = HttpTransferFailure{
                redirect ? "HTTP_REDIRECT_ERROR" : "HTTP_STATUS_ERROR",
                "HTTP request ended with status " + std::to_string(status), false, false};
        }

        if (failure) {
            if (failure->restart_without_range && !transfer.fallback_used) {
                transfer.fallback_requested = true;
            } else if (!transfer.failure) {
                transfer.failure = failure;
            }
            continue;
        }

        if (request.kind == Impl::RequestKind::probe) {
            if (status != 206 || !request.content_range
                || request.content_range->start != 0
                || request.content_range->end != 0) {
                transfer.fallback_requested = true;
            } else {
                transfer.total_bytes = request.content_range->total;
                transfer.etag = request.etag;
                transfer.last_modified = request.last_modified;
                transfer.probe_ready = true;
            }
        } else if (request.kind == Impl::RequestKind::segment) {
            const auto expected_start = request.start + request.initial_downloaded;
            const auto validator_changed =
                (!transfer.etag.empty() && !request.etag.empty()
                    && transfer.etag != request.etag)
                || (!transfer.last_modified.empty() && !request.last_modified.empty()
                    && transfer.last_modified != request.last_modified);
            if (status != 206 || !request.content_range
                || request.content_range->start != expected_start
                || request.content_range->end != request.end - 1
                || request.content_range->total != transfer.total_bytes
                || validator_changed
                || request.downloaded != request.end - request.start) {
                transfer.fallback_requested = true;
            }
        }
    }

    std::vector<std::string> completed_ids;
    for (auto& [id, transfer] : impl_->transfers) {
        if (transfer->fallback_requested) {
            transfer->fallback_requested = false;
            transfer->probe_ready = false;
            transfer->fallback_used = true;
            impl_->destroy_requests(*transfer);
            std::error_code error;
            remove_http_transfer_state(transfer->partial_path, error);
            if (error) {
                transfer->failure = HttpTransferFailure{"STORAGE_ERROR",
                    "cannot reset HTTP range state: " + error.message(), true, false};
            } else if (const auto failure = impl_->add_sequential(*transfer, 0)) {
                transfer->failure = failure;
            }
        } else if (transfer->probe_ready) {
            transfer->probe_ready = false;
            if (const auto failure = impl_->start_segmented(*transfer)) {
                transfer->failure = failure;
            }
        }

        if (transfer->failure) {
            const auto progress = impl_->progress(*transfer);
            updates.push_back({id, progress.total_bytes,
                progress.downloaded_bytes, 0, true, false, transfer->failure,
                progress.active_connections, progress.ranges});
            impl_->destroy_requests(*transfer);
            completed_ids.push_back(id);
            continue;
        }

        if (transfer->mode == Impl::TransferMode::segmented) {
            if (const auto failure = impl_->activate_pending_ranges(*transfer)) {
                transfer->failure = failure;
                const auto progress = impl_->progress(*transfer);
                updates.push_back({id, progress.total_bytes,
                    progress.downloaded_bytes, 0, true, false, transfer->failure,
                    progress.active_connections, progress.ranges});
                impl_->destroy_requests(*transfer);
                completed_ids.push_back(id);
                continue;
            }
        }

        const bool segmented_complete = transfer->mode == Impl::TransferMode::segmented
            && !transfer->requests.empty()
            && std::all_of(transfer->requests.begin(), transfer->requests.end(),
                [](const auto& request) {
                    return request->downloaded == request->end - request->start;
                });
        const bool sequential_complete = transfer->mode == Impl::TransferMode::sequential
            && transfer->requests.size() == 1
            && transfer->requests.front()->easy == nullptr;
        if (segmented_complete || sequential_complete) {
            if (segmented_complete) {
                if (const auto failure = impl_->persist_state(*transfer)) {
                    updates.push_back({id, transfer->total_bytes, 0, 0,
                        true, false, failure, 0, {}});
                    impl_->destroy_requests(*transfer);
                    completed_ids.push_back(id);
                    continue;
                }
            }
            const auto progress = impl_->progress(*transfer);
            updates.push_back({id, progress.total_bytes,
                progress.downloaded_bytes, 0, true, true, std::nullopt,
                0, progress.ranges});
            impl_->destroy_requests(*transfer);
            completed_ids.push_back(id);
            continue;
        }

        if (transfer->mode == Impl::TransferMode::segmented
            && std::chrono::steady_clock::now() >= transfer->next_persist) {
            if (const auto failure = impl_->persist_state(*transfer)) {
                transfer->failure = failure;
            }
        }
    }
    for (const auto& id : completed_ids) impl_->transfers.erase(id);

    for (auto& [id, transfer] : impl_->transfers) {
        std::uint64_t speed = 0;
        for (auto& request : transfer->requests) {
            if (request->easy == nullptr) continue;
            curl_off_t current = 0;
            curl_easy_getinfo(request->easy, CURLINFO_SPEED_DOWNLOAD_T, &current);
            request->download_rate = static_cast<std::uint64_t>(
                std::max<curl_off_t>(0, current));
            speed += request->download_rate;
        }
        const auto progress = impl_->progress(*transfer);
        updates.push_back({id, progress.total_bytes,
            progress.downloaded_bytes, speed, false, false, std::nullopt,
            progress.active_connections, progress.ranges});
    }
    return updates;
}

std::optional<HttpTransferProgress> read_http_transfer_progress(
    const std::filesystem::path& partial_path) {
    const auto state = load_transfer_state(partial_path);
    return state ? std::optional(state->progress) : std::nullopt;
}

bool remove_http_transfer_state(const std::filesystem::path& partial_path,
    std::error_code& error) {
    error.clear();
    const auto state = transfer_state_path(partial_path);
    auto temporary = state;
    temporary += ".tmp";
    std::filesystem::remove(state, error);
    if (error) return false;
    std::filesystem::remove(temporary, error);
    return !error;
}

std::optional<std::string> normalize_http_url(std::string_view value) {
    if (value.empty() || value.size() > 64U * 1024U || curl_runtime().result() != CURLE_OK) {
        return std::nullopt;
    }
    const auto separator = value.find("://");
    if (separator == std::string_view::npos) return std::nullopt;
    const auto input_scheme = lowercase_ascii(std::string(value.substr(0, separator)));
    if (input_scheme != "http" && input_scheme != "https") return std::nullopt;
    const auto authority = separator + 3;
    if (authority >= value.size() || value[authority] == '/'
        || value[authority] == '?' || value[authority] == '#') {
        return std::nullopt;
    }
    CURLU* handle = curl_url();
    if (handle == nullptr) return std::nullopt;
    const std::string input(value);
    const auto parsed = curl_url_set(handle, CURLUPART_URL, input.c_str(), 0);
    const auto scheme = parsed == CURLUE_OK ? url_part(handle, CURLUPART_SCHEME) : std::nullopt;
    const auto host = parsed == CURLUE_OK ? url_part(handle, CURLUPART_HOST) : std::nullopt;
    const auto normalized = parsed == CURLUE_OK ? url_part(handle, CURLUPART_URL) : std::nullopt;
    curl_url_cleanup(handle);
    if (!scheme || !host || host->empty() || !normalized) return std::nullopt;
    const auto lower_scheme = lowercase_ascii(*scheme);
    if (lower_scheme != "http" && lower_scheme != "https") return std::nullopt;
    return normalized;
}

std::string http_file_name_from_url(std::string_view value) {
    CURLU* handle = curl_url();
    if (handle == nullptr) return "download";
    const std::string input(value);
    if (curl_url_set(handle, CURLUPART_URL, input.c_str(), 0) != CURLUE_OK) {
        curl_url_cleanup(handle);
        return "download";
    }
    const auto path = url_part(handle, CURLUPART_PATH).value_or(std::string{});
    curl_url_cleanup(handle);
    const auto slash = path.find_last_of('/');
    const auto segment = slash == std::string::npos ? path : path.substr(slash + 1);
    return safe_http_file_name(percent_decode(segment));
}

} // namespace bt
