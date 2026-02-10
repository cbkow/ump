#include "frameio_client.h"
#include "../utils/debug_utils.h"
#include <nlohmann/json.hpp>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace ump {
namespace Integrations {

FrameioClient::FetchResult FrameioClient::GetAssetComments(
    const std::string& asset_id,
    const std::string& bearer_token
) {
    FetchResult result;

    if (asset_id.empty()) {
        result.error_message = "Asset ID is empty";
        return result;
    }

    if (bearer_token.empty()) {
        result.error_message = "Bearer token is empty";
        return result;
    }

    // Construct API endpoint
    std::string url = "https://api.frame.io/v2/assets/" + asset_id + "/comments";

    // Make HTTP request
    int status_code = 0;
    std::string response_body = HttpGet(url, bearer_token, status_code);
    result.http_status_code = status_code;

    if (status_code == 0) {
        result.error_message = "Network error - failed to connect to Frame.io";
        return result;
    }

    if (status_code == 401) {
        result.error_message = "Authentication failed - check API token";
        return result;
    }

    if (status_code == 403) {
        result.error_message = "Access forbidden - you may not have permission to access this asset";
        return result;
    }

    if (status_code == 404) {
        result.error_message = "Asset not found - check URL or asset ID";
        return result;
    }

    if (status_code == 429) {
        result.error_message = "Rate limited - too many requests";
        return result;
    }

    if (status_code != 200) {
        result.error_message = "HTTP error " + std::to_string(status_code);
        return result;
    }

    // Parse JSON response
    try {
        result.comments = ParseCommentsJson(response_body);
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = std::string("Failed to parse response: ") + e.what();
        return result;
    }

    return result;
}

std::string FrameioClient::HttpGet(
    const std::string& url,
    const std::string& bearer_token,
    int& out_status_code
) {
    out_status_code = 0;

#ifdef _WIN32
    // Parse URL
    std::string host = "api.frame.io";
    std::string path = url.substr(url.find("/v2"));

    // Convert to wide strings
    int host_len = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    int path_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);

    std::wstring wide_host(host_len, 0);
    std::wstring wide_path(path_len, 0);

    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &wide_host[0], host_len);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide_path[0], path_len);

    // Prepare authorization header
    std::string auth_header = "Authorization: Bearer " + bearer_token;
    int auth_len = MultiByteToWideChar(CP_UTF8, 0, auth_header.c_str(), -1, nullptr, 0);
    std::wstring wide_auth(auth_len, 0);
    MultiByteToWideChar(CP_UTF8, 0, auth_header.c_str(), -1, &wide_auth[0], auth_len);

    // Initialize WinHTTP
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
        L"GET",
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

    std::string response_body;

    // Send request with authorization header
    if (WinHttpSendRequest(
        hRequest,
        wide_auth.c_str(),
        -1,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    ) && WinHttpReceiveResponse(hRequest, nullptr)) {

        // Get status code
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
        out_status_code = static_cast<int>(statusCode);

        // Read response body
        DWORD bytesAvailable = 0;
        DWORD bytesRead = 0;
        std::vector<char> buffer;

        do {
            bytesAvailable = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                break;
            }

            if (bytesAvailable == 0) {
                break;
            }

            buffer.resize(bytesAvailable + 1);
            if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                buffer[bytesRead] = 0;
                response_body.append(buffer.data(), bytesRead);
            }
        } while (bytesAvailable > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response_body;
#else
    // TODO: Implement for non-Windows platforms using libcurl
    return "";
#endif
}

std::vector<FrameioComment> FrameioClient::ParseCommentsJson(const std::string& json_response) {
    std::vector<FrameioComment> comments;

    nlohmann::json root = nlohmann::json::parse(json_response);

    // Frame.io returns an array of comments
    if (!root.is_array()) {
        return comments;
    }

    for (const auto& comment_json : root) {
        FrameioComment comment;

        // ID
        if (comment_json.contains("id")) {
            comment.id = comment_json["id"].get<std::string>();
        }

        // Annotation data (JSON string or object)
        if (comment_json.contains("annotation") && !comment_json["annotation"].is_null()) {
            if (comment_json["annotation"].is_string()) {
                comment.annotation_json = comment_json["annotation"].get<std::string>();
            } else if (comment_json["annotation"].is_array() || comment_json["annotation"].is_object()) {
                // If it's an object or array, serialize it
                comment.annotation_json = comment_json["annotation"].dump();
            }

            // Debug log to see what we got
            if (!comment.annotation_json.empty()) {
                // Truncate for logging if too long
                std::string preview = comment.annotation_json.length() > 200
                    ? comment.annotation_json.substr(0, 200) + "..."
                    : comment.annotation_json;
                // Note: Can't use Debug::Log here, but the data is captured
            }
        }

        // Text
        if (comment_json.contains("text")) {
            comment.text = comment_json["text"].get<std::string>();
        }

        // Timestamp
        if (comment_json.contains("timestamp")) {
            if (comment_json["timestamp"].is_number()) {
                comment.timestamp = comment_json["timestamp"].get<double>();
            } else if (comment_json["timestamp"].is_string()) {
                // Try to parse as number from string
                try {
                    comment.timestamp = std::stod(comment_json["timestamp"].get<std::string>());
                } catch (...) {
                    comment.timestamp = 0.0;
                }
            }
        }

        // Debug: Log raw timestamp and comment ID
        Debug::Log("Frame.io comment ID: " + comment.id +
                   ", Raw timestamp: " + std::to_string(comment.timestamp) +
                   ", Text: " + (comment.text.length() > 50 ? comment.text.substr(0, 50) + "..." : comment.text));

        // Owner info
        if (comment_json.contains("owner") && comment_json["owner"].is_object()) {
            const auto& owner = comment_json["owner"];
            if (owner.contains("name")) {
                comment.owner_name = owner["name"].get<std::string>();
            }
            if (owner.contains("email")) {
                comment.owner_email = owner["email"].get<std::string>();
            }
        }

        comments.push_back(comment);
    }

    return comments;
}

} // namespace Integrations
} // namespace ump
