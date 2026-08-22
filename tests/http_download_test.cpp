#include "bt_download/http_download.hpp"

#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

void run_http_download_tests() {
    const auto normalized = bt::normalize_http_url(
        "https://example.com/releases/App%20Setup.exe?token=private");
    expect(normalized.has_value(), "valid HTTPS URL was rejected");
    expect(!bt::normalize_http_url("file:///C:/Windows/system.ini"),
        "file URL was accepted");
    expect(!bt::normalize_http_url("https:///missing-host"),
        "hostless HTTPS URL was accepted");
    expect(bt::http_file_name_from_url(*normalized) == "App Setup.exe",
        "HTTP filename was not decoded");
    expect(bt::http_file_name_from_url("https://example.com/") == "download",
        "empty HTTP filename did not use the fallback");
    expect(bt::http_file_name_from_url("https://example.com/CON.txt") == "_CON.txt",
        "reserved Windows filename was not escaped");
    expect(bt::http_file_name_from_url("https://example.com/a%2Fb%3Fc.txt") == "a_b_c.txt",
        "unsafe HTTP filename characters were not replaced");
    expect(bt::http_file_name_from_url("https://example.com/%FF.exe") == "_.exe",
        "invalid UTF-8 in HTTP filename was not replaced");
    const std::string long_name(190, 'a');
    expect(bt::http_file_name_from_url("https://example.com/" + long_name + ".exe").ends_with(".exe"),
        "HTTP filename truncation discarded the extension");
}
