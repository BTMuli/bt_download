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

    expect(bt::http_file_name_from_content_disposition(
            "attachment; filename=\"Pretty Name.mkv\"").value_or("") == "Pretty Name.mkv",
        "quoted Content-Disposition filename was not parsed");
    expect(bt::http_file_name_from_content_disposition(
            "attachment; filename=plain.mkv").value_or("") == "plain.mkv",
        "token Content-Disposition filename was not parsed");
    expect(bt::http_file_name_from_content_disposition(
            "attachment; filename=\"fallback.mkv\"; filename*=UTF-8''%E6%B5%8B%E8%AF%95.mkv")
            .value_or("") == "\xE6\xB5\x8B\xE8\xAF\x95.mkv",
        "RFC 5987 filename* did not take precedence");
    expect(bt::http_file_name_from_content_disposition(
            "attachment; filename*=ISO-8859-1''caf%E9.txt").value_or("") == "caf\xc3\xa9.txt",
        "ISO-8859-1 filename* was not decoded");
    expect(bt::http_file_name_from_content_disposition(
            "attachment; filename=\"a;b.mkv\"").value_or("") == "a;b.mkv",
        "quoted filename containing a semicolon was split");
    expect(!bt::http_file_name_from_content_disposition("inline"),
        "Content-Disposition without a filename was accepted");
    expect(bt::http_file_name_from_content_disposition(
            "attachment; filename=\"../secret.txt\"").value_or("") == ".._secret.txt",
        "Content-Disposition path separators were not sanitized");

    expect(bt::resolve_http_file_name(
            "https://cdn.example/a1b2c3d4e5f67890abcdef1234567890",
            "https://cdn.example/a1b2c3d4e5f67890abcdef1234567890",
            "attachment; filename=\"Show.S01E01.mkv\"") == "Show.S01E01.mkv",
        "Content-Disposition did not override a hash URL");
    expect(bt::resolve_http_file_name(
            "https://cdn.example/redirect.bin",
            "https://cdn.example/payload.bin", "") == "payload.bin",
        "redirected URL filename was not used");
    expect(bt::resolve_http_file_name(
            "https://cdn.example/a1b2c3d4e5f67890",
            "https://cdn.example/", "") == "a1b2c3d4e5f67890",
        "empty redirected path replaced the original filename");
}
