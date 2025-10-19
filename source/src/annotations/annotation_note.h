#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace ump {

/**
 * AnnotationNote - Single annotation/note data structure
 *
 * Designed for easy JSON export to Markdown/HTML/PDF via Pandoc
 * Notes are always sorted by timecode, not creation order
 */
struct AnnotationNote {
    std::string timecode;           // HH:MM:SS:FF format (primary sort key)
    double timestamp_seconds;       // Precise position in seconds for seeking
    int frame;                      // Frame number at time of capture
    std::string image_path;         // Relative path: "images/note_HH_MM_SS_FF.png"
    std::string annotation_data;    // Future: JSON string for drawing/visual annotations (null for now)
    std::string text;               // User's note content (supports multiline)

    // Default constructor
    AnnotationNote()
        : timestamp_seconds(0.0)
        , frame(0)
    {}

    // Constructor with parameters
    AnnotationNote(const std::string& tc, double ts, int f, const std::string& img, const std::string& txt)
        : timecode(tc)
        , timestamp_seconds(ts)
        , frame(f)
        , image_path(img)
        , text(txt)
    {}

    // Comparison operator for sorting by timecode
    bool operator<(const AnnotationNote& other) const {
        return timecode < other.timecode;
    }

    // Equality operator
    bool operator==(const AnnotationNote& other) const {
        return timecode == other.timecode;
    }
};

// JSON serialization functions
inline void to_json(nlohmann::json& j, const AnnotationNote& note) {
    j = nlohmann::json{
        {"timecode", note.timecode},
        {"timestamp_seconds", note.timestamp_seconds},
        {"frame", note.frame},
        {"image", note.image_path},
        {"annotation_data", note.annotation_data.empty() ? nullptr : nlohmann::json::parse(note.annotation_data)},
        {"text", note.text}
    };
}

inline void from_json(const nlohmann::json& j, AnnotationNote& note) {
    j.at("timecode").get_to(note.timecode);
    j.at("timestamp_seconds").get_to(note.timestamp_seconds);
    j.at("frame").get_to(note.frame);
    j.at("image").get_to(note.image_path);

    // annotation_data is optional and might be null
    if (j.contains("annotation_data") && !j["annotation_data"].is_null()) {
        note.annotation_data = j["annotation_data"].dump();
    } else {
        note.annotation_data = "";
    }

    j.at("text").get_to(note.text);
}

} // namespace ump
