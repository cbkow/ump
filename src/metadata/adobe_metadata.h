#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include "../utils/debug_utils.h"

struct AdobeMetadata {
    // Existing Adobe project paths
    std::string ae_project_path;
    std::string premiere_win_path;
    std::string premiere_mac_path;

    // === ADD TIMECODE FIELDS ===
    std::string qt_start_timecode;      // QuickTime:StartTimecode
    std::string qt_timecode;            // QuickTime:TimeCode  
    std::string qt_creation_date;       // QuickTime:CreationDate
    std::string qt_media_create_date;   // QuickTime:MediaCreateDate

    std::string mxf_start_timecode;     // MXF:StartTimecode
    std::string mxf_timecode_at_start;  // MXF:TimecodeAtStart
    std::string mxf_start_of_content;   // MXF:StartOfContent

    std::string xmp_start_timecode;     // XMP:StartTimecode
    std::string xmp_alt_timecode;       // XMP:AltTimecode
    std::string xmp_alt_timecode_time_value; // XMP:AltTimecodeTimeValue
    std::string xmp_timecode;           // XMP:TimeCode

    std::string userdata_timecode;      // UserData:TimeCode

    bool is_loaded = false;

    // Existing utility methods
    bool HasAnyAdobeProject() const {
        return !ae_project_path.empty() ||
            !premiere_win_path.empty() ||
            !premiere_mac_path.empty();
    }

    std::string GetBestProjectPath() const {
        if (!ae_project_path.empty()) return ae_project_path;
        if (!premiere_win_path.empty()) return premiere_win_path;
        if (!premiere_mac_path.empty()) return premiere_mac_path;
        return "";
    }

    // === ADD TIMECODE METHOD WITH DEBUG INFO ===
    bool HasAnyTimecode() const {
        return !qt_start_timecode.empty() || !qt_timecode.empty() ||
            !mxf_start_timecode.empty() || !mxf_timecode_at_start.empty() ||
            !xmp_start_timecode.empty() || !xmp_alt_timecode.empty() ||
            !xmp_alt_timecode_time_value.empty() ||
            !userdata_timecode.empty();
    }
};

class AdobeMetadataExtractor {
public:
    static std::unique_ptr<AdobeMetadata> ExtractAdobePaths(const std::string& file_path);

private:
    static std::string GetExifToolPath();
    static std::unordered_map<std::string, std::string> ParseExifOutput(const std::string& output);
};