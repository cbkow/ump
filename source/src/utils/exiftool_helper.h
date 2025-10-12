#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

class ExifToolHelper {
public:
    struct Metadata {
        std::string ae_project_path;        // After Effects project link
        std::string premiere_win_path;      // Premiere Pro Windows path
        std::string premiere_mac_path;      // Premiere Pro Mac path

        // === TIMECODE INFORMATION ===
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

        bool has_any_timecode = false;      // Flag for quick checking

        // Additional metadata placeholder
        std::unordered_map<std::string, std::string> other_fields;
    };

    static std::unique_ptr<Metadata> ExtractMetadata(const std::string& file_path);

private:
    static std::string GetExifToolPath();
    static std::unordered_map<std::string, std::string> ParseExifOutput(const std::string& output);
};