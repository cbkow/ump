#include "annotation_exporter.h"
#include "markdown_parser.h"
#include "../utils/debug_utils.h"
#include "../utils/asset_path.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <ctime>
#include <thread>

// FFmpeg base64 encoding
extern "C" {
#include <libavutil/base64.h>
}

// libharu for PDF generation
#include <hpdf.h>

namespace qcview {
namespace Annotations {

// Helper: Render a block of text with inline code highlighting in a PDF.
// Handles word-wrapping across code/non-code segments, drawing gray background
// rects behind code spans and switching to mono font.
//
// Parameters:
//   page          - current HPDF page
//   raw_text      - text with backtick markers preserved (bold/italic already stripped)
//   base_font     - font for regular text (e.g. Helvetica)
//   code_font     - font for code spans (e.g. Courier)
//   font_size     - font size in points
//   x, y          - starting position (y is modified as lines are emitted)
//   wrap_width    - maximum line width in points
//   line_height   - vertical advance per line
static void RenderBlockWithInlineCode(
    HPDF_Page page,
    const std::string& raw_text,
    HPDF_Font base_font,
    HPDF_Font code_font,
    float font_size,
    float x, float& y,
    float wrap_width,
    float line_height)
{
    auto segments = ParseInlineCodeSegments(raw_text);
    if (segments.empty()) return;

    // Flatten segments into words, each tagged with is_code
    struct TaggedWord {
        std::string text;
        bool is_code;
    };
    std::vector<TaggedWord> words;

    for (const auto& seg : segments) {
        if (seg.is_code) {
            // Keep code spans as single "words" (don't split on spaces)
            words.push_back({seg.text, true});
        } else {
            // Split regular text into words
            std::istringstream stream(seg.text);
            std::string word;
            while (stream >> word) {
                words.push_back({word, false});
            }
        }
    }

    // Build lines by accumulating words until wrap_width is exceeded
    struct LineSegment {
        std::string text;
        bool is_code;
    };

    std::vector<std::vector<LineSegment>> lines;
    std::vector<LineSegment> current_line;
    float current_width = 0;

    for (size_t i = 0; i < words.size(); i++) {
        const auto& w = words[i];

        // Measure this word in the appropriate font
        HPDF_Page_SetFontAndSize(page, w.is_code ? code_font : base_font, font_size);
        float word_w = HPDF_Page_TextWidth(page, w.text.c_str());

        // Add space before non-first, non-code words (and not after code)
        float space_w = 0;
        if (!current_line.empty()) {
            HPDF_Page_SetFontAndSize(page, base_font, font_size);
            space_w = HPDF_Page_TextWidth(page, " ");
        }

        if (!current_line.empty() && current_width + space_w + word_w > wrap_width) {
            // Line is full, start new line
            lines.push_back(current_line);
            current_line.clear();
            current_width = 0;
            space_w = 0;
        }

        // Add space separator if needed
        if (!current_line.empty() && !w.is_code) {
            // Append space to previous segment if same type, else add new segment
            if (!current_line.back().is_code) {
                current_line.back().text += " ";
                current_width += space_w;
            } else {
                current_line.push_back({" ", false});
                current_width += space_w;
            }
        } else if (!current_line.empty() && w.is_code) {
            // Space before code span
            HPDF_Page_SetFontAndSize(page, base_font, font_size);
            space_w = HPDF_Page_TextWidth(page, " ");
            current_line.push_back({" ", false});
            current_width += space_w;
        }

        // Merge with previous segment if same type
        if (!current_line.empty() && current_line.back().is_code == w.is_code) {
            current_line.back().text += w.text;
        } else {
            current_line.push_back({w.text, w.is_code});
        }
        current_width += word_w;
    }

    if (!current_line.empty()) {
        lines.push_back(current_line);
    }

    // Render each line
    for (const auto& line : lines) {
        float cursor_x = x;

        for (const auto& seg : line) {
            HPDF_Font seg_font = seg.is_code ? code_font : base_font;
            HPDF_Page_SetFontAndSize(page, seg_font, font_size);
            float seg_w = HPDF_Page_TextWidth(page, seg.text.c_str());

            if (seg.is_code) {
                // Draw gray background rect behind code
                float pad_x = 2.0f;
                float pad_y = 1.5f;
                HPDF_Page_SetRGBFill(page, 0.9f, 0.9f, 0.9f);
                HPDF_Page_Rectangle(page,
                    cursor_x - pad_x,
                    y - font_size * 0.25f - pad_y,
                    seg_w + pad_x * 2,
                    font_size + pad_y * 2);
                HPDF_Page_Fill(page);

                // Reset fill color to black for text
                HPDF_Page_SetRGBFill(page, 0.0f, 0.0f, 0.0f);
            }

            HPDF_Page_BeginText(page);
            HPDF_Page_SetFontAndSize(page, seg_font, font_size);
            HPDF_Page_TextOut(page, cursor_x, y, seg.text.c_str());
            HPDF_Page_EndText(page);

            cursor_x += seg_w;
        }

        y -= line_height;
    }
}

AnnotationExporter::AnnotationExporter() {
}

AnnotationExporter::~AnnotationExporter() {
}

void AnnotationExporter::SetCaptureCallback(CaptureCallback callback) {
    capture_callback_ = callback;
}

void AnnotationExporter::SetProgressCallback(ProgressCallback callback) {
    progress_callback_ = callback;
}

std::string AnnotationExporter::ExportNotes(
    const std::vector<AnnotationNote>& notes,
    const ExportOptions& options
) {
    cancel_requested_ = false;
    current_progress_ = ExportProgress();
    current_progress_.total_notes = static_cast<int>(notes.size());

    if (notes.empty()) {
        SetError("No notes to export");
        return "";
    }

    if (!capture_callback_) {
        SetError("No capture callback set");
        return "";
    }

    std::string result;
    switch (options.format) {
        case ExportFormat::MARKDOWN:
            result = ExportMarkdown(notes, options);
            break;
        case ExportFormat::HTML:
            result = ExportHTML(notes, options);
            break;
        case ExportFormat::PDF:
            result = ExportPDF(notes, options);
            break;
    }

    if (!result.empty()) {
        current_progress_.is_complete = true;
        UpdateProgress(current_progress_.total_notes, current_progress_.total_notes, "Export complete");
    }

    return result;
}

void AnnotationExporter::CancelExport() {
    cancel_requested_ = true;
}

std::string AnnotationExporter::ExportMarkdown(
    const std::vector<AnnotationNote>& notes,
    const ExportOptions& options
) {
    namespace fs = std::filesystem;

    UpdateProgress(0, notes.size(), "Creating export directory...");

    // Create folder: MediaName-YYYYMMDD-HHMMSS
    std::string folder_name = SanitizeFilename(options.media_name) + "-" + GenerateTimestamp();
    fs::path export_folder = fs::path(options.output_directory) / folder_name;
    fs::path images_folder = export_folder / "images";

    try {
        fs::create_directories(images_folder);
    } catch (const std::exception& e) {
        SetError("Failed to create export directory: " + std::string(e.what()));
        return "";
    }

    // Capture all images
    for (size_t i = 0; i < notes.size(); i++) {
        if (cancel_requested_) {
            SetError("Export cancelled by user");
            return "";
        }

        const auto& note = notes[i];
        UpdateProgress(i, notes.size(), "Capturing image " + std::to_string(i + 1) + "...");

        // Generate image filename
        std::string img_filename = "note_" + SanitizeFilename(note.timecode) + ".png";
        fs::path img_path = images_folder / img_filename;

        if (!CaptureNoteImage(note, img_path.string())) {
            SetError("Failed to capture image for note at " + note.timecode);
            return "";
        }
    }

    UpdateProgress(notes.size(), notes.size(), "Generating markdown file...");

    // Generate markdown content
    std::ostringstream md;

    // Header
    md << "# " << options.media_name << "\n\n";
    md << "```\n" << options.media_path << "\n```\n\n";
    md << "**Exported:** " << GenerateTimestamp() << "\n";
    md << "**Duration:** " << std::fixed << std::setprecision(2) << options.duration << "s\n";
    md << "**Frame Rate:** " << options.frame_rate << " fps\n";
    md << "**Resolution:** " << options.width << "x" << options.height << "\n\n";
    md << "---\n\n";

    // Synopsis section (grid)
    md << "## Synopsis\n\n";
    md << "| Frame | Timecode | Note |\n";
    md << "|-------|----------|------|\n";

    for (const auto& note : notes) {
        std::string img_filename = "note_" + SanitizeFilename(note.timecode) + ".png";
        md << "| <img src=\"images/" << img_filename << "\" width=\"200\"> | ";
        md << "**" << FormatTimecode(note.timecode) << "**<br>Frame: " << note.frame << " | ";
        if (note.addressed) {
            md << "**[Addressed]**<br>";
        }
        md << qcview::Annotations::EscapeForMarkdownTable(note.text) << " |\n";
    }

    md << "\n---\n\n";

    // Full details section
    md << "## Detailed Notes\n\n";

    for (const auto& note : notes) {
        std::string img_filename = "note_" + SanitizeFilename(note.timecode) + ".png";
        md << "### " << FormatTimecode(note.timecode) << "\n\n";
        md << "**Frame:** " << note.frame << "\n\n";
        md << "<img src=\"images/" << img_filename << "\" alt=\"" << note.timecode << "\" style=\"display: block; max-width: 800px; max-height: 960px;\">\n\n";
        if (note.addressed) {
            md << "**[Addressed]**\n\n";
        }
        md << note.text << "\n\n";
        md << "---\n\n";
    }

    // Write markdown file
    std::string md_filename = folder_name + ".md";
    fs::path md_path = export_folder / md_filename;

    try {
        std::ofstream file(md_path);
        if (!file) {
            SetError("Failed to create markdown file");
            return "";
        }
        file << md.str();
        file.close();
    } catch (const std::exception& e) {
        SetError("Failed to write markdown file: " + std::string(e.what()));
        return "";
    }

    Debug::Log("Markdown export complete: " + export_folder.string());
    RevealInExplorer(export_folder.string(), true);  // Reveal folder
    return export_folder.string();
}

std::string AnnotationExporter::ExportHTML(
    const std::vector<AnnotationNote>& notes,
    const ExportOptions& options
) {
    namespace fs = std::filesystem;

    UpdateProgress(0, notes.size(), "Creating temporary images...");

    // Create temp folder for image captures
    fs::path temp_folder = fs::temp_directory_path() / ("ump_export_" + GenerateTimestamp());

    try {
        fs::create_directories(temp_folder);
    } catch (const std::exception& e) {
        SetError("Failed to create temp directory: " + std::string(e.what()));
        return "";
    }

    // Capture all images
    std::vector<std::string> temp_image_paths;
    for (size_t i = 0; i < notes.size(); i++) {
        if (cancel_requested_) {
            // Cleanup temp files
            fs::remove_all(temp_folder);
            SetError("Export cancelled by user");
            return "";
        }

        const auto& note = notes[i];
        UpdateProgress(i, notes.size(), "Capturing image " + std::to_string(i + 1) + "...");

        std::string img_filename = "note_" + SanitizeFilename(note.timecode) + ".png";
        fs::path img_path = temp_folder / img_filename;

        if (!CaptureNoteImage(note, img_path.string())) {
            fs::remove_all(temp_folder);
            SetError("Failed to capture image for note at " + note.timecode);
            return "";
        }

        temp_image_paths.push_back(img_path.string());
    }

    UpdateProgress(notes.size(), notes.size(), "Generating HTML file...");

    // Generate HTML content
    std::ostringstream html;

    // HTML header with embedded CSS
    html << "<!DOCTYPE html>\n";
    html << "<html lang=\"en\">\n";
    html << "<head>\n";
    html << "    <meta charset=\"UTF-8\">\n";
    html << "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    html << "    <title>" << options.media_name << " - Annotations</title>\n";
    html << "    <style>\n";

    // Embed GitHub Markdown CSS
    std::string css = GetGitHubMarkdownCSS();
    if (!css.empty()) {
        html << css << "\n";
    }

    // Additional custom styles
    html << "        body { max-width: 1200px; margin: 0 auto; padding: 20px; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif; }\n";
    html << "        .synopsis-grid { display: grid; grid-template-columns: 1fr 2fr; gap: 20px; margin: 20px 0; }\n";
    html << "        .synopsis-image { width: 100%; height: auto; max-height: 300px; object-fit: contain; }\n";
    html << "        .synopsis-info { display: flex; flex-direction: column; justify-content: center; }\n";
    html << "        .full-image { display: block; max-width: 800px; max-height: 960px; height: auto; object-fit: contain; margin: 20px 0; }\n";
    html << "        .note-section { page-break-inside: avoid; margin: 40px 0; }\n";
    html << "        code { background: #f6f8fa; padding: 2px 6px; border-radius: 3px; }\n";
    html << "    </style>\n";
    html << "</head>\n";
    html << "<body class=\"markdown-body\">\n";

    // Header
    html << "    <h1>" << options.media_name << "</h1>\n";
    html << "    <pre><code>" << options.media_path << "</code></pre>\n";
    html << "    <p><strong>Exported:</strong> " << GenerateTimestamp() << "</p>\n";
    html << "    <p><strong>Duration:</strong> " << std::fixed << std::setprecision(2) << options.duration << "s</p>\n";
    html << "    <p><strong>Frame Rate:</strong> " << options.frame_rate << " fps</p>\n";
    html << "    <p><strong>Resolution:</strong> " << options.width << "x" << options.height << "</p>\n";
    html << "    <hr>\n";

    // Synopsis section
    html << "    <h2>Synopsis</h2>\n";

    for (size_t i = 0; i < notes.size(); i++) {
        const auto& note = notes[i];
        std::string base64_image = EncodeImageToBase64(temp_image_paths[i]);

        html << "    <div class=\"synopsis-grid\">\n";
        html << "        <div>\n";
        html << "            <img src=\"data:image/png;base64," << base64_image << "\" class=\"synopsis-image\" alt=\"" << note.timecode << "\">\n";
        html << "        </div>\n";
        html << "        <div class=\"synopsis-info\">\n";
        html << "            <h3>" << FormatTimecode(note.timecode) << "</h3>\n";
        html << "            <p><strong>Frame:</strong> " << note.frame << "</p>\n";
        if (note.addressed) {
            html << "            <p><strong>[Addressed]</strong></p>\n";
        }
        html << "            <div>" << qcview::Annotations::MarkdownToHtml(note.text) << "</div>\n";
        html << "        </div>\n";
        html << "    </div>\n";
    }

    html << "    <hr>\n";

    // Full details section
    html << "    <h2>Detailed Notes</h2>\n";

    for (size_t i = 0; i < notes.size(); i++) {
        const auto& note = notes[i];
        std::string base64_image = EncodeImageToBase64(temp_image_paths[i]);

        html << "    <div class=\"note-section\">\n";
        html << "        <h3>" << FormatTimecode(note.timecode) << "</h3>\n";
        html << "        <p><strong>Frame:</strong> " << note.frame << "</p>\n";
        html << "        <img src=\"data:image/png;base64," << base64_image << "\" class=\"full-image\" alt=\"" << note.timecode << "\">\n";
        if (note.addressed) {
            html << "        <p><strong>[Addressed]</strong></p>\n";
        }
        html << "        <div class=\"note-text\">" << qcview::Annotations::MarkdownToHtml(note.text) << "</div>\n";
        html << "        <hr>\n";
        html << "    </div>\n";
    }

    html << "</body>\n";
    html << "</html>\n";

    // Write HTML file
    std::string html_filename = SanitizeFilename(options.media_name) + "-" + GenerateTimestamp() + ".html";
    fs::path html_path = fs::path(options.output_directory) / html_filename;

    try {
        std::ofstream file(html_path);
        if (!file) {
            fs::remove_all(temp_folder);
            SetError("Failed to create HTML file");
            return "";
        }
        file << html.str();
        file.close();
    } catch (const std::exception& e) {
        fs::remove_all(temp_folder);
        SetError("Failed to write HTML file: " + std::string(e.what()));
        return "";
    }

    // Cleanup temp files
    try {
        fs::remove_all(temp_folder);
    } catch (...) {
        // Ignore cleanup errors
    }

    Debug::Log("HTML export complete: " + html_path.string());
    RevealInExplorer(html_path.string(), false);  // Reveal file
    return html_path.string();
}

std::string AnnotationExporter::ExportPDF(
    const std::vector<AnnotationNote>& notes,
    const ExportOptions& options
) {
    namespace fs = std::filesystem;

    UpdateProgress(0, notes.size(), "Creating temporary images...");

    // Create temp folder for image captures
    fs::path temp_folder = fs::temp_directory_path() / ("ump_export_" + GenerateTimestamp());

    try {
        fs::create_directories(temp_folder);
    } catch (const std::exception& e) {
        SetError("Failed to create temp directory: " + std::string(e.what()));
        return "";
    }

    // Capture all images
    std::vector<std::string> temp_image_paths;
    for (size_t i = 0; i < notes.size(); i++) {
        if (cancel_requested_) {
            fs::remove_all(temp_folder);
            SetError("Export cancelled by user");
            return "";
        }

        const auto& note = notes[i];
        UpdateProgress(i, notes.size(), "Capturing image " + std::to_string(i + 1) + "...");

        std::string img_filename = "note_" + SanitizeFilename(note.timecode) + ".png";
        fs::path img_path = temp_folder / img_filename;

        if (!CaptureNoteImage(note, img_path.string())) {
            fs::remove_all(temp_folder);
            SetError("Failed to capture image for note at " + note.timecode);
            return "";
        }

        temp_image_paths.push_back(img_path.string());
    }

    UpdateProgress(notes.size(), notes.size(), "Generating PDF file...");

    // Create PDF using libharu
    HPDF_Doc pdf = HPDF_New(nullptr, nullptr);
    if (!pdf) {
        fs::remove_all(temp_folder);
        SetError("Failed to create PDF document");
        return "";
    }

    try {
        // Set compression mode
        HPDF_SetCompressionMode(pdf, HPDF_COMP_ALL);

        // Add metadata
        HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, (options.media_name + " - Annotations").c_str());
        HPDF_SetInfoAttr(pdf, HPDF_INFO_CREATOR, "QCView");
        HPDF_SetInfoAttr(pdf, HPDF_INFO_PRODUCER, "QCView Annotation Exporter");

        // Get default font
        HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", nullptr);
        HPDF_Font font_bold = HPDF_GetFont(pdf, "Helvetica-Bold", nullptr);
        HPDF_Font font_mono = HPDF_GetFont(pdf, "Courier", nullptr);

        // Cover page with metadata
        HPDF_Page page = HPDF_AddPage(pdf);
        HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);

