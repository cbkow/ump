#include "annotation_panel.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <filesystem>
#include <png.h>
#include <vector>

namespace ump {

AnnotationPanel::AnnotationPanel()
    : annotation_manager_(nullptr)
    , is_editing_(false)
    , annotations_enabled_ptr_(nullptr)
{
}

AnnotationPanel::~AnnotationPanel() {
    CleanupThumbnails();
}

void AnnotationPanel::Render(bool* p_open, ImVec4 accent_regular, ImVec4 accent_muted_dark) {
    if (!p_open || !*p_open) {
        return;
    }

    if (!annotation_manager_) {
        ImGui::Begin("Annotations", p_open);
        ImGui::Text("No annotation manager set");
        ImGui::End();
        return;
    }

    ImGui::Begin("Annotations", p_open, ImGuiWindowFlags_MenuBar);

    // Menu bar for import/export
    // Match main menu popup style
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
    if (ImGui::BeginMenuBar()) {
        RenderMenuBar();
        ImGui::EndMenuBar();
    }
    ImGui::PopStyleColor();

    RenderHeader();

    ImGui::Separator();

    // We'll use auto-layout: footer in its own child, notes list takes remaining space
    // First, render notes list in a child that takes all available space except what footer needs
    float available_height = ImGui::GetContentRegionAvail().y;

    // Reserve some minimum space for footer (just the enabled button now)
    float footer_reserve = 50.0f; // Reduced from 196.00

    // Scrollable notes list
    if (ImGui::BeginChild("NotesScrollRegion", ImVec2(0, available_height - footer_reserve), false)) {
        RenderNotesList();
    }
    ImGui::EndChild();

    ImGui::Separator();

    // Footer in auto-sized child (expands to fit content)
    if (ImGui::BeginChild("FooterRegion", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar)) {
        RenderFooter(accent_regular);
    }
    ImGui::EndChild();

    ImGui::End();
}

void AnnotationPanel::RenderHeader() {
    size_t note_count = annotation_manager_->GetNoteCount();
    ImGui::Text("Notes: %zu", note_count);

    if (annotation_manager_->IsLoading()) {
        ImGui::SameLine();
        ImGui::Text("Loading...");
    }

    if (annotation_manager_->IsSaving()) {
        ImGui::SameLine();
        ImGui::Text("Saving...");
    }

    // Full-width Add Note button on its own row
    if (ImGui::Button("Add Note", ImVec2(-1, 30.0f))) {
        HandleAddNote();
    }
}

void AnnotationPanel::RenderMenuBar() {
    bool has_notes = annotation_manager_->GetNoteCount() > 0;

    // Export menu
    if (ImGui::BeginMenu("Export")) {
        if (!has_notes) {
            ImGui::BeginDisabled();
        }

        if (ImGui::MenuItem("Markdown")) {
            if (export_callback_) {
                export_callback_("markdown");
            }
        }

        if (ImGui::MenuItem("CSV")) {
            if (export_callback_) {
                export_callback_("csv");
            }
        }

        if (ImGui::MenuItem("HTML")) {
            if (export_callback_) {
                export_callback_("html");
            }
        }

        if (ImGui::MenuItem("PDF")) {
            if (export_callback_) {
                export_callback_("pdf");
            }
        }

        if (!has_notes) {
            ImGui::EndDisabled();
        }

        ImGui::EndMenu();
    }

    ImGui::SameLine();

    // Import menu
    if (ImGui::BeginMenu("Import")) {
        if (ImGui::MenuItem("From Frame.io")) {
            if (frameio_import_callback_) {
                frameio_import_callback_();
            }
        }

        ImGui::EndMenu();
    }
}

void AnnotationPanel::RenderNotesList() {
    const auto& notes = annotation_manager_->GetNotes();

    if (notes.empty()) {
        ImGui::TextDisabled("No annotations yet");
        ImGui::Text("Click 'Add Note' to create your first annotation");
        return;
    }

    for (auto& note : annotation_manager_->GetNotes()) {
        // Note: This is casting away const, which is necessary for editing
        // In production, we'd want a better pattern here
        RenderNote(const_cast<AnnotationNote&>(note));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
}

void AnnotationPanel::RenderFooter(ImVec4 accent_regular) {
    if (!annotations_enabled_ptr_) return;

    // Button colors based on enabled state
    ImVec4 button_color = *annotations_enabled_ptr_ ? accent_regular : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImVec4 button_hover = *annotations_enabled_ptr_ ?
        ImVec4(accent_regular.x * 1.2f, accent_regular.y * 1.2f, accent_regular.z * 1.2f, accent_regular.w) :
        ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
    ImVec4 button_active = *annotations_enabled_ptr_ ?
        ImVec4(accent_regular.x * 0.8f, accent_regular.y * 0.8f, accent_regular.z * 0.8f, accent_regular.w) :
        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);

    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_active);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 8.0f));

    const char* button_text = *annotations_enabled_ptr_ ? "Annotations Enabled" : "Annotations Disabled";
    if (ImGui::Button(button_text, ImVec2(-1, 0))) {
        *annotations_enabled_ptr_ = !(*annotations_enabled_ptr_);
        Debug::Log(*annotations_enabled_ptr_ ? "Annotations enabled for playback" : "Annotations disabled for playback");
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void AnnotationPanel::RenderNote(AnnotationNote& note) {
    ImGui::PushID(note.timecode.c_str());

    // Timecode (clickable header with bright accent color)
    ImVec4 timecode_color = get_bright_accent_color_callback_ ? get_bright_accent_color_callback_() : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, timecode_color);
    if (ImGui::Selectable(note.timecode.c_str(), selected_timecode_ == note.timecode)) {
        selected_timecode_ = note.timecode;
        if (seek_callback_) {
            seek_callback_(note.timestamp_seconds);
        }
    }
    ImGui::PopStyleColor();

    // Frame number (disabled text style with mono font)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImFont* mono_font = ImGui::GetIO().Fonts->Fonts.Size > 2 ? ImGui::GetIO().Fonts->Fonts[2] : nullptr;
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::Text("Frame: %d", note.frame);
    if (mono_font) ImGui::PopFont();
    ImGui::PopStyleColor();

    // Thumbnail preview
    if (annotation_manager_) {
        std::string images_folder = annotation_manager_->GetImagesFolder();
        std::string full_image_path = images_folder + "/" + note.image_path.substr(note.image_path.find_last_of('/') + 1);

        GLuint thumbnail_id = LoadThumbnail(full_image_path);
        if (thumbnail_id != 0) {
            // Display thumbnail with fixed width, height auto-calculated
            float thumbnail_width = ImGui::GetContentRegionAvail().x;
            float aspect_ratio = 16.0f / 9.0f; // Assume 16:9, will be corrected when we load actual dimensions
            float thumbnail_height = thumbnail_width / aspect_ratio;

            ImGui::Image((void*)(intptr_t)thumbnail_id, ImVec2(thumbnail_width, thumbnail_height));

            // Single-click to navigate to frame
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_timecode_ = note.timecode;
                if (seek_callback_) {
                    seek_callback_(note.timestamp_seconds);
                }
            }

            // Show tooltip on hover
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to navigate to this frame");
            }
        } else {
            ImGui::Text("[Screenshot: %s]", note.image_path.c_str());
        }
    }

    // Text field (editable)
    char text_buffer[1024];
    strncpy(text_buffer, note.text.c_str(), sizeof(text_buffer) - 1);
    text_buffer[sizeof(text_buffer) - 1] = '\0';

    if (ImGui::InputTextMultiline("##text", text_buffer, sizeof(text_buffer),
        ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4))) {
        // Text changed - update note
        annotation_manager_->UpdateNoteText(note.timecode, text_buffer);
    }

    // Edit and Delete buttons side by side
    float button_height = 30.0f;
    float available_width = ImGui::GetContentRegionAvail().x;
    float button_width = (available_width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // Material Icons edit icon
    #define ICON_EDIT u8"\uE3C9"

    // Get icon font if available (index 3: MaterialSymbolsSharp)
    ImFont* icon_font = ImGui::GetIO().Fonts->Fonts.Size > 3 ? ImGui::GetIO().Fonts->Fonts[3] : nullptr;

    // Check if this note is currently being edited
    bool is_currently_editing = is_editing_callback_ ? is_editing_callback_(note.timecode) : false;

    // Edit button - use regular accent if editing, muted-dark if not
    ImVec4 accent_bright = get_bright_accent_color_callback_ ? get_bright_accent_color_callback_() : ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
    // Convert bright accent to regular by reducing intensity
    ImVec4 accent_regular = ImVec4(accent_bright.x * 0.7f, accent_bright.y * 0.7f, accent_bright.z * 0.7f, accent_bright.w);
    ImVec4 accent_muted_dark = ImVec4(accent_bright.x * 0.35f, accent_bright.y * 0.35f, accent_bright.z * 0.35f, accent_bright.w * 0.7f);

    ImVec4 button_color = is_currently_editing ? accent_regular : accent_muted_dark;
    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(button_color.x * 1.2f, button_color.y * 1.2f, button_color.z * 1.2f, button_color.w));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(button_color.x * 0.8f, button_color.y * 0.8f, button_color.z * 0.8f, button_color.w));

    if (icon_font) ImGui::PushFont(icon_font);
    bool edit_clicked = ImGui::Button(icon_font ? ICON_EDIT : "Edit", ImVec2(button_width, button_height));
    if (icon_font) ImGui::PopFont();

    ImGui::PopStyleColor(3);

    if (edit_clicked) {
        if (is_currently_editing) {
            // Already editing this note - save and exit
            if (exit_edit_mode_callback_) {
                exit_edit_mode_callback_();
            }
        } else {
            // Enter edit mode for this note
            // Ensure annotations are enabled when entering edit mode
            if (annotations_enabled_ptr_ && !(*annotations_enabled_ptr_)) {
                *annotations_enabled_ptr_ = true;
                Debug::Log("Annotations auto-enabled for editing");
            }

            if (enter_edit_mode_callback_) {
                enter_edit_mode_callback_(note.timecode, note.timestamp_seconds, note.frame, note.annotation_data);
            }
        }
    }

    ImGui::SameLine();

    // Delete button
    if (ImGui::Button("Delete", ImVec2(button_width, button_height))) {
        HandleDeleteNote(note.timecode);
    }

    #undef ICON_EDIT

    ImGui::PopID();
}

