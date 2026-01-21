#include "annotation_panel.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <filesystem>
#include <png.h>
#include <vector>

#define ICON_CLOSE u8"\uE5CD"

extern ImFont* font_icons;

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

    // Transparent border for docked panel (dock borders remain visible)
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // Match Inspector panel background color
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.141f, 0.141f, 0.141f, 1.0f));  // #242424

    if (!annotation_manager_) {
        ImGui::Begin("Annotations", p_open);
        ImGui::Text("No annotation manager set");
        ImGui::End();
        ImGui::PopStyleColor(2);
        return;
    }

    ImGui::Begin("Annotations", p_open);

    // Header row with icon, title, and close button
    {
        #define ICON_NOTES u8"\uE873"  // Material icon for notes/description
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_NOTES);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        ImGui::Text("Annotations");
        ImGui::PopStyleColor();

        // Close button on the right
        float button_size = ImGui::GetFontSize() + 4.0f;  // Compact size
        ImGui::SameLine(ImGui::GetWindowWidth() - button_size - ImGui::GetStyle().WindowPadding.x);
        ImVec2 button_pos = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##CloseAnnotations", ImVec2(button_size, button_size));
        bool hovered = ImGui::IsItemHovered();
        // Draw icon centered - disabled color by default, regular on hover
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImVec2 icon_size = ImGui::CalcTextSize(ICON_CLOSE);
            ImVec2 icon_pos = ImVec2(button_pos.x + (button_size - icon_size.x) / 2,
                                     button_pos.y + (button_size - icon_size.y) / 2 - 1.0f);
            ImU32 icon_col = hovered ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            ImGui::GetWindowDrawList()->AddText(icon_pos, icon_col, ICON_CLOSE);
            ImGui::PopFont();
        }
        if (clicked && p_open) {
            *p_open = false;
        }
        #undef ICON_NOTES
    }

    ImGui::Separator();

    // Check availability state - show appropriate message if disabled
    if (availability_ == AnnotationAvailability::NO_PROJECT_SAVED) {
        ImGui::Spacing();
        ImGui::TextWrapped("Annotations for timelines require a saved project.");
        ImGui::Spacing();
        ImGui::TextDisabled("Save your project first:");
        ImGui::TextDisabled("  File > Project > Save Project");
        ImGui::Spacing();
        ImGui::BeginDisabled();
        ImGui::Button("Add Note", ImVec2(-1, 0));
        ImGui::EndDisabled();
        ImGui::End();
        ImGui::PopStyleColor(2);  // Transparent border + window background
        return;
    }

    if (availability_ == AnnotationAvailability::DUAL_VIEW_DISABLED) {
        ImGui::Spacing();
        ImGui::TextDisabled("Annotations are not available");
        ImGui::TextDisabled("in Dual View comparison mode.");
        ImGui::End();
        ImGui::PopStyleColor(2);  // Transparent border + window background
        return;
    }

    RenderHeader();

    ImGui::Separator();

    // We'll use auto-layout: footer in its own child, notes list takes remaining space
    // First, render notes list in a child that takes all available space except what footer needs
    float available_height = ImGui::GetContentRegionAvail().y;

    // Reserve some minimum space for footer (just the enabled button now)
    // Scale with font (0.65 dampened)
    const float ui_scale = ImGui::GetIO().FontGlobalScale;
    const float height_scale = 1.0f + (ui_scale - 1.0f) * 0.65f;
    float footer_reserve = 50.0f * height_scale;

    // Scrollable notes list - use transparent background to show panel color
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (ImGui::BeginChild("NotesScrollRegion", ImVec2(0, available_height - footer_reserve), false)) {
        RenderNotesList();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Separator();

    // Footer in auto-sized child (expands to fit content) - use transparent background
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (ImGui::BeginChild("FooterRegion", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar)) {
        RenderFooter(accent_regular);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleColor(2);  // Transparent border + window background
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
    if (ImGui::Button("Add Note", ImVec2(-1, 0))) {
        HandleAddNote();
    }
}


void AnnotationPanel::RenderNotesList() {
    const auto& notes = annotation_manager_->GetNotes();

    // Track if we right-clicked on a note (set by RenderNote)
    right_clicked_note_timecode_.clear();

    if (notes.empty()) {
        ImGui::TextDisabled("No annotations yet");
        ImGui::TextDisabled("Click 'Add Note' to create your first annotation");
    } else {
        int note_index = 0;
        for (auto& note : annotation_manager_->GetNotes()) {
            // Use index-based ID to handle multiple notes at same timecode
            ImGui::PushID(note_index++);

            // Note: This is casting away const, which is necessary for editing
            // In production, we'd want a better pattern here
            RenderNote(const_cast<AnnotationNote&>(note));

            ImGui::PopID();

            // Add spacing between notes (no separator needed since each note has its own border)
            ImGui::Spacing();
        }
    }

    // Context menu - right-click anywhere in the list area or on a note
    if (right_clicked_note_timecode_.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("AnnotationsContextMenu");
    }

    if (!right_clicked_note_timecode_.empty()) {
        ImGui::OpenPopup("AnnotationsContextMenu");
    }

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));

    if (ImGui::BeginPopup("AnnotationsContextMenu")) {
        bool has_notes = annotation_manager_->GetNoteCount() > 0;

        if (ImGui::BeginMenu("Export")) {
            if (!has_notes) {
                ImGui::BeginDisabled();
            }
            if (ImGui::MenuItem("Markdown")) {
                if (export_callback_) export_callback_("markdown");
            }
            if (ImGui::MenuItem("HTML")) {
                if (export_callback_) export_callback_("html");
            }
            if (ImGui::MenuItem("PDF")) {
                if (export_callback_) export_callback_("pdf");
            }
            if (!has_notes) {
                ImGui::EndDisabled();
            }
            ImGui::EndMenu();
        }

        // Import requires media to be loaded
        bool has_media = !annotation_manager_->GetImagesFolder().empty();
        if (!has_media) {
            ImGui::BeginDisabled();
        }
        if (ImGui::BeginMenu("Import")) {
            if (ImGui::MenuItem("From Frame.io")) {
                if (frameio_import_callback_) frameio_import_callback_();
            }
            ImGui::EndMenu();
        }
        if (!has_media) {
            ImGui::EndDisabled();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor();
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

    const char* button_text = *annotations_enabled_ptr_ ? "Annotations Enabled" : "Annotations Disabled";
    if (ImGui::Button(button_text, ImVec2(-1, 0))) {
        *annotations_enabled_ptr_ = !(*annotations_enabled_ptr_);
        Debug::Log(*annotations_enabled_ptr_ ? "Annotations enabled for playback" : "Annotations disabled for playback");
    }

    ImGui::PopStyleColor(3);
}

void AnnotationPanel::RenderNote(AnnotationNote& note) {
    // Note: Unique ID is pushed by caller (RenderNotesList) using index

    // Padding and styling
    const float padding = 8.0f;
    const float rounding = 9.0f;

    // Get draw list for background shape
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Store cursor position before the group
    ImVec2 group_start_pos = ImGui::GetCursorScreenPos();

    ImGui::BeginGroup(); // Group all content for proper sizing

    // Add top padding
    ImGui::Dummy(ImVec2(0, padding));

    // Add left padding and begin inner content
    ImGui::Indent(padding);

    // 3-column layout: Thumbnail | Timecode+Frame | Edit+Delete buttons
    float content_width = ImGui::GetContentRegionAvail().x - padding;

    // Column widths and spacing
    const float thumbnail_width = 140.0f;  // Increased from 100.0f for better visibility
    const float button_width = 80.0f;
    const float column_spacing = 12.0f; // Spacing between columns
    const float middle_width = content_width - thumbnail_width - button_width - column_spacing * 2;

    // === COLUMN 1: Thumbnail ===
    GLuint thumbnail_id = 0;
    float thumbnail_aspect = video_aspect_ratio_;  // Default fallback
    std::string full_image_path;

    if (annotation_manager_) {
        std::string images_folder = annotation_manager_->GetImagesFolder();
        full_image_path = images_folder + "/" + note.image_path.substr(note.image_path.find_last_of('/') + 1);
        thumbnail_id = LoadThumbnail(full_image_path);

        // Use cached aspect ratio from actual image if available
        auto aspect_it = thumbnail_aspect_cache_.find(full_image_path);
        if (aspect_it != thumbnail_aspect_cache_.end()) {
            thumbnail_aspect = aspect_it->second;
        }
    }

    float thumbnail_height = thumbnail_width / thumbnail_aspect;

    if (thumbnail_id != 0) {
        // No fade on thumbnail - keep fully visible
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
        // Placeholder if no thumbnail
        ImGui::Dummy(ImVec2(thumbnail_width, thumbnail_height));
    }

    // Add spacing before next column
    ImGui::SameLine(0.0f, column_spacing);

    // === COLUMN 2: Timecode and Frame (stacked) ===
    ImGui::BeginGroup();
    ImGui::PushItemWidth(middle_width);

    // Get mono font for timecode and frame display
    ImFont* mono_font = ImGui::GetIO().Fonts->Fonts.Size > 2 ? ImGui::GetIO().Fonts->Fonts[2] : nullptr;

    // Timecode (clickable with bright accent color, mono font)
    ImVec4 timecode_color = get_bright_accent_color_callback_ ? get_bright_accent_color_callback_() : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, timecode_color);
    if (mono_font) ImGui::PushFont(mono_font);
    if (ImGui::Selectable(note.timecode.c_str(), selected_timecode_ == note.timecode, 0, ImVec2(middle_width, 0))) {
        selected_timecode_ = note.timecode;
        if (seek_callback_) {
            seek_callback_(note.timestamp_seconds);
        }
    }
    if (mono_font) ImGui::PopFont();
    ImGui::PopStyleColor();

    // Frame number (disabled text style with mono font)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::Text("Frame: %d", note.frame);
    if (mono_font) ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::PopItemWidth();
    ImGui::EndGroup();

    // Add spacing before next column
    ImGui::SameLine(0.0f, column_spacing);

    // === COLUMN 3: Edit and Delete buttons (stacked) ===
    ImGui::BeginGroup();

    const float button_height = 0;  // Auto-height based on frame padding

    // Material Icons edit icon
    #define ICON_EDIT u8"\uE3C9"

    // Get icon font if available (index 3: MaterialSymbolsSharp)
    ImFont* icon_font = ImGui::GetIO().Fonts->Fonts.Size > 3 ? ImGui::GetIO().Fonts->Fonts[3] : nullptr;

    // Check if this note is currently being edited
    bool is_currently_editing = is_editing_callback_ ? is_editing_callback_(note.timecode) : false;

    // Edit button colors (disabled if note is addressed)
    ImVec4 accent_bright = get_bright_accent_color_callback_ ? get_bright_accent_color_callback_() : ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
    ImVec4 accent_regular = ImVec4(accent_bright.x * 0.7f, accent_bright.y * 0.7f, accent_bright.z * 0.7f, accent_bright.w);
    ImVec4 accent_muted_dark = ImVec4(accent_bright.x * 0.35f, accent_bright.y * 0.35f, accent_bright.z * 0.35f, accent_bright.w * 0.7f);

    bool edit_button_enabled = !note.addressed;

    if (edit_button_enabled) {
        if (is_currently_editing) {
            // Currently editing this note - use accent color
            ImGui::PushStyleColor(ImGuiCol_Button, accent_regular);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent_regular.x * 1.2f, accent_regular.y * 1.2f, accent_regular.z * 1.2f, accent_regular.w));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent_muted_dark);
        } else {
            // Not editing - use normal colors with muted-dark for active
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent_muted_dark);
        }
    } else {
        // Disabled state - use greyed out colors
        ImVec4 disabled_color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(disabled_color.x * 0.5f, disabled_color.y * 0.5f, disabled_color.z * 0.5f, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(disabled_color.x * 0.5f, disabled_color.y * 0.5f, disabled_color.z * 0.5f, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(disabled_color.x * 0.5f, disabled_color.y * 0.5f, disabled_color.z * 0.5f, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_Text, disabled_color);
    }

    if (icon_font) ImGui::PushFont(icon_font);
    bool edit_clicked = edit_button_enabled && ImGui::Button(icon_font ? ICON_EDIT : "Edit", ImVec2(button_width, button_height));
    if (icon_font) ImGui::PopFont();

    if (edit_button_enabled) {
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PopStyleColor(4); // Pop 4 colors for disabled state
    }

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

    // Delete button (stacked below Edit)
    if (ImGui::Button("Delete", ImVec2(button_width, button_height))) {
        HandleDeleteNote(note.timecode);
    }

    #undef ICON_EDIT

    ImGui::EndGroup();

    // === Full-width Text field below ===
    // Add some spacing before text field
    ImGui::Spacing();

    char text_buffer[1024];
    strncpy(text_buffer, note.text.c_str(), sizeof(text_buffer) - 1);
    text_buffer[sizeof(text_buffer) - 1] = '\0';

    // Text field - fade down to 60% opacity if addressed
    if (note.addressed) {
        ImVec4 text_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        text_color.w = 0.6f; // 60% opacity
        ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    }

    // InputTextMultiline with WordWrap - always editable with wrapped text
    if (ImGui::InputTextMultiline("##text", text_buffer, sizeof(text_buffer),
        ImVec2(content_width, ImGui::GetTextLineHeight() * 4), ImGuiInputTextFlags_WordWrap)) {
        // Text changed - update note
        annotation_manager_->UpdateNoteText(note.timecode, text_buffer);
    }

    if (note.addressed) {
        ImGui::PopStyleColor();
    }

    // If user clicked into this text field, seek to this note's frame
    if (ImGui::IsItemActivated()) {
        selected_timecode_ = note.timecode;
        if (seek_callback_) {
            seek_callback_(note.timestamp_seconds);
        }
    }

    // Addressed checkbox (flush-left below text field, smaller regular font)
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::SetWindowFontScale(0.85f);
    bool addressed = note.addressed;
    if (ImGui::Checkbox("Addressed", &addressed)) {
        if (annotation_manager_) {
            annotation_manager_->UpdateNoteAddressed(note.timecode, addressed);
        }
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // Add bottom padding
    ImGui::Dummy(ImVec2(0, padding));

    // Unindent to close left padding
    ImGui::Unindent(padding);

    ImGui::EndGroup(); // End content group

    // Get the size of the group we just drew
    ImVec2 group_end_pos = ImGui::GetItemRectMax();
    ImVec2 group_size = ImVec2(group_end_pos.x - group_start_pos.x, group_end_pos.y - group_start_pos.y);

    // Extend the shape a few pixels on the right for better visual spacing
    const float right_extension = 8.0f;

    // Draw subtle border with rounded corners (always visible)
    draw_list->AddRect(
        group_start_pos,
        ImVec2(group_start_pos.x + group_size.x + right_extension, group_start_pos.y + group_size.y),
        IM_COL32(255, 255, 255, 15),
        rounding, 0, 1.0f);

    // If addressed, draw semi-transparent wash OVER everything
    if (note.addressed) {
        ImU32 wash_color = IM_COL32(30, 30, 30, 180);
        draw_list->AddRectFilled(
            group_start_pos,
            ImVec2(group_start_pos.x + group_size.x + right_extension, group_start_pos.y + group_size.y),
            wash_color,
            rounding);
    }

    // Right-click detection for context menu
    ImVec2 mouse_pos = ImGui::GetMousePos();
    ImVec2 note_max = ImVec2(group_start_pos.x + group_size.x + right_extension, group_start_pos.y + group_size.y);
    bool mouse_in_note = mouse_pos.x >= group_start_pos.x && mouse_pos.x <= note_max.x &&
                         mouse_pos.y >= group_start_pos.y && mouse_pos.y <= note_max.y;

    if (mouse_in_note && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        right_clicked_note_timecode_ = note.timecode;
    }
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

    // No Y-axis flip needed - ImGui expects textures in normal top-down format
    for (int y = 0; y < height; y++) {
        row_pointers[y] = &image_data[y * width * 4];
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

    // Cache the texture and aspect ratio
    thumbnail_cache_[image_path] = texture_id;
    thumbnail_aspect_cache_[image_path] = static_cast<float>(width) / static_cast<float>(height);

    Debug::Log("Loaded thumbnail: " + image_path + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

    return texture_id;
}

void AnnotationPanel::CleanupThumbnails() {
    for (auto& pair : thumbnail_cache_) {
        glDeleteTextures(1, &pair.second);
    }
    thumbnail_cache_.clear();
    thumbnail_aspect_cache_.clear();
}

} // namespace ump
