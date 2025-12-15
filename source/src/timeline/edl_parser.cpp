#include "edl_parser.h"
#include "../utils/debug_utils.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace ump {

// ============================================================================
// Timecode Implementation
// ============================================================================

double EDLParser::Timecode::ToSeconds(double fps, bool drop_frame) const {
    // For drop frame (29.97 DF), 2 frames are "dropped" every minute except every 10th minute
    // This keeps timecode synchronized with real-time clock
    if (drop_frame && (std::abs(fps - 29.97) < 0.1 || std::abs(fps - 30.0) < 0.1)) {
        // Calculate total nominal frames first (as if non-drop frame)
        int total_minutes = hours * 60 + minutes;

        // Calculate dropped frames:
        // - 2 frames dropped per minute
        // - But NOT on the 10th minute (00, 10, 20, 30, 40, 50)
        int dropped_frames = 2 * (total_minutes - (total_minutes / 10));

        // Total actual frames
        double actual_fps = 30000.0 / 1001.0;  // Exact 29.97
        int total_frames = hours * 30 * 60 * 60 +
                          minutes * 30 * 60 +
                          seconds * 30 +
                          frames -
                          dropped_frames;

        return total_frames / actual_fps;
    }

    // Non-drop frame: simple conversion
    double total_frames = hours * 3600.0 * fps +
                         minutes * 60.0 * fps +
                         seconds * fps +
                         frames;
    return total_frames / fps;
}

std::optional<EDLParser::Timecode> EDLParser::Timecode::Parse(const std::string& tc_str) {
    // Format: HH:MM:SS:FF or HH;MM;SS;FF (drop frame uses semicolons)
    std::regex tc_regex(R"((\d{2})[:;](\d{2})[:;](\d{2})[:;](\d{2}))");
    std::smatch match;

    if (!std::regex_match(tc_str, match, tc_regex)) {
        return std::nullopt;
    }

    Timecode tc;
    tc.hours = std::stoi(match[1].str());
    tc.minutes = std::stoi(match[2].str());
    tc.seconds = std::stoi(match[3].str());
    tc.frames = std::stoi(match[4].str());

    return tc;
}

// ============================================================================
// Helper Functions
// ============================================================================

std::string EDLParser::Trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> EDLParser::Split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string current;

    for (char c : str) {
        if (c == delimiter) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

// ============================================================================
// Event Line Parsing
// ============================================================================

std::optional<EDLParser::EDLEvent> EDLParser::ParseEventLine(const std::string& line) {
    // CMX 3600 format:
    // 001  AX       V     C        00:00:00:00 00:00:10:00 01:00:00:00 01:00:10:00
    // Event Reel    Track Edit     SrcIn       SrcOut      RecIn       RecOut

    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '*' || trimmed[0] == '#') {
        return std::nullopt; // Comment or empty line
    }

    // Check if line starts with a number (event number)
    if (!std::isdigit(trimmed[0])) {
        return std::nullopt;
    }

    // Split by whitespace
    auto tokens = Split(trimmed, ' ');
    if (tokens.size() < 8) {
        return std::nullopt; // Not enough fields
    }

    EDLEvent event;

    // Parse event number
    try {
        event.event_number = std::stoi(tokens[0]);
    } catch (...) {
        return std::nullopt;
    }

    // Reel name
    event.reel_name = tokens[1];

    // Track type (V, A, A1, A2, AA, B, etc.)
    event.track_type = tokens[2];

    // Edit type (C=cut, D=dissolve, W=wipe)
    event.edit_type = tokens[3];

    // Check for transition duration (for dissolves)
    size_t tc_start_idx = 4;
    if (event.edit_type == "D" || event.edit_type == "W") {
        // Next token might be transition frame count
        if (tokens.size() > 8) {
            try {
                event.transition_frames = std::stoi(tokens[4]);
                tc_start_idx = 5;
            } catch (...) {
                // Not a number, assume it's the timecode
            }
        }
    }

    // Parse timecodes (need at least 4 more tokens)
    if (tokens.size() < tc_start_idx + 4) {
        return std::nullopt;
    }

    auto src_in = Timecode::Parse(tokens[tc_start_idx]);
    auto src_out = Timecode::Parse(tokens[tc_start_idx + 1]);
    auto rec_in = Timecode::Parse(tokens[tc_start_idx + 2]);
    auto rec_out = Timecode::Parse(tokens[tc_start_idx + 3]);

    if (!src_in || !src_out || !rec_in || !rec_out) {
        return std::nullopt;
    }

    event.source_in = *src_in;
    event.source_out = *src_out;
    event.record_in = *rec_in;
    event.record_out = *rec_out;

    return event;
}

