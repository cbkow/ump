#include "frameio_url_parser.h"
#include <regex>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace ump {
namespace Integrations {

FrameioUrlParser::ParseResult FrameioUrlParser::Parse(const std::string& input) {
    ParseResult result;

    if (input.empty()) {
        result.error_message = "Empty input";
        return result;
    }

    // Trim whitespace
    std::string trimmed = input;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);

    // Check if it's already a UUID
    if (IsUuid(trimmed)) {
        result.success = true;
        result.asset_id = trimmed;
        return result;
    }

    // Check if it's a Frame.io URL
    if (!IsFrameioUrl(trimmed)) {
        result.error_message = "Input is not a Frame.io URL or valid UUID";
        return result;
    }

    // Handle short URLs (f.io) by following redirect
    if (trimmed.find("f.io") != std::string::npos) {
        std::string expanded_url = FollowRedirect(trimmed);
        if (expanded_url.empty()) {
            result.error_message = "Failed to resolve short URL";
            return result;
        }
        trimmed = expanded_url;
    }

    // Extract asset_id from full URL
    std::string asset_id = ExtractAssetId(trimmed);
    if (asset_id.empty()) {
        result.error_message = "Could not extract asset ID from URL";
        return result;
    }

    result.success = true;
    result.asset_id = asset_id;
    return result;
}

bool FrameioUrlParser::IsFrameioUrl(const std::string& str) {
    // Convert to lowercase for comparison
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    return (lower.find("frame.io") != std::string::npos ||
            lower.find("f.io") != std::string::npos);
}

bool FrameioUrlParser::IsUuid(const std::string& str) {
    // UUID format: 8-4-4-4-12 hex digits with dashes
    // Example: 3e7e30f1-4b13-4b6b-8f68-631c2df85dd9
    std::regex uuid_pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
    );
    return std::regex_match(str, uuid_pattern);
}

std::string FrameioUrlParser::FollowRedirect(const std::string& url) {
#ifdef _WIN32
    // Parse URL to extract host and path
    std::string host;
    std::string path;

    // Simple URL parsing for f.io URLs
    size_t protocol_end = url.find("://");
    if (protocol_end == std::string::npos) {
        return "";
    }

    size_t host_start = protocol_end + 3;
    size_t path_start = url.find('/', host_start);

    if (path_start != std::string::npos) {
        host = url.substr(host_start, path_start - host_start);
        path = url.substr(path_start);
    } else {
        host = url.substr(host_start);
        path = "/";
    }

    // Convert to wide strings for WinHTTP
    int host_len = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    int path_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);

    std::wstring wide_host(host_len, 0);
    std::wstring wide_path(path_len, 0);

    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &wide_host[0], host_len);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide_path[0], path_len);

    // Use WinHTTP to follow redirect
    HINTERNET hSession = WinHttpOpen(
        L"ump/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession) {
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        wide_host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"HEAD",  // HEAD request to get redirect without downloading content
        wide_path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Disable automatic redirects so we can read the Location header
    DWORD dwOption = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &dwOption, sizeof(dwOption));

    std::string final_url;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {

        // Check status code
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusCodeSize,
            WINHTTP_NO_HEADER_INDEX
        );

        // If it's a redirect (301, 302, 307, 308), get Location header
        if (statusCode == 301 || statusCode == 302 || statusCode == 307 || statusCode == 308) {
            DWORD locationSize = 0;
            WinHttpQueryHeaders(
                hRequest,
                WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX,
                nullptr,
                &locationSize,
                WINHTTP_NO_HEADER_INDEX
            );

            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                std::wstring location(locationSize / sizeof(wchar_t), 0);
                if (WinHttpQueryHeaders(
                    hRequest,
                    WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &location[0],
                    &locationSize,
                    WINHTTP_NO_HEADER_INDEX
                )) {
                    // Convert wide string back to UTF-8
                    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, location.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    if (utf8_len > 0) {
                        final_url.resize(utf8_len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, location.c_str(), -1, &final_url[0], utf8_len, nullptr, nullptr);
                    }
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return final_url;
#else
    // TODO: Implement for non-Windows platforms using libcurl
    return "";
#endif
}

std::string FrameioUrlParser::ExtractAssetId(const std::string& url) {
    // Extract the last UUID from the URL (works with /view/, /reel/, or any other pattern)
    // Examples:
    //   https://next.frame.io/share/81d8c0f4-1218-46c3-bbd6-51d178569397/view/3e7e30f1-4b13-4b6b-8f68-631c2df85dd9
    //   https://next.frame.io/share/c67a1c41-172c-4fe9-8977-cc9d493772f6/reel/8afa8c7b-f47c-48f9-afa4-17cc15d9e275
    // Both return the last UUID (the asset ID)

    std::regex uuid_pattern(R"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");

    // Find all UUIDs in the URL
    std::string last_uuid;
    auto words_begin = std::sregex_iterator(url.begin(), url.end(), uuid_pattern);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        last_uuid = match.str();
    }

    return last_uuid;
}

} // namespace Integrations
} // namespace ump
