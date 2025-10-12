#pragma once

#include <string>
#include <vector>

namespace ump {
namespace Integrations {

/**
 * Represents a single comment from Frame.io API.
 */
struct FrameioComment {
    std::string id;
    std::string annotation_json;  // Raw JSON string from "annotation" field
    std::string text;              // Comment text
    double timestamp = 0.0;        // Frame number or seconds
    std::string owner_name;
    std::string owner_email;
};

/**
 * HTTP client for Frame.io REST API v2.
 */
class FrameioClient {
public:
    struct FetchResult {
        bool success = false;
        std::vector<FrameioComment> comments;
        std::string error_message;
        int http_status_code = 0;
    };

    /**
     * Fetch all comments for a given asset.
     *
     * @param asset_id Frame.io asset UUID
     * @param bearer_token API authentication token
     * @return FetchResult with comments or error
     */
    static FetchResult GetAssetComments(
        const std::string& asset_id,
        const std::string& bearer_token
    );

private:
    /**
     * Make HTTP GET request to Frame.io API.
     */
    static std::string HttpGet(
        const std::string& url,
        const std::string& bearer_token,
        int& out_status_code
    );

    /**
     * Parse Frame.io API JSON response into FrameioComment objects.
     */
    static std::vector<FrameioComment> ParseCommentsJson(const std::string& json_response);
};

} // namespace Integrations
} // namespace ump