void EDLParser::ProcessComment(const std::string& line, EDLEvent& current_event) {
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] != '*') {
        return;
    }

    // Remove leading * and spaces
    std::string comment = Trim(trimmed.substr(1));

    // Common comment patterns:
    // * FROM CLIP NAME: filename.mov
    // * SOURCE FILE: /path/to/file.mov
    // * CLIP NAME: name
    // *FROM CLIP NAME:  filename.mov (note double space sometimes)

    // Case-insensitive matching
    std::string upper_comment = comment;
    std::transform(upper_comment.begin(), upper_comment.end(), upper_comment.begin(), ::toupper);

    if (upper_comment.find("FROM CLIP NAME:") != std::string::npos ||
        upper_comment.find("CLIP NAME:") != std::string::npos) {
        size_t colon_pos = comment.find(':');
        if (colon_pos != std::string::npos) {
            current_event.clip_name = Trim(comment.substr(colon_pos + 1));
        }
    }
    else if (upper_comment.find("SOURCE FILE:") != std::string::npos) {
        size_t colon_pos = comment.find(':');
        if (colon_pos != std::string::npos) {
            current_event.source_file = Trim(comment.substr(colon_pos + 1));
        }
    }
}

// ============================================================================
// Main Parse Functions
// ============================================================================