        float page_width = HPDF_Page_GetWidth(page);
        float page_height = HPDF_Page_GetHeight(page);
        float margin = 50.0f;
        float y_pos = page_height - margin;
        float video_aspect_ratio = (options.height > 0) ? static_cast<float>(options.width) / options.height : 16.0f / 9.0f;

        // Title (smaller font to fit long filenames)
        HPDF_Page_SetFontAndSize(page, font_bold, 14);
        HPDF_Page_BeginText(page);
        HPDF_Page_TextOut(page, margin, y_pos, options.media_name.c_str());
        HPDF_Page_EndText(page);
        y_pos -= 24;

        // File path (smaller font, word-wrapped on path separators)
        HPDF_Page_SetFontAndSize(page, font_mono, 7);
        {
            float path_wrap_width = page_width - (margin * 2);
            std::string remaining = options.media_path;
            HPDF_Page_BeginText(page);

            while (!remaining.empty()) {
                // Find how much fits on this line
                std::string line;
                size_t pos = 0;
                while (pos < remaining.size()) {
                    // Try to break at path separators (/ or \)
                    size_t next_sep = remaining.find_first_of("/\\", pos);
                    std::string candidate;
                    if (next_sep != std::string::npos) {
                        candidate = remaining.substr(0, next_sep + 1);
                    } else {
                        candidate = remaining;
                    }

                    float text_w = HPDF_Page_TextWidth(page, candidate.c_str());
                    if (text_w > path_wrap_width && !line.empty()) {
                        break;  // Current line is full, wrap here
                    }

                    line = candidate;
                    if (next_sep != std::string::npos) {
                        pos = next_sep + 1;
                    } else {
                        break;  // No more separators, take the rest
                    }
                }

                if (line.empty()) {
                    // Single segment too long — just output it (will be clipped)
                    line = remaining;
                }

                HPDF_Page_TextOut(page, margin, y_pos, line.c_str());
                y_pos -= 12;
                remaining = remaining.substr(line.size());
            }

            HPDF_Page_EndText(page);
            y_pos -= 10;  // Spacing after path block
        }