void AnnotationPanel::HandleAddNote() {
    if (!annotation_manager_) {
        Debug::Log("Cannot add note: No annotation manager");
        return;
    }

    if (!capture_callback_ || !get_state_callback_) {
        Debug::Log("Cannot add note: Missing callbacks");
        return;
    }

    // Get current video state
    double timestamp = 0.0;
    std::string timecode = "";
    int frame = 0;
    get_state_callback_(timestamp, timecode, frame);

    Debug::Log("Adding note at timecode: " + timecode + ", frame: " + std::to_string(frame));

    // Get the images folder from annotation manager
    std::string images_folder = annotation_manager_->GetImagesFolder();
    if (images_folder.empty()) {
        Debug::Log("Cannot add note: No media loaded");
        return;
    }

    // Ensure images folder exists
    namespace fs = std::filesystem;
    try {
        if (!fs::exists(images_folder)) {
            fs::create_directories(images_folder);
            Debug::Log("Created images folder: " + images_folder);
        }
    } catch (const std::exception& e) {
        Debug::Log("Failed to create images folder: " + std::string(e.what()));
        return;
    }

    // Generate filename from timecode: note_HH_MM_SS_FF.png
    std::string filename = "note_" + timecode;
    // Replace colons with underscores for filename
    for (size_t i = 0; i < filename.length(); i++) {
        if (filename[i] == ':') {
            filename[i] = '_';
        }
    }
    filename += ".png";

    // Capture screenshot
    bool screenshot_success = capture_callback_(images_folder, filename);

    if (!screenshot_success) {
        Debug::Log("Failed to capture screenshot for note");
        return;
    }

    // Add note to annotation manager (it will generate image path internally)
    annotation_manager_->AddNote(timestamp, timecode, frame, "");

    Debug::Log("Note added successfully at " + timecode);
}

