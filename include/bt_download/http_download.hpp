#pragma once

#include "bt_download/proxy_config.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace bt {

struct HttpTransferFailure {
    std::string code;
    std::string message;
    bool retryable{false};
    bool restart_without_range{false};
};

struct HttpByteRangeProgress {
    std::uint64_t start{0};
    std::uint64_t end{0};
    std::uint64_t downloaded{0};
};

struct HttpTransferProgress {
    std::uint64_t total_bytes{0};
    std::uint64_t downloaded_bytes{0};
    std::size_t active_connections{0};
    std::vector<HttpByteRangeProgress> ranges;
};

struct HttpTransferUpdate {
    std::string id;
    std::uint64_t total_bytes{0};
    std::uint64_t downloaded_bytes{0};
    std::uint64_t download_rate{0};
    bool finished{false};
    bool succeeded{false};
    std::optional<HttpTransferFailure> failure;
    std::size_t active_connections{0};
    std::vector<HttpByteRangeProgress> ranges;
};

class HttpDownloadManager {
public:
    HttpDownloadManager();
    ~HttpDownloadManager();

    HttpDownloadManager(const HttpDownloadManager&) = delete;
    HttpDownloadManager& operator=(const HttpDownloadManager&) = delete;

    std::optional<HttpTransferFailure> start(const std::string& id,
        const std::string& url,
        const std::filesystem::path& partial_path,
        const std::string& user_agent,
        std::size_t max_connections,
        const EngineProxyConfig& proxy);
    bool cancel(const std::string& id);
    bool contains(const std::string& id) const;
    std::size_t active_count() const noexcept;
    std::optional<HttpTransferProgress> progress(const std::string& id) const;
    std::vector<HttpTransferUpdate> poll(std::uint64_t total_rate_limit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::optional<std::string> normalize_http_url(std::string_view value);
std::string http_file_name_from_url(std::string_view value);
std::optional<std::string> select_http_proxy(
    std::string_view url, const EngineProxyConfig& proxy);
std::optional<HttpTransferProgress> read_http_transfer_progress(
    const std::filesystem::path& partial_path);
bool remove_http_transfer_state(const std::filesystem::path& partial_path,
    std::error_code& error);

} // namespace bt