        // Metadata
        HPDF_Page_SetFontAndSize(page, font, 12);
        HPDF_Page_BeginText(page);

        std::string exported_line = "Exported: " + GenerateTimestamp();
        HPDF_Page_TextOut(page, margin, y_pos, exported_line.c_str());
        y_pos -= 20;

        char duration_str[64];
        snprintf(duration_str, sizeof(duration_str), "Duration: %.2fs", options.duration);
        HPDF_Page_TextOut(page, margin, y_pos, duration_str);
        y_pos -= 20;

        char fps_str[64];
        snprintf(fps_str, sizeof(fps_str), "Frame Rate: %.2f fps", options.frame_rate);
        HPDF_Page_TextOut(page, margin, y_pos, fps_str);
        y_pos -= 20;

        char res_str[64];
        snprintf(res_str, sizeof(res_str), "Resolution: %dx%d", options.width, options.height);
        HPDF_Page_TextOut(page, margin, y_pos, res_str);

        HPDF_Page_EndText(page);
        y_pos -= 40;

        // Line separator
        HPDF_Page_SetLineWidth(page, 1);
        HPDF_Page_MoveTo(page, margin, y_pos);
        HPDF_Page_LineTo(page, page_width - margin, y_pos);
        HPDF_Page_Stroke(page);
        y_pos -= 40;

