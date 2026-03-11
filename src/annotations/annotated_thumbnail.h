#pragma once

#include <string>

namespace qcview {
namespace Annotations {

// Renders annotation strokes onto a clean thumbnail PNG and saves the result.
// clean_png_path: path to existing clean frame capture
// annotated_png_path: output path for composited result
// annotation_data_json: JSON string from AnnotationNote::annotation_data
// Returns true on success.
bool GenerateAnnotatedThumbnail(
    const std::string& clean_png_path,
    const std::string& annotated_png_path,
    const std::string& annotation_data_json
);

// Derives the annotated path from a clean path:
// "note_01_02_03_04.png" -> "note_01_02_03_04_annotated.png"
std::string GetAnnotatedThumbnailPath(const std::string& clean_png_path);

} // namespace Annotations
} // namespace qcview
