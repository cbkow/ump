#pragma once

#include <string>

namespace ump {
namespace Integrations {

/**
 * Parses Frame.io URLs to extract asset IDs for API calls.
 *
 * Supports:
 * - Short URLs: https://f.io/MtJwtwSb
 * - Full URLs: https://next.frame.io/share/{share_id}/view/{asset_id}
 * - Direct UUID input: 3e7e30f1-4b13-4b6b-8f68-631c2df85dd9
 */
class FrameioUrlParser {
public:
    struct ParseResult {
        bool success = false;
        std::string asset_id;
        std::string error_message;
    };

    /**
     * Parse a Frame.io URL or asset ID.
     * Handles short URLs by following redirects.
     *
     * @param input Frame.io URL (short or full) or direct asset_id UUID
     * @return ParseResult with success status and extracted asset_id
     */
    static ParseResult Parse(const std::string& input);

    /**
     * Check if string looks like a Frame.io URL.
     */
    static bool IsFrameioUrl(const std::string& str);

    /**
     * Check if string is a valid UUID format.
     */
    static bool IsUuid(const std::string& str);

private:
    /**
     * Follow HTTP redirect and return final URL.
     * Used for f.io short URLs.
     */
    static std::string FollowRedirect(const std::string& url);

    /**
     * Extract asset_id from full Frame.io URL.
     * Pattern: /view/{asset_id}
     */
    static std::string ExtractAssetId(const std::string& url);
};

} // namespace Integrations
} // namespace ump