        // Synopsis section header
        HPDF_Page_SetFontAndSize(page, font_bold, 18);
        HPDF_Page_BeginText(page);
        HPDF_Page_TextOut(page, margin, y_pos, "Synopsis");
        HPDF_Page_EndText(page);
        y_pos -= 30;

        // Synopsis grid (thumbnails + info)
        float thumbnail_width = 200.0f;
        float thumbnail_height = thumbnail_width / video_aspect_ratio;

        // Cap thumbnail height for portrait videos to prevent overflow
        float max_thumbnail_height = 200.0f;
        if (thumbnail_height > max_thumbnail_height) {
            thumbnail_height = max_thumbnail_height;
            thumbnail_width = thumbnail_height * video_aspect_ratio;
        }

        for (size_t i = 0; i < notes.size(); i++) {
            const auto& note = notes[i];

            // Check if we need a new page
            if (y_pos - thumbnail_height - 30 < margin) {
                page = HPDF_AddPage(pdf);
                HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
                y_pos = page_height - margin;
            }

            // Load and embed image
            HPDF_Image image = HPDF_LoadPngImageFromFile(pdf, temp_image_paths[i].c_str());
            if (image) {
                // Draw thumbnail
                HPDF_Page_DrawImage(page, image, margin, y_pos - thumbnail_height,
                                   thumbnail_width, thumbnail_height);

                // Draw info next to thumbnail
                float info_x = margin + thumbnail_width + 20;
                float info_y = y_pos - 20;

                HPDF_Page_SetFontAndSize(page, font_bold, 14);
                HPDF_Page_BeginText(page);
                HPDF_Page_TextOut(page, info_x, info_y, note.timecode.c_str());
                HPDF_Page_EndText(page);
                info_y -= 25;

                HPDF_Page_SetFontAndSize(page, font, 10);
                HPDF_Page_BeginText(page);
                char frame_str[64];
                snprintf(frame_str, sizeof(frame_str), "Frame: %d", note.frame);
                HPDF_Page_TextOut(page, info_x, info_y, frame_str);
                HPDF_Page_EndText(page);
                info_y -= 25;

                // Addressed status
                if (note.addressed) {
                    HPDF_Page_SetFontAndSize(page, font_bold, 10);
                    HPDF_Page_BeginText(page);
                    HPDF_Page_TextOut(page, info_x, info_y, "[Addressed]");
                    HPDF_Page_EndText(page);
                    info_y -= 25;
                }

                // Note text (markdown-aware block rendering with inline code)
                float text_width = page_width - info_x - margin;
                auto blocks = qcview::Annotations::ParseMarkdownBlocks(note.text);
                int ol_counter = 0;

                for (const auto& block : blocks) {
                    bool is_heading = block.type == qcview::Annotations::MarkdownBlock::Type::Heading;
                    HPDF_Font block_font = is_heading ? font_bold : font;
                    float block_size = is_heading ? 11.0f : 10.0f;

                    // Build display text with prefix for list items
                    std::string display_text = block.raw_text;
                    float indent = 0;
                    if (block.type == qcview::Annotations::MarkdownBlock::Type::UnorderedListItem) {
                        ol_counter = 0;
                        indent = 10;
                        display_text = "- " + display_text;
                    } else if (block.type == qcview::Annotations::MarkdownBlock::Type::OrderedListItem) {
                        ol_counter++;
                        indent = 10;
                        display_text = std::to_string(ol_counter) + ". " + display_text;
                    } else {
                        ol_counter = 0;
                    }

                    float draw_x = info_x + indent;
                    float wrap_w = text_width - indent;
                    RenderBlockWithInlineCode(page, display_text, block_font, font_mono,
                                             block_size, draw_x, info_y, wrap_w, 15.0f);

                    // Extra spacing after headings and paragraphs
                    if (is_heading || block.type == qcview::Annotations::MarkdownBlock::Type::Paragraph) {
                        info_y -= 3;
                    }
                }
            }

            y_pos -= thumbnail_height + 20;
        }

