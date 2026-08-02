#include "tracker_config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <unordered_set>

namespace bt {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool is_default_port(std::string_view scheme, int port) {
    return (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
}

bool contains_invalid_url_character(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character <= 0x20 || character == 0x7f;
    });
}

int parse_port(std::string_view value) {
    if (value.empty()) throw std::invalid_argument("tracker URL port is empty");
    int port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
        || port < 1 || port > 65535) {
        throw std::invalid_argument("tracker URL port is invalid");
    }
    return port;
}

} // namespace

std::string normalize_tracker_url(std::string_view raw_url) {
    if (raw_url.empty() || raw_url.size() > max_tracker_url_bytes) {
        throw std::invalid_argument("tracker URL length is invalid");
    }
    if (raw_url.find('#') != std::string_view::npos) {
        throw std::invalid_argument("tracker URL fragments are not allowed");
    }

    if (contains_invalid_url_character(raw_url)) {
        throw std::invalid_argument("tracker URL contains whitespace or control characters");
    }
    const auto scheme_end = raw_url.find("://");
    if (scheme_end == std::string_view::npos) throw std::invalid_argument("tracker URL has no scheme");
    auto scheme = lower_ascii(std::string(raw_url.substr(0, scheme_end)));
    if (scheme != "udp" && scheme != "http" && scheme != "https") {
        throw std::invalid_argument("tracker URL scheme is unsupported");
    }

    const auto remainder = raw_url.substr(scheme_end + 3);
    const auto authority_end = remainder.find_first_of("/?");
    const auto authority = remainder.substr(0, authority_end);
    auto path = authority_end == std::string_view::npos ? std::string_view{} : remainder.substr(authority_end);
    if (authority.empty()) throw std::invalid_argument("tracker URL host is required");
    if (authority.find('@') != std::string_view::npos) {
        throw std::invalid_argument("tracker URL credentials are not allowed");
    }

    std::string host;
    int port = -1;
    bool ipv6_literal = false;
    if (authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos || closing == 1) {
            throw std::invalid_argument("tracker URL IPv6 host is invalid");
        }
        host = std::string(authority.substr(1, closing - 1));
        ipv6_literal = true;
        const auto suffix = authority.substr(closing + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':') throw std::invalid_argument("tracker URL authority is invalid");
            port = parse_port(suffix.substr(1));
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) {
                throw std::invalid_argument("IPv6 tracker hosts must use brackets");
            }
            host = std::string(authority.substr(0, colon));
            port = parse_port(authority.substr(colon + 1));
        } else {
            host = std::string(authority);
        }
    }
    host = lower_ascii(std::move(host));
    if (host.empty()) throw std::invalid_argument("tracker URL host is required");
    if (scheme == "udp" && port < 1) throw std::invalid_argument("UDP tracker URL port is required");

    std::string normalized = scheme + "://";
    if (ipv6_literal) normalized += '[';
    normalized += host;
    if (ipv6_literal) normalized += ']';
    if (port > 0 && !is_default_port(scheme, port)) normalized += ':' + std::to_string(port);
    if (path.empty()) normalized += '/';
    else if (path.front() == '/') normalized += path;
    else normalized += '/' + std::string(path);
    if (normalized.size() > max_tracker_url_bytes) {
        throw std::invalid_argument("normalized tracker URL is too long");
    }
    return normalized;
}

std::vector<std::string> normalize_tracker_urls(const nlohmann::json& value) {
    if (!value.is_array()) throw std::invalid_argument("additionalTrackers must be an array");
    if (value.size() > max_additional_trackers) {
        throw std::invalid_argument("additionalTrackers cannot contain more than 512 entries");
    }

    std::vector<std::string> result;
    result.reserve(value.size());
    std::unordered_set<std::string> seen;
    for (const auto& item : value) {
        if (!item.is_string()) throw std::invalid_argument("additionalTrackers entries must be strings");
        auto normalized = normalize_tracker_url(item.get<std::string>());
        if (seen.insert(normalized).second) result.push_back(std::move(normalized));
    }
    return result;
}

} // namespace bt
