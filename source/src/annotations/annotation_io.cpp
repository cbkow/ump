#include "annotation_io.h"
#include "../utils/debug_utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ump {
namespace AnnotationIO {

std::string SanitizeMediaName(const std::string& filename) {
    std::string sanitized = filename;

    // Replace invalid characters with underscore
    const std::string invalid_chars = "<>:\"/\\|?*";
    for (char& c : sanitized) {
        if (invalid_chars.find(c) != std::string::npos) {
            c = '_';
        }
    }

    return sanitized;
}

std::string GetUMPPath(const std::string& media_path) {
    fs::path path(media_path);
    fs::path parent = path.parent_path();

    // Create .ump folder in the same directory as the media file
    fs::path ump_folder = parent / ".ump";

    return ump_folder.string();
}

std::string GetNotesJSONPath(const std::string& media_path) {
    fs::path path(media_path);
    std::string media_name = SanitizeMediaName(path.filename().string());

    fs::path ump_path(GetUMPPath(media_path));
    fs::path media_folder = ump_path / media_name;
    fs::path json_path = media_folder / "notes.json";

    return json_path.string();
}

std::string GetImagesFolder(const std::string& media_path) {
    fs::path path(media_path);
    std::string media_name = SanitizeMediaName(path.filename().string());

    fs::path ump_path(GetUMPPath(media_path));
    fs::path media_folder = ump_path / media_name;
    fs::path images_folder = media_folder / "images";

    return images_folder.string();
}

std::string GenerateImageFilename(const std::string& timecode) {
    // Convert HH:MM:SS:FF to note_HH_MM_SS_FF.png
    std::string filename = "note_";
    for (char c : timecode) {
        if (c == ':') {
            filename += '_';
        } else {
            filename += c;
        }
    }
    filename += ".png";
    return filename;
}

bool CreateUMPFolder(const std::string& media_path) {
    try {
        fs::path path(media_path);
        std::string media_name = SanitizeMediaName(path.filename().string());

        fs::path ump_path(GetUMPPath(media_path));
        fs::path media_folder = ump_path / media_name;

        // Create .ump folder
        if (!fs::exists(ump_path)) {
            fs::create_directory(ump_path);

            // On Windows, set hidden attribute
            #ifdef _WIN32
            SetFileAttributesA(ump_path.string().c_str(), FILE_ATTRIBUTE_HIDDEN);
            #endif

            Debug::Log("Created .ump folder: " + ump_path.string());
        }

        // Create media-specific folder
        if (!fs::exists(media_folder)) {
            fs::create_directories(media_folder);
            Debug::Log("Created media folder: " + media_folder.string());
        }

        return true;
    }
    catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to create UMP folder: " + std::string(e.what()));
        return false;
    }
}

bool EnsureImagesFolderExists(const std::string& media_path) {
    try {
        fs::path images_folder(GetImagesFolder(media_path));

        if (!fs::exists(images_folder)) {
            fs::create_directories(images_folder);
            Debug::Log("Created images folder: " + images_folder.string());
        }

        return true;
    }
    catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to create images folder: " + std::string(e.what()));
        return false;
    }
}

bool SaveNotes(const std::vector<AnnotationNote>& notes, const std::string& media_path) {
    try {
        // Ensure folder structure exists
        if (!CreateUMPFolder(media_path)) {
            return false;
        }

        // Build JSON object
        json j;
        fs::path path(media_path);
        j["media_file"] = path.filename().string();
        j["media_type"] = "video"; // TODO: Detect type from extension

        // Current timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
        j["created"] = oss.str();

        // Add notes array
        j["notes"] = json::array();
        for (const auto& note : notes) {
            j["notes"].push_back(note);
        }

        // Write to file
        std::string json_path = GetNotesJSONPath(media_path);
        std::ofstream file(json_path);
        if (!file.is_open()) {
            Debug::Log("ERROR: Failed to open file for writing: " + json_path);
            return false;
        }

        file << j.dump(2); // Pretty print with 2-space indent
        file.close();

        Debug::Log("Saved " + std::to_string(notes.size()) + " notes to: " + json_path);
        return true;
    }
    catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to save notes: " + std::string(e.what()));
        return false;
    }
}

bool LoadNotes(std::vector<AnnotationNote>& notes, const std::string& media_path) {
    try {
        std::string json_path = GetNotesJSONPath(media_path);

        // Check if file exists
        if (!fs::exists(json_path)) {
            Debug::Log("No annotations found for media: " + media_path);
            notes.clear();
            return true; // Not an error, just no notes
        }

        // Read file
        std::ifstream file(json_path);
        if (!file.is_open()) {
            Debug::Log("ERROR: Failed to open annotations file: " + json_path);
            return false;
        }

        // Parse JSON
        json j;
        file >> j;
        file.close();

        // Extract notes array
        notes.clear();
        if (j.contains("notes") && j["notes"].is_array()) {
            for (const auto& note_json : j["notes"]) {
                AnnotationNote note = note_json.get<AnnotationNote>();
                notes.push_back(note);
            }
        }

        // Sort by timecode
        std::sort(notes.begin(), notes.end());

        Debug::Log("Loaded " + std::to_string(notes.size()) + " notes from: " + json_path);
        return true;
    }
    catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to load notes: " + std::string(e.what()));
        notes.clear();
        return false;
    }
}

void LoadNotesAsync(const std::string& media_path, LoadCallback callback) {
    // TODO: Implement async version using std::async
    // For now, call sync version
    std::vector<AnnotationNote> notes;
    bool success = LoadNotes(notes, media_path);
    callback(success, notes);
}

void SaveNotesAsync(const std::vector<AnnotationNote>& notes, const std::string& media_path) {
    // TODO: Implement async version using std::async
    // For now, call sync version
    SaveNotes(notes, media_path);
}

bool SaveScreenshot(const std::string& image_path, const unsigned char* data, int width, int height) {
    // TODO: Implement PNG saving using libpng
    // This is a placeholder - will need actual libpng implementation
    Debug::Log("SaveScreenshot called for: " + image_path);
    Debug::Log("  Size: " + std::to_string(width) + "x" + std::to_string(height));
    return true;
}

} // namespace AnnotationIO
} // namespace ump