        // Detailed notes section (one per page)
        for (size_t i = 0; i < notes.size(); i++) {
            const auto& note = notes[i];

            page = HPDF_AddPage(pdf);
            HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
            y_pos = page_height - margin;

            // Title
            HPDF_Page_SetFontAndSize(page, font_bold, 16);
            HPDF_Page_BeginText(page);
            HPDF_Page_TextOut(page, margin, y_pos, note.timecode.c_str());
            HPDF_Page_EndText(page);
            y_pos -= 30;

            // Frame number
            HPDF_Page_SetFontAndSize(page, font, 12);
            HPDF_Page_BeginText(page);
            char frame_str[64];
            snprintf(frame_str, sizeof(frame_str), "Frame: %d", note.frame);
            HPDF_Page_TextOut(page, margin, y_pos, frame_str);
            HPDF_Page_EndText(page);
            y_pos -= 30;

            // Full-size image
            HPDF_Image image = HPDF_LoadPngImageFromFile(pdf, temp_image_paths[i].c_str());
            if (image) {
                float img_width = page_width - (margin * 2);
                float img_height = img_width / video_aspect_ratio;

                // Cap image height to fit on page (leave room for header and text)
                float max_img_height = page_height - (margin * 2) - 150;  // 150pt for header/text
                if (img_height > max_img_height) {
                    img_height = max_img_height;
                    img_width = img_height * video_aspect_ratio;
                }

                HPDF_Page_DrawImage(page, image, margin, y_pos - img_height,
                                   img_width, img_height);
                y_pos -= img_height + 20;
            }

            // Addressed status
            if (note.addressed) {
                HPDF_Page_SetFontAndSize(page, font_bold, 12);
                HPDF_Page_BeginText(page);
                HPDF_Page_TextOut(page, margin, y_pos, "[Addressed]");
                HPDF_Page_EndText(page);
                y_pos -= 25;
            }

            // Note text (markdown-aware block rendering with inline code)
            float text_width = page_width - (margin * 2);
            auto blocks = qcview::Annotations::ParseMarkdownBlocks(note.text);
            int ol_counter = 0;

            for (const auto& block : blocks) {
                bool is_heading = block.type == qcview::Annotations::MarkdownBlock::Type::Heading;
                HPDF_Font block_font = is_heading ? font_bold : font;
                float block_size = is_heading ? 14.0f : 12.0f;

                // Build display text with prefix for list items
                std::string display_text = block.raw_text;
                float indent = 0;
                if (block.type == qcview::Annotations::MarkdownBlock::Type::UnorderedListItem) {
                    ol_counter = 0;
                    indent = 15;
                    display_text = "- " + display_text;
                } else if (block.type == qcview::Annotations::MarkdownBlock::Type::OrderedListItem) {
                    ol_counter++;
                    indent = 15;
                    display_text = std::to_string(ol_counter) + ". " + display_text;
                } else {
                    ol_counter = 0;
                }

                float draw_x = margin + indent;
                float wrap_w = text_width - indent;
                RenderBlockWithInlineCode(page, display_text, block_font, font_mono,
                                         block_size, draw_x, y_pos, wrap_w, 18.0f);

                // Extra spacing after headings and paragraphs
                if (is_heading || block.type == qcview::Annotations::MarkdownBlock::Type::Paragraph) {
                    y_pos -= 4;
                }
            }
        }

