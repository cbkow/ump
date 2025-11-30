#pragma once

#include <string>
#include <vector>
#include <optional>
#include "timeline_view.h"

namespace ump {

// CMX 3600 EDL Parser
// Supports standard EDL format exported from Premiere, DaVinci, Avid, etc.
class EDLParser {
public:
    struct ParseResult {
        bool success = false;
        std::string error_message;
        std::string timeline_name;
        double frame_rate = 23.976;
        bool drop_frame = false;
        std::vector<OTIOTrack> tracks;
    };

    // Parse an EDL file and return tracks
    static ParseResult Parse(const std::string& file_path);

    // Parse EDL content from string
    static ParseResult ParseString(const std::string& content, double frame_rate = 23.976);

private:
    // Timecode conversion
    struct Timecode {
        int hours = 0;
        int minutes = 0;
        int seconds = 0;
        int frames = 0;

        double ToSeconds(double fps, bool drop_frame = false) const;
        static std::optional<Timecode> Parse(const std::string& tc_str);
    };

    // EDL event (one line in the EDL)
    struct EDLEvent {
        int event_number = 0;
        std::string reel_name;
        std::string track_type;      // V, A, A1, A2, AA, etc.
        std::string edit_type;       // C (cut), D (dissolve), W (wipe)
        int transition_frames = 0;   // For dissolves/wipes

        Timecode source_in;
        Timecode source_out;
        Timecode record_in;
        Timecode record_out;

        // Metadata from comments
        std::string clip_name;
        std::string source_file;
    };

    static std::optional<EDLEvent> ParseEventLine(const std::string& line);
    static void ProcessComment(const std::string& line, EDLEvent& current_event);
    static std::string Trim(const std::string& str);
    static std::vector<std::string> Split(const std::string& str, char delimiter = ' ');
};

} // namespace ump
