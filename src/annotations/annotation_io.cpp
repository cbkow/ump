#include "annotation_io.h"
#include "../utils/debug_utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <future>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../../external/glfw/deps/stb_image_write.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace qcview {
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

std::string GetQCViewPath(const std::string& media_path) {
    fs::path path(media_path);
    fs::path parent = path.parent_path();

    // Create .qcview folder in the same directory as the media file
    fs::path qcview_folder = parent / ".qcview";

    return qcview_folder.string();
}

// Returns the sidecar folder, preferring .qcview but falling back to .ump for reading
static std::string GetSidecarPathWithFallback(const std::string& media_path) {
    fs::path path(media_path);
    fs::path parent = path.parent_path();

    fs::path qcview_folder = parent / ".qcview";
    if (fs::exists(qcview_folder)) {
        return qcview_folder.string();
    }

    // Fallback: check for legacy .ump folder
    fs::path ump_folder = parent / ".ump";
    if (fs::exists(ump_folder)) {
        return ump_folder.string();
    }

    // Neither exists yet - return .qcview (will be created on save)
    return qcview_folder.string();
}

std::string GetNotesJSONPath(const std::string& media_path) {
    fs::path path(media_path);
    std::string media_name = SanitizeMediaName(path.filename().string());

    fs::path sidecar_path(GetSidecarPathWithFallback(media_path));
    fs::path media_folder = sidecar_path / media_name;
    fs::path json_path = media_folder / "notes.json";

    return json_path.string();
}

std::string GetImagesFolder(const std::string& media_path) {
    if (media_path.empty()) {
        return "";
    }

    fs::path path(media_path);
    std::string media_name = SanitizeMediaName(path.filename().string());

    fs::path sidecar_path(GetSidecarPathWithFallback(media_path));
    fs::path media_folder = sidecar_path / media_name;
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

// Returns project sidecar folder, preferring .qcview but falling back to .ump
static fs::path GetProjectSidecarWithFallback(const fs::path& project_dir) {
    fs::path qcview_folder = project_dir / ".qcview";
    if (fs::exists(qcview_folder)) {
        return qcview_folder;
    }

    fs::path ump_folder = project_dir / ".ump";
    if (fs::exists(ump_folder)) {
        return ump_folder;
    }

    return qcview_folder;
}

std::string GetProjectAnnotationPath(const std::string& project_path, const std::string& timeline_name) {
    if (project_path.empty() || timeline_name.empty()) {
        return "";
    }

    fs::path proj_path(project_path);
    fs::path project_dir = proj_path.parent_path();

    // Sanitize timeline name for use as folder name
    std::string sanitized_name = SanitizeMediaName(timeline_name);

    // Build path: {project_dir}/.qcview/{timeline_name}/notes.json
    fs::path sidecar_folder = GetProjectSidecarWithFallback(project_dir);
    fs::path timeline_folder = sidecar_folder / sanitized_name;
    fs::path json_path = timeline_folder / "notes.json";

    return json_path.string();
}

std::string GetProjectImagesFolder(const std::string& project_path, const std::string& timeline_name) {
    if (project_path.empty() || timeline_name.empty()) {
        return "";
    }

    fs::path proj_path(project_path);
    fs::path project_dir = proj_path.parent_path();

    // Sanitize timeline name for use as folder name
    std::string sanitized_name = SanitizeMediaName(timeline_name);

    // Build path: {project_dir}/.qcview/{timeline_name}/images/
    fs::path sidecar_folder = GetProjectSidecarWithFallback(project_dir);
    fs::path timeline_folder = sidecar_folder / sanitized_name;
    fs::path images_folder = timeline_folder / "images";

    return images_folder.string();
}

bool CreateProjectQCViewFolder(const std::string& project_path, const std::string& timeline_name) {
    try {
        if (project_path.empty() || timeline_name.empty()) {
            Debug::Log("ERROR: Cannot create project QCView folder - empty project path or timeline name");
            return false;
        }

        fs::path proj_path(project_path);
        fs::path project_dir = proj_path.parent_path();

        // Sanitize timeline name for use as folder name
        std::string sanitized_name = SanitizeMediaName(timeline_name);

        // Build path: {project_dir}/.qcview/{timeline_name}/
        fs::path qcview_folder = project_dir / ".qcview";
        fs::path timeline_folder = qcview_folder / sanitized_name;

        // Create .qcview folder
        if (!fs::exists(qcview_folder)) {
            fs::create_directory(qcview_folder);

            // On Windows, set hidden attribute
            #ifdef _WIN32
            SetFileAttributesA(qcview_folder.string().c_str(), FILE_ATTRIBUTE_HIDDEN);
            #endif

            Debug::Log("Created project .qcview folder: " + qcview_folder.string());
        }

        // Create timeline-specific folder
        if (!fs::exists(timeline_folder)) {
            fs::create_directories(timeline_folder);
            Debug::Log("Created timeline annotation folder: " + timeline_folder.string());
        }

        return true;
    }
    catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to create project QCView folder: " + std::string(e.what()));
        return false;
    }
}

bool CreateQCViewFolder(const std::string& media_path) {
    try {
        fs::path path(media_path);
        std::string media_name = SanitizeMediaName(path.filename().string());

        fs::path qcview_path(GetQCViewPath(media_path));
        fs::path media_folder = qcview_path / media_name;

        // Create .qcview folder
        if (!fs::exists(qcview_path)) {
            fs::create_directory(qcview_path);

            // On Windows, set hidden attribute
            #ifdef _WIN32
            SetFileAttributesA(qcview_path.string().c_str(), FILE_ATTRIBUTE_HIDDEN);
            #endif

            Debug::Log("Created .qcview folder: " + qcview_path.string());
        }

        // Create media-specific folder
        if (!fs::exists(media_folder)) {
            fs::create_directories(media_folder);
            Debug::Log("Created media folder: " + media_folder.string());
        }

        return true;
    }
    catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to create QCView folder: " + std::string(e.what()));
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
        if (!CreateQCViewFolder(media_path)) {
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
    std::thread([media_path, callback]() {
        std::vector<AnnotationNote> notes;
        bool success = LoadNotes(notes, media_path);
        callback(success, notes);
    }).detach();
}

void SaveNotesAsync(const std::vector<AnnotationNote>& notes, const std::string& media_path) {
    std::thread([notes, media_path]() {
        SaveNotes(notes, media_path);
    }).detach();
}

bool SaveScreenshot(const std::string& image_path, const unsigned char* data, int width, int height) {
    if (!data || width <= 0 || height <= 0) {
        Debug::Log("SaveScreenshot: Invalid parameters");
        return false;
    }

    // Ensure parent directory exists
    std::filesystem::path path(image_path);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    int result = stbi_write_png(image_path.c_str(), width, height, 4, data, width * 4);
    if (result) {
        Debug::Log("SaveScreenshot: Saved " + std::to_string(width) + "x" +
                   std::to_string(height) + " to " + image_path);
    } else {
        Debug::Log("SaveScreenshot: Failed to write " + image_path);
    }
    return result != 0;
}

} // namespace AnnotationIO
} // namespace qcview