        // Save PDF
        std::string pdf_filename = SanitizeFilename(options.media_name) + "-" + GenerateTimestamp() + ".pdf";
        fs::path pdf_path = fs::path(options.output_directory) / pdf_filename;

        HPDF_SaveToFile(pdf, pdf_path.string().c_str());
        HPDF_Free(pdf);

        // Cleanup temp files
        try {
            fs::remove_all(temp_folder);
        } catch (...) {
            // Ignore cleanup errors
        }

        Debug::Log("PDF export complete: " + pdf_path.string());
        RevealInExplorer(pdf_path.string(), false);  // Reveal file
        return pdf_path.string();

    } catch (const std::exception& e) {
        HPDF_Free(pdf);
        fs::remove_all(temp_folder);
        SetError("PDF generation failed: " + std::string(e.what()));
        return "";
    }
}

std::string AnnotationExporter::GenerateTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y%m%d-%H%M%S");
    return oss.str();
}

std::string AnnotationExporter::SanitizeFilename(const std::string& filename) const {
    std::string result = filename;
    // Replace invalid filename characters with underscores
    const std::string invalid_chars = "\\/:*?\"<>|";
    for (char& c : result) {
        if (invalid_chars.find(c) != std::string::npos) {
            c = '_';
        }
    }
    return result;
}