void AnnotationPanel::HandleDeleteNote(const std::string& timecode) {
    // TODO: Add confirmation dialog
    annotation_manager_->DeleteNote(timecode);
    if (selected_timecode_ == timecode) {
        selected_timecode_.clear();
    }
}

void AnnotationPanel::SetSelectedNote(const std::string& timecode) {
    selected_timecode_ = timecode;
}

GLuint AnnotationPanel::LoadThumbnail(const std::string& image_path) {
    // Check cache first
    auto it = thumbnail_cache_.find(image_path);
    if (it != thumbnail_cache_.end()) {
        return it->second;
    }

    // Check if file exists
    namespace fs = std::filesystem;
    if (!fs::exists(image_path)) {
        Debug::Log("Thumbnail not found: " + image_path);
        return 0;
    }

    // Open PNG file
    FILE* fp = fopen(image_path.c_str(), "rb");
    if (!fp) {
        Debug::Log("Failed to open thumbnail: " + image_path);
        return 0;
    }

    // Read PNG header
    png_byte header[8];
    fread(header, 1, 8, fp);
    if (png_sig_cmp(header, 0, 8)) {
        fclose(fp);
        Debug::Log("Not a valid PNG file: " + image_path);
        return 0;
    }

    // Initialize PNG structures
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        fclose(fp);
        return 0;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(fp);
        return 0;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        return 0;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    // Convert to RGBA8
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    // Allocate image buffer
    std::vector<png_bytep> row_pointers(height);
    std::vector<png_byte> image_data(width * height * 4);

    // Flip Y-axis for OpenGL texture coordinates
    // OpenGL expects bottom row first, but PNG has top row first
    for (int y = 0; y < height; y++) {
        row_pointers[height - 1 - y] = &image_data[y * width * 4];
    }

    png_read_image(png_ptr, row_pointers.data());
    png_read_end(png_ptr, nullptr);

    // Clean up PNG structures
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(fp);

    // Create OpenGL texture
    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data.data());

    // Cache the texture
    thumbnail_cache_[image_path] = texture_id;

    Debug::Log("Loaded thumbnail: " + image_path + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

    return texture_id;
}

void AnnotationPanel::CleanupThumbnails() {
    for (auto& pair : thumbnail_cache_) {
        glDeleteTextures(1, &pair.second);
    }
    thumbnail_cache_.clear();
}

} // namespace ump
