#pragma once

#include "annotation_note.h"
#include <string>
#include <vector>
#include <functional>

namespace ump {
namespace AnnotationIO {

/**
 * AnnotationIO - Async file I/O for annotations
 *
 * Handles creating .ump folder structure and reading/writing JSON
 * All operations are async to avoid blocking playback
 */

// Path helpers
std::string GetUMPPath(const std::string& media_path);
std::string GetNotesJSONPath(const std::string& media_path);
std::string GetImagesFolder(const std::string& media_path);
std::string SanitizeMediaName(const std::string& filename);
std::string GenerateImageFilename(const std::string& timecode);

// Folder management
bool CreateUMPFolder(const std::string& media_path);
bool EnsureImagesFolderExists(const std::string& media_path);

// JSON I/O (sync versions for now, will add async later)
bool SaveNotes(const std::vector<AnnotationNote>& notes, const std::string& media_path);
bool LoadNotes(std::vector<AnnotationNote>& notes, const std::string& media_path);

// Async versions (future)
using LoadCallback = std::function<void(bool success, const std::vector<AnnotationNote>&)>;
void LoadNotesAsync(const std::string& media_path, LoadCallback callback);
void SaveNotesAsync(const std::vector<AnnotationNote>& notes, const std::string& media_path);

// Screenshot save
bool SaveScreenshot(const std::string& image_path, const unsigned char* data, int width, int height);

} // namespace AnnotationIO
} // namespace ump