std::string AnnotationExporter::EncodeImageToBase64(const std::string& image_path) const {
    // Read image file into memory
    std::ifstream file(image_path, std::ios::binary);
    if (!file) {
        return "";
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read into buffer
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    // Encode to base64 using FFmpeg's implementation
    size_t base64_size = AV_BASE64_SIZE(file_size);
    std::vector<char> base64_buffer(base64_size);

    char* result = av_base64_encode(base64_buffer.data(), base64_size, buffer.data(), file_size);
    if (!result) {
        return "";
    }

    return std::string(result);
}

std::string AnnotationExporter::GetGitHubMarkdownCSS() const {
    // Read CSS from assets/css/github-markdown.css
    std::ifstream file(GetAssetPath("assets/css/github-markdown.css"));
    if (!file) {
        Debug::Log("Warning: Could not load GitHub Markdown CSS");
        return "";
    }

    std::ostringstream css;
    css << file.rdbuf();
    return css.str();
}

std::string AnnotationExporter::FormatTimecode(const std::string& timecode) const {
    // Timecode is already formatted, just return it
    return timecode;
}

std::string AnnotationExporter::FormatTimecode(double timestamp, double frame_rate) {
    int total_frames = static_cast<int>(timestamp * frame_rate);
    int hours = total_frames / (3600 * static_cast<int>(frame_rate));
    int minutes = (total_frames / (60 * static_cast<int>(frame_rate))) % 60;
    int seconds = (total_frames / static_cast<int>(frame_rate)) % 60;
    int frames = total_frames % static_cast<int>(frame_rate);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << "_"
        << std::setw(2) << minutes << "_"
        << std::setw(2) << seconds << "_"
        << std::setw(2) << frames;
    return oss.str();
}

bool AnnotationExporter::CaptureNoteImage(
    const AnnotationNote& note,
    const std::string& output_path
) {
    if (!capture_callback_) {
        return false;
    }

    return capture_callback_(note.timestamp_seconds, note.annotation_data, output_path);
}

void AnnotationExporter::UpdateProgress(int current, int total, const std::string& operation) {
    current_progress_.current_note = current;
    current_progress_.total_notes = total;
    current_progress_.current_operation = operation;

    if (progress_callback_) {
        progress_callback_(current_progress_);
    }
}

void AnnotationExporter::SetError(const std::string& error_message) {
    current_progress_.has_error = true;
    current_progress_.error_message = error_message;
    Debug::Log("Export error: " + error_message);

    if (progress_callback_) {
        progress_callback_(current_progress_);
    }
}

void AnnotationExporter::RevealInExplorer(const std::string& path, bool is_folder) {
#ifdef _WIN32
    // Convert path to wide string
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide_path(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide_path[0], size_needed);

    if (is_folder) {
        // Open the folder
        ShellExecuteW(nullptr, L"explore", wide_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        // Select the file in Explorer using /select parameter
        std::wstring command = L"/select,\"" + wide_path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOWNORMAL);
    }
    Debug::Log("Revealed in Explorer: " + path);
#elif defined(__APPLE__)
    // macOS: use 'open' command; -R reveals (selects) the file in Finder
    std::string target = path;
    std::thread([target, is_folder]() {
        std::string cmd = is_folder
            ? "open \"" + target + "\""
            : "open -R \"" + target + "\"";
        system(cmd.c_str());
    }).detach();
    Debug::Log("Revealed in Finder: " + target);
#else
    // Linux: open parent directory (xdg-open can't select a specific file)
    std::string target = path;
    if (!is_folder) {
        std::filesystem::path fs_path(path);
        target = fs_path.parent_path().string();
    }
    std::thread([target]() {
        std::string cmd = "xdg-open \"" + target + "\"";
        system(cmd.c_str());
    }).detach();
    Debug::Log("Revealed in file browser: " + target);
#endif
}

} // namespace Annotations
} // namespace qcview