EDLParser::ParseResult EDLParser::Parse(const std::string& file_path) {
    ParseResult result;

    std::ifstream file(file_path);
    if (!file.is_open()) {
        result.error_message = "Failed to open EDL file: " + file_path;
        Debug::Log(result.error_message);
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    // Default to 24fps, but try to detect from file
    return ParseString(buffer.str(), 24.0);
}

EDLParser::ParseResult EDLParser::ParseString(const std::string& content, double frame_rate) {
    ParseResult result;
    result.frame_rate = frame_rate;

    std::istringstream stream(content);
    std::string line;

    std::vector<EDLEvent> events;
    EDLEvent* current_event = nullptr;

    while (std::getline(stream, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;

        // Check for TITLE line
        if (trimmed.find("TITLE:") == 0) {
            result.timeline_name = Trim(trimmed.substr(6));
            continue;
        }

        // Check for FCM (Frame Code Mode) line
        if (trimmed.find("FCM:") == 0) {
            std::string fcm = Trim(trimmed.substr(4));
            std::transform(fcm.begin(), fcm.end(), fcm.begin(), ::toupper);
            result.drop_frame = (fcm.find("DROP") != std::string::npos);

            // Try to detect frame rate from FCM
            if (fcm.find("30") != std::string::npos || fcm.find("29.97") != std::string::npos) {
                result.frame_rate = result.drop_frame ? 29.97 : 30.0;
            } else if (fcm.find("25") != std::string::npos) {
                result.frame_rate = 25.0;
            } else if (fcm.find("23.98") != std::string::npos) {
                result.frame_rate = 23.976;
            } else if (fcm.find("24") != std::string::npos) {
                result.frame_rate = 24.0;
            }
            continue;
        }

        // Try to parse as event line
        auto event_opt = ParseEventLine(trimmed);
        if (event_opt) {
            events.push_back(*event_opt);
            current_event = &events.back();
            continue;
        }

        // Process comments (metadata for current event)
        if (current_event && trimmed[0] == '*') {
            ProcessComment(trimmed, *current_event);
        }
    }

    if (events.empty()) {
        result.error_message = "No valid events found in EDL";
        Debug::Log(result.error_message);
        return result;
    }

    // Convert events to tracks
    // Group by track type (V, A, A1, A2, etc.)
    std::map<std::string, std::vector<EDLEvent*>> events_by_track;
    for (auto& event : events) {
        events_by_track[event.track_type].push_back(&event);
    }

    // Find minimum record_in time to normalize timeline to start at 0
    // (EDLs often start at 01:00:00:00 for broadcast)
    double min_record_in = std::numeric_limits<double>::max();
    for (const auto& event : events) {
        double rec_in = event.record_in.ToSeconds(result.frame_rate, result.drop_frame);
        if (rec_in < min_record_in) {
            min_record_in = rec_in;
        }
    }
    if (min_record_in == std::numeric_limits<double>::max()) {
        min_record_in = 0.0;
    }
    Debug::Log("EDL timeline offset: " + std::to_string(min_record_in) + "s (normalizing to 0)");

    // Create tracks
    int video_track_num = 0;
    int audio_track_num = 0;

    for (auto& [track_type, track_events] : events_by_track) {
        OTIOTrack track;

        bool is_video = (track_type == "V" || track_type == "B");
        track.is_video = is_video;

        if (is_video) {
            video_track_num++;
            track.id = "V" + std::to_string(video_track_num);
            track.name = "V" + std::to_string(video_track_num);
            track.z_index = video_track_num;
        } else {
            audio_track_num++;
            track.id = track_type.empty() ? ("A" + std::to_string(audio_track_num)) : track_type;
            track.name = track.id;
            track.z_index = 0;
        }

        track.visible = true;
        track.muted = false;

        // Sort events by record in time
        std::sort(track_events.begin(), track_events.end(),
                  [&](const EDLEvent* a, const EDLEvent* b) {
                      return a->record_in.ToSeconds(result.frame_rate, result.drop_frame) <
                             b->record_in.ToSeconds(result.frame_rate, result.drop_frame);
                  });

        // Convert events to clips
        for (const auto* event : track_events) {
            double rec_in = event->record_in.ToSeconds(result.frame_rate, result.drop_frame) - min_record_in;
            double rec_out = event->record_out.ToSeconds(result.frame_rate, result.drop_frame) - min_record_in;
            double src_in = event->source_in.ToSeconds(result.frame_rate, result.drop_frame);
            double src_out = event->source_out.ToSeconds(result.frame_rate, result.drop_frame);

            // Check if this is a black/filler clip (BL reel)
            bool is_black = (event->reel_name == "BL" || event->reel_name == "BLACK" ||
                            event->reel_name == "AX" && event->clip_name.empty());

            // Create clip (or gap for black)
            OTIOClip clip;
            clip.id = "clip_" + std::to_string(event->event_number);

            if (is_black) {
                // Black/filler becomes a gap
                clip.name = "Black";
                clip.is_gap = true;
                clip.file_path = "";
            } else {
                clip.name = event->clip_name.empty() ? event->reel_name : event->clip_name;
                clip.file_path = event->source_file.empty() ? event->clip_name : event->source_file;
                clip.is_gap = false;
            }

            clip.start_time = rec_in;
            clip.duration = rec_out - rec_in;
            clip.source_in = src_in;
            clip.source_out = src_out;

            // EDL assumes source and timeline use same frame rate
            // This is a hint - actual source fps will be confirmed when media is probed
            clip.source_fps = result.frame_rate;

            // Handle transitions
            if (event->edit_type == "D" && event->transition_frames > 0) {
                clip.has_fade_in = true;
                clip.fade_in_duration = event->transition_frames / result.frame_rate;
            }

            track.clips.push_back(clip);
        }

        result.tracks.push_back(track);
    }

    result.success = true;
    Debug::Log("EDL parsed: " + result.timeline_name +
               " (" + std::to_string(video_track_num) + " video, " +
               std::to_string(audio_track_num) + " audio tracks, " +
               std::to_string(events.size()) + " events)");

    return result;
}

} // namespace ump
