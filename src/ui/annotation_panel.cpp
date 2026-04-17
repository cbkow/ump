#include "annotation_panel.h"
#include "../app/ui_scale.h"
#include "../app/app_ui_macros.h"
#include "../annotations/annotation_toolbar.h"
#include "../annotations/viewport_annotator.h"
#include "../annotations/annotated_thumbnail.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <imgui_markdown.h>
#include <algorithm>
#include <filesystem>
#include <png.h>
#include <vector>

#ifdef QCVIEW_USE_VULKAN
#include "../gpu/vulkan_texture_pool.h"
static inline ImTextureID ThumbHandleToImTexture(uintptr_t id) {
    if (id == 0) return ImTextureID{};
    return qcview::VulkanTexturePool::Instance().GetImTextureID(static_cast<uint64_t>(id));
}
#elif defined(QCVIEW_USE_METAL)
#include "annotation_thumbnail_metal.h"
// Metal: stored handle is a retained id<MTLTexture>* — use it directly.
static inline ImTextureID ThumbHandleToImTexture(uintptr_t id) {
    return (ImTextureID)id;
}
#else
static inline ImTextureID ThumbHandleToImTexture(uintptr_t id) {
    return (ImTextureID)(intptr_t)id;
}
#endif

#define ICON_CLOSE u8"\uE5CD"
#define ICON_NOTE           "\xEE\xA0\xB6"   // U+E836
#define ICON_NOTE_SELECTED  "\xEE\xA0\xB7"   // U+E837

ImVec4 GetWindowsAccentColor();

extern ImFont* font_icons;
extern ImFont* font_bold;
extern ImFont* font_italic;
extern ImFont* font_mono;

// Custom markdown format callback: uses bold/italic fonts and SetWindowFontScale
// for headings. We handle HEADING and EMPHASIS ourselves to avoid the default's
// extra NewLine/Separator padding.
static void MarkdownFormatCallback(const ImGui::MarkdownFormatInfo& info, bool start) {
    switch (info.type) {
        case ImGui::MarkdownFormatType::HEADING:
            if (start) {
                if (font_bold) ImGui::PushFont(font_bold);
                switch (info.level) {
                    case 1: ImGui::SetWindowFontScale(1.3f); break;
                    case 2: ImGui::SetWindowFontScale(1.15f); break;
                    default: ImGui::SetWindowFontScale(1.05f); break;
                }
            } else {
                ImGui::SetWindowFontScale(1.0f);
                if (font_bold) ImGui::PopFont();
            }
            break;
        case ImGui::MarkdownFormatType::EMPHASIS:
            if (start) {
                if (info.level == 1) {
                    // Italic (*text*)
                    if (font_italic) ImGui::PushFont(font_italic);
                } else {
                    // Bold (**text**)
                    if (font_bold) ImGui::PushFont(font_bold);
                }
            } else {
                if (info.level == 1) {
                    if (font_italic) ImGui::PopFont();
                } else {
                    if (font_bold) ImGui::PopFont();
                }
            }
            break;
        default:
            ImGui::defaultMarkdownFormatCallback(info, start);
            break;
    }
}

// Renders a single line that may contain `backtick` code spans.
// Code spans are drawn with font_mono + a subtle rounded background.
// Non-code text is rendered as plain ImGui text (no markdown processing).
static void RenderLineWithInlineCode(const char* line_start, const char* line_end) {
    const char* pos = line_start;
    bool first_segment = true;

    while (pos < line_end) {
        const char* tick = static_cast<const char*>(memchr(pos, '`', line_end - pos));
        if (!tick) {
            if (pos < line_end) {
                if (!first_segment) ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(pos, line_end);
            }
            break;
        }

        // Normal text before the opening backtick
        if (tick > pos) {
            if (!first_segment) ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(pos, tick);
            first_segment = false;
        }

        // Find closing backtick
        const char* close = static_cast<const char*>(memchr(tick + 1, '`', line_end - (tick + 1)));
        if (!close) {
            // Unmatched backtick — render the rest as plain text
            if (!first_segment) ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(tick, line_end);
            break;
        }

        // Render the code span
        const char* code_start = tick + 1;
        if (code_start < close) {
            if (!first_segment) ImGui::SameLine(0, 0);

            const float pad_x = S(3.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad_x);

            if (font_mono) ImGui::PushFont(font_mono);
            ImVec2 text_pos = ImGui::GetCursorScreenPos();
            ImVec2 text_size = ImGui::CalcTextSize(code_start, close);
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(text_pos.x - pad_x, text_pos.y - 1),
                ImVec2(text_pos.x + text_size.x + pad_x, text_pos.y + text_size.y + 1),
                IM_COL32(255, 255, 255, 20), 3.0f);
            ImGui::TextUnformatted(code_start, close);
            if (font_mono) ImGui::PopFont();

            // Right padding before next segment
            ImGui::SameLine(0, pad_x);
            first_segment = false;
        }

        pos = close + 1;
    }
}

// Renders note text with both markdown formatting and inline code support.
// Lines without backticks are batched and passed to ImGui::Markdown().
// Lines with backticks are rendered directly with inline code spans.
static void RenderMarkdownWithCode(const std::string& text, const ImGui::MarkdownConfig& config) {
    if (text.empty()) return;

    // Fast path: no backticks at all — pure markdown
    if (text.find('`') == std::string::npos) {
        ImGui::Markdown(text.c_str(), text.length(), config);
        return;
    }

    const char* start = text.c_str();
    const char* end = start + text.length();
    const char* batch_start = nullptr;
    const char* pos = start;

    while (pos <= end) {
        // Find line boundaries
        const char* line_start = pos;
        const char* line_end = static_cast<const char*>(memchr(pos, '\n', end - pos));
        if (!line_end) line_end = end;

        bool has_backtick = (memchr(line_start, '`', line_end - line_start) != nullptr);

        if (!has_backtick) {
            // Accumulate into markdown batch
            if (!batch_start) batch_start = line_start;
        } else {
            // Flush any accumulated markdown batch first
            if (batch_start) {
                // batch runs from batch_start to line_start (which includes trailing newline of prev line)
                size_t batch_len = line_start - batch_start;
                if (batch_len > 0) {
                    ImGui::Markdown(batch_start, batch_len, config);
                }
                batch_start = nullptr;
            }
            // Render this line with inline code handling
            RenderLineWithInlineCode(line_start, line_end);
        }

        pos = line_end + 1; // skip past newline
    }

    // Flush remaining markdown batch
    if (batch_start) {
        size_t batch_len = end - batch_start;
        if (batch_len > 0) {
            ImGui::Markdown(batch_start, batch_len, config);
        }
    }
}

namespace qcview {

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
        #define ICON_DRAW u8"\uE22B"  // Material icon for draw/brush
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_DRAW);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Annotations");
        if (font_bold) ImGui::PopFont();
        ImGui::PopStyleColor();

        // Close button on the right
        float button_size = ImGui::GetFontSize() + S(4.0f);  // Compact size
        ImGui::SameLine(ImGui::GetWindowWidth() - button_size - ImGui::GetStyle().WindowPadding.x);
        ImVec2 button_pos = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##CloseAnnotations", ImVec2(button_size, button_size));
        bool hovered = ImGui::IsItemHovered();
        // Draw icon centered - disabled color by default, regular on hover
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImVec2 icon_size = ImGui::CalcTextSize(ICON_CLOSE);
            ImVec2 icon_pos = ImVec2(button_pos.x + (button_size - icon_size.x) / 2,
                                     button_pos.y + (button_size - icon_size.y) / 2 - S(1.0f));
            ImU32 icon_col = hovered ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            ImGui::GetWindowDrawList()->AddText(icon_pos, icon_col, ICON_CLOSE);
            ImGui::PopFont();
        }
        if (clicked && p_open) {
            *p_open = false;
        }
        #undef ICON_DRAW
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

    if (availability_ == AnnotationAvailability::PLAYLIST_DISABLED) {
        ImGui::Spacing();
        ImGui::TextDisabled("Annotations are not available");
        ImGui::TextDisabled("in Playlist mode.");
        ImGui::End();
        ImGui::PopStyleColor(2);  // Transparent border + window background
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.25f, 0.50f));
    if (ImGui::BeginTabBar("AnnotationTabs")) {
        if (ImGui::BeginTabItem("Edit")) {
            RenderHeader();

            ImGui::Separator();

            // We'll use auto-layout: footer in its own child, notes list takes remaining space
            // First, render notes list in a child that takes all available space except what footer needs
            float available_height = ImGui::GetContentRegionAvail().y;

            // Reserve some minimum space for footer (just the enabled button now)
            float footer_reserve = S(50.0f);

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

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Preview")) {
            RenderPreviewTab(accent_regular);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor();  // Border

    ImGui::End();

    // Render the edit modal (must be after End() since it's a separate popup)
    RenderEditModal(accent_regular, accent_muted_dark);

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
    PushOutlineButtonStyle();
    if (ImGui::Button("Add Note", ImVec2(-1, 0))) {
        HandleAddNote();
    }
    PopOutlineButtonStyle();
}


void AnnotationPanel::RenderNotesList() {
    const auto& notes = annotation_manager_->GetNotes();

    // Detect note list changes (load/add/delete) and clear resolved path cache
    if (notes.size() != last_known_note_count_) {
        resolved_path_cache_.clear();
        last_known_note_count_ = notes.size();
    }

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

            // Scroll to this note if it was just added
            if (!scroll_to_timecode_.empty() && note.timecode == scroll_to_timecode_) {
                ImGui::SetScrollHereY(0.5f);
                scroll_to_timecode_.clear();
            }

            ImGui::PopID();

            // Add spacing between notes (no separator needed since each note has its own border)
            ImGui::Spacing();
        }
    }

}

void AnnotationPanel::RenderFooter(ImVec4 accent_regular) {
    if (!annotations_enabled_ptr_) return;

    // Button colors based on enabled state
    // When disabled: outline style (transparent bg + visible border)
    // When enabled: solid accent fill (unchanged)
    bool outline_border = !(*annotations_enabled_ptr_);
    ImVec4 button_color = *annotations_enabled_ptr_ ? accent_regular : ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    ImVec4 button_hover = *annotations_enabled_ptr_ ?
        ImVec4(accent_regular.x * 1.2f, accent_regular.y * 1.2f, accent_regular.z * 1.2f, accent_regular.w) :
        ImVec4(0.28f, 0.28f, 0.28f, 0.50f);
    ImVec4 button_active = *annotations_enabled_ptr_ ?
        ImVec4(accent_regular.x * 0.8f, accent_regular.y * 0.8f, accent_regular.z * 0.8f, accent_regular.w) :
        ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

    if (outline_border) ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.45f, 0.45f, 0.45f, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_active);

    const char* button_text = *annotations_enabled_ptr_ ? "Annotations Enabled" : "Annotations Disabled";
    if (ImGui::Button(button_text, ImVec2(-1, 0))) {
        *annotations_enabled_ptr_ = !(*annotations_enabled_ptr_);
        Debug::Log(*annotations_enabled_ptr_ ? "Annotations enabled for playback" : "Annotations disabled for playback");
    }

    ImGui::PopStyleColor(3);
    if (outline_border) ImGui::PopStyleColor();
}

void AnnotationPanel::RenderNote(AnnotationNote& note) {
    // Note: Unique ID is pushed by caller (RenderNotesList) using index

    // Padding and styling
    const float padding = S(8.0f);
    const float rounding = S(1.0f);

    // Get draw list for background shape
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Store cursor position before the group
    ImVec2 group_start_pos = ImGui::GetCursorScreenPos();

    ImGui::BeginGroup(); // Group all content for proper sizing

    // Add top padding
    ImGui::Dummy(ImVec2(0, padding));

    // Add left padding and begin inner content
    ImGui::Indent(padding);

    float content_width = ImGui::GetContentRegionAvail().x - padding;
    const float column_spacing = S(8.0f);

    // === ROW 1: Timecode (top-left) + Frame number (next to it) ===
    {
        {
            bool is_selected = (selected_timecode_ == note.timecode);
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(text_col.x, text_col.y, text_col.z, 0.4f));
                ImGui::Text("%s", is_selected ? ICON_NOTE_SELECTED : ICON_NOTE);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::SameLine();
            }
        }
        if (ImGui::Selectable(note.timecode.c_str(), selected_timecode_ == note.timecode, 0, ImVec2(ImGui::CalcTextSize(note.timecode.c_str()).x, 0))) {
            selected_timecode_ = note.timecode;
            if (seek_callback_) {
                seek_callback_(note.timestamp_seconds);
            }
            // Auto-enter viewport drawing for this note
            if (!note.addressed && enter_edit_mode_callback_) {
                if (annotations_enabled_ptr_ && !(*annotations_enabled_ptr_))
                    *annotations_enabled_ptr_ = true;
                enter_edit_mode_callback_(note.timecode, note.timestamp_seconds, note.frame, note.annotation_data);
            }
        }

        ImGui::SameLine(0.0f, column_spacing);

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("F: %d", note.frame);
        ImGui::PopStyleColor();
    }

    // === ROW 2: Thumbnail (left) + Text input (fills rest) ===
    {
        const float thumbnail_width = S(120.0f);
        uintptr_t thumbnail_id = 0;
        float thumbnail_aspect = video_aspect_ratio_;
        std::string full_image_path;

        full_image_path = ResolveThumbnailPath(note);
        if (!full_image_path.empty()) {
            thumbnail_id = LoadThumbnail(full_image_path);

            auto aspect_it = thumbnail_aspect_cache_.find(full_image_path);
            if (aspect_it != thumbnail_aspect_cache_.end()) {
                thumbnail_aspect = aspect_it->second;
            }
        }

        float thumbnail_height = thumbnail_width / thumbnail_aspect;

        if (thumbnail_id != 0) {
            ImGui::Image(ThumbHandleToImTexture(thumbnail_id), ImVec2(thumbnail_width, thumbnail_height));

            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_timecode_ = note.timecode;
                if (seek_callback_) {
                    seek_callback_(note.timestamp_seconds);
                }
                // Auto-enter viewport drawing for this note
                if (!note.addressed && enter_edit_mode_callback_) {
                    if (annotations_enabled_ptr_ && !(*annotations_enabled_ptr_))
                        *annotations_enabled_ptr_ = true;
                    enter_edit_mode_callback_(note.timecode, note.timestamp_seconds, note.frame, note.annotation_data);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to navigate to this frame");
            }
        } else {
            ImGui::Dummy(ImVec2(thumbnail_width, thumbnail_height));
        }

        ImGui::SameLine(0.0f, column_spacing);

        // Text input fills remaining width, matches thumbnail height
        float text_width = content_width - thumbnail_width - column_spacing;

        char text_buffer[1024];
        strncpy(text_buffer, note.text.c_str(), sizeof(text_buffer) - 1);
        text_buffer[sizeof(text_buffer) - 1] = '\0';

        if (note.addressed) {
            ImVec4 text_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            text_color.w = 0.6f;
            ImGui::PushStyleColor(ImGuiCol_Text, text_color);
        }

        if (ImGui::InputTextMultiline("##text", text_buffer, sizeof(text_buffer),
            ImVec2(text_width, thumbnail_height), ImGuiInputTextFlags_WordWrap)) {
            annotation_manager_->UpdateNoteText(note.timecode, text_buffer);
        }

        if (note.addressed) {
            ImGui::PopStyleColor();
        }

        if (ImGui::IsItemActivated()) {
            selected_timecode_ = note.timecode;
            if (seek_callback_) {
                seek_callback_(note.timestamp_seconds);
            }
            // Auto-enter viewport drawing for this note
            if (!note.addressed && enter_edit_mode_callback_) {
                if (annotations_enabled_ptr_ && !(*annotations_enabled_ptr_))
                    *annotations_enabled_ptr_ = true;
                enter_edit_mode_callback_(note.timecode, note.timestamp_seconds, note.frame, note.annotation_data);
            }
        }
    }

    // === ROW 3: Addressed (left) + Edit & Delete buttons (flush right) ===
    ImGui::Dummy(ImVec2(0, S(1.0f)));
    {
        bool edit_button_enabled = !note.addressed;

        // Addressed checkbox (left side)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(5.0f), S(2.0f)));
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

        // Edit and Delete buttons flush right
        const float button_spacing = S(4.0f);
        const float edit_padding_x = S(12.0f);
        float edit_text_button_width = ImGui::CalcTextSize("Edit").x + edit_padding_x * 2;
        const float delete_padding_x = S(12.0f);
        float delete_button_width = ImGui::CalcTextSize("Delete").x + delete_padding_x * 2;
        float buttons_total_width = edit_text_button_width + delete_button_width + button_spacing;
        float indent_amount = padding;  // match our left indent

        ImGui::SameLine(content_width - buttons_total_width + indent_amount);

        // Edit button (opens modal editor)
        if (!edit_button_enabled) ImGui::BeginDisabled();
        PushOutlineButtonStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(edit_padding_x, ImGui::GetStyle().FramePadding.y));
        bool edit_modal_clicked = ImGui::Button("Edit", ImVec2(0, 0));
        ImGui::PopStyleVar();
        PopOutlineButtonStyle();
        if (!edit_button_enabled) ImGui::EndDisabled();

        if (edit_modal_clicked && edit_button_enabled) {
            modal_edit_timecode_ = note.timecode;
            modal_edit_timestamp_ = note.timestamp_seconds;
            modal_edit_frame_ = note.frame;
            modal_edit_text_buffer_ = note.text;
            modal_edit_text_buffer_.resize(4096);

            // Load thumbnail for modal and read its real aspect ratio
            if (annotation_manager_) {
                std::string images_folder = annotation_manager_->GetImagesFolder();
                std::string full_path = images_folder + "/" + note.image_path.substr(note.image_path.find_last_of('/') + 1);
                modal_image_texture_ = LoadThumbnail(full_path);

                auto aspect_it = thumbnail_aspect_cache_.find(full_path);
                if (aspect_it != thumbnail_aspect_cache_.end()) {
                    modal_image_aspect_ = aspect_it->second;
                } else {
                    modal_image_aspect_ = video_aspect_ratio_;  // fallback
                }
            }

            edit_modal_open_ = true;
            edit_modal_just_opened_ = true;

            // Enter annotation edit mode for this note
            if (annotations_enabled_ptr_ && !(*annotations_enabled_ptr_)) {
                *annotations_enabled_ptr_ = true;
            }
            if (enter_modal_edit_callback_) {
                enter_modal_edit_callback_(note.timecode, note.timestamp_seconds, note.frame, note.annotation_data);
            }
        }

        // Delete button (extra horizontal padding for text breathing room)
        ImGui::SameLine(0.0f, button_spacing);
        PushOutlineButtonStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12.0f), ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button("Delete", ImVec2(0, 0))) {
            HandleDeleteNote(note.timecode);
        }
        ImGui::PopStyleVar();
        PopOutlineButtonStyle();

        #undef ICON_DRAW
    }

    // Add bottom padding
    ImGui::Dummy(ImVec2(0, padding));

    // Unindent to close left padding
    ImGui::Unindent(padding);

    ImGui::EndGroup(); // End content group

    // Get the size of the group we just drew
    ImVec2 group_end_pos = ImGui::GetItemRectMax();
    ImVec2 group_size = ImVec2(group_end_pos.x - group_start_pos.x, group_end_pos.y - group_start_pos.y);

    // Extend the shape a few pixels on the right for better visual spacing
    const float right_extension = S(8.0f);

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

}

void AnnotationPanel::RenderPreviewTab(ImVec4 accent_regular) {
    // Header: note count + loading/saving status (no Add Note button)
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

    ImGui::Separator();

    // Layout: scrollable notes + footer
    float available_height = ImGui::GetContentRegionAvail().y;
    float footer_reserve = S(50.0f);

    // Scrollable preview notes
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (ImGui::BeginChild("PreviewScrollRegion", ImVec2(0, available_height - footer_reserve), false)) {
        const auto& notes = annotation_manager_->GetNotes();

        // Detect note list changes and clear resolved path cache
        if (notes.size() != last_known_note_count_) {
            resolved_path_cache_.clear();
            last_known_note_count_ = notes.size();
        }

        if (notes.empty()) {
            ImGui::TextDisabled("No annotations yet");
        } else {
            int note_index = 0;
            for (auto& note : annotation_manager_->GetNotes()) {
                ImGui::PushID(note_index++);
                RenderPreviewNote(const_cast<AnnotationNote&>(note));
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Separator();

    // Footer with Annotations Enabled button
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (ImGui::BeginChild("PreviewFooterRegion", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar)) {
        RenderFooter(accent_regular);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void AnnotationPanel::RenderPreviewNote(AnnotationNote& note) {
    const float padding = S(8.0f);
    const float rounding = S(1.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 group_start_pos = ImGui::GetCursorScreenPos();

    ImGui::BeginGroup();

    // Apply alpha modulation for addressed notes
    if (note.addressed) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f);
    }

    ImGui::Dummy(ImVec2(0, padding));
    ImGui::Indent(padding);

    float content_width = ImGui::GetContentRegionAvail().x - padding;

    // Timecode + frame number
    {
        bool is_selected = (selected_timecode_ == note.timecode);
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(text_col.x, text_col.y, text_col.z, 0.4f));
            ImGui::Text("%s", is_selected ? ICON_NOTE_SELECTED : ICON_NOTE);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine();
        }
    }
    ImGui::Text("%s", note.timecode.c_str());
    ImGui::SameLine(0.0f, S(8.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::Text("Frame: %d", note.frame);
    ImGui::PopStyleColor();

    // Full-width thumbnail
    uintptr_t thumbnail_id = 0;
    float thumbnail_aspect = video_aspect_ratio_;

    {
        std::string full_image_path = ResolveThumbnailPath(note);
        if (!full_image_path.empty()) {
            thumbnail_id = LoadThumbnail(full_image_path);

            auto aspect_it = thumbnail_aspect_cache_.find(full_image_path);
            if (aspect_it != thumbnail_aspect_cache_.end()) {
                thumbnail_aspect = aspect_it->second;
            }
        }
    }

    float thumb_w = content_width;
    float thumb_h = thumb_w / thumbnail_aspect;

    if (thumbnail_id != 0) {
        ImGui::Image(ThumbHandleToImTexture(thumbnail_id), ImVec2(thumb_w, thumb_h));
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (seek_callback_) {
                seek_callback_(note.timestamp_seconds);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to seek to this frame");
        }
    } else {
        ImGui::Dummy(ImVec2(thumb_w, thumb_h));
    }

    // Markdown-rendered note text
    if (!note.text.empty()) {
        ImGui::Spacing();

        ImGui::MarkdownConfig md_config;
        md_config.linkCallback = nullptr;
        md_config.tooltipCallback = nullptr;
        md_config.imageCallback = nullptr;
        md_config.headingFormats[0] = { nullptr, true };
        md_config.headingFormats[1] = { nullptr, true };
        md_config.headingFormats[2] = { nullptr, false };
        md_config.formatCallback = MarkdownFormatCallback;

        RenderMarkdownWithCode(note.text, md_config);
    }

    ImGui::Dummy(ImVec2(0, padding));
    ImGui::Unindent(padding);

    if (note.addressed) {
        ImGui::PopStyleVar();
    }

    ImGui::EndGroup();

    // Draw rounded border (same style as Edit tab)
    ImVec2 group_end_pos = ImGui::GetItemRectMax();
    ImVec2 group_size = ImVec2(group_end_pos.x - group_start_pos.x, group_end_pos.y - group_start_pos.y);
    const float right_extension = S(8.0f);

    draw_list->AddRect(
        group_start_pos,
        ImVec2(group_start_pos.x + group_size.x + right_extension, group_start_pos.y + group_size.y),
        IM_COL32(255, 255, 255, 15),
        rounding, 0, 1.0f);

    // Invisible button over the card for click interactions
    ImVec2 card_size = ImVec2(group_size.x + right_extension, group_size.y);
    ImGui::SetCursorScreenPos(group_start_pos);
    ImGui::InvisibleButton("##card_click", card_size);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        // Left-click: select note and seek to its timestamp
        selected_timecode_ = note.timecode;
        if (seek_callback_) {
            seek_callback_(note.timestamp_seconds);
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        // Right-click: toggle addressed state
        annotation_manager_->UpdateNoteAddressed(note.timecode, !note.addressed);
    }
}

void AnnotationPanel::RenderEditModal(ImVec4 accent_regular, ImVec4 accent_muted_dark) {
    if (!edit_modal_open_) return;

    // Handle deferred open
    if (edit_modal_just_opened_) {
        ImGui::OpenPopup("##EditAnnotationModal");
        edit_modal_just_opened_ = false;
    }

    // Modal height is fixed at 95% viewport; width wraps to image width
    // (clamped between 50% and 95% of viewport for tall/wide extremes).
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float padding = S(16.0f);

    float modal_h = vp->Size.y * 0.95f;

    // Pre-compute expected image width from vertical budget + aspect ratio
    // so we can size the modal *before* BeginPopupModal.
    float est_line_h = ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y;
    float est_frame_pad_y = ImGui::GetStyle().FramePadding.y;
    float est_spacing_y = ImGui::GetStyle().ItemSpacing.y;
    float est_header = est_line_h * 2.0f + 2.0f + est_spacing_y;  // title + subtitle + separator
    float est_text_area = est_line_h * 12.0f;
    float est_toolbar = est_line_h + est_frame_pad_y * 2.0f;
    float est_bottom = est_spacing_y + est_text_area + est_spacing_y + 2.0f + est_spacing_y + est_toolbar;
    float est_img_h = (modal_h - 2.0f * padding) - est_header - est_bottom;
    if (est_img_h < S(60.0f)) est_img_h = S(60.0f);
    float est_img_w = est_img_h * modal_image_aspect_;

    // Clamp modal width: at least 50% viewport, at most 95%
    float modal_w = est_img_w + 2.0f * padding;
    modal_w = std::max(modal_w, vp->Size.x * 0.5f);
    modal_w = std::min(modal_w, vp->Size.x * 0.95f);

    ImVec2 center = vp->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(modal_w, modal_h), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.0f));

    bool modal_open = true;
    if (ImGui::BeginPopupModal("##EditAnnotationModal", &modal_open,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {

        // Handle close request from external code (Done/Cancel from toolbar)
        if (modal_close_requested_) {
            modal_close_requested_ = false;
            edit_modal_open_ = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            return;
        }

        float content_w = ImGui::GetContentRegionAvail().x;

        // === Header: Title + Close button ===
        {
            ImGui::Text("Edit Annotation");
            ImGui::SameLine(content_w - S(20.0f));
            if (font_icons) {
                ImGui::PushFont(font_icons);
                if (ImGui::SmallButton(ICON_CLOSE)) {
                    if (save_modal_edit_callback_) save_modal_edit_callback_();
                    edit_modal_open_ = false;
                    ImGui::CloseCurrentPopup();
                    ImGui::PopFont();
                    ImGui::EndPopup();
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor();
                    return;
                }
                ImGui::PopFont();
            } else {
                if (ImGui::SmallButton("X")) {
                    if (save_modal_edit_callback_) save_modal_edit_callback_();
                    edit_modal_open_ = false;
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor();
                    return;
                }
            }
        }

        // Subtitle: timecode and frame
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("Editing: %s  (Frame: %d)", modal_edit_timecode_.c_str(), modal_edit_frame_);
        ImGui::PopStyleColor();

        ImGui::Separator();
        ImGui::Spacing();

        // --- Measure remaining space and allocate to image / text / toolbar ---
        // We know what comes after the image: spacing + text area + separator + spacing + toolbar row.
        // Measure those heights from actual ImGui metrics so we don't drift.
        float line_h = ImGui::GetTextLineHeightWithSpacing();
        float frame_pad_y = ImGui::GetStyle().FramePadding.y;
        float item_spacing_y = ImGui::GetStyle().ItemSpacing.y;

        float text_area_h = line_h * 12.0f;  // ~12 lines for notes
        float toolbar_row_h = line_h + frame_pad_y * 2.0f;  // one row of buttons
        float bottom_chrome = item_spacing_y          // Spacing() after image
                            + text_area_h             // text input
                            + item_spacing_y          // spacing before separator
                            + 2.0f                    // Separator
                            + item_spacing_y          // Spacing() after separator
                            + toolbar_row_h;          // toolbar + delete button row

        float avail_h = ImGui::GetContentRegionAvail().y;
        float img_h = avail_h - bottom_chrome;
        if (img_h < S(60.0f)) img_h = S(60.0f);

        // Constrain image by its actual aspect ratio (from the thumbnail file, not the video player)
        float img_w = img_h * modal_image_aspect_;
        if (img_w > content_w) {
            img_w = content_w;
            img_h = img_w / modal_image_aspect_;
        }

        // === Image ===
        if (modal_image_texture_ != 0) {
            ImGui::Image(ThumbHandleToImTexture(modal_image_texture_), ImVec2(img_w, img_h));
        } else {
            ImGui::Dummy(ImVec2(img_w, img_h));
        }

        // Read back the actual rendered rect — this is what NanoVG must match exactly
        modal_image_screen_pos_ = ImGui::GetItemRectMin();
        modal_image_screen_size_ = ImGui::GetItemRectSize();

        ImGui::Spacing();

        // === Text area: match image width, but never narrower than 50% of modal ===
        float text_w = std::max(img_w, content_w * 0.5f);
        if (ImGui::InputTextMultiline("##ModalText", modal_edit_text_buffer_.data(),
            modal_edit_text_buffer_.capacity(), ImVec2(text_w, text_area_h),
            ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize,
            [](ImGuiInputTextCallbackData* data) -> int {
                if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                    auto* str = static_cast<std::string*>(data->UserData);
                    str->resize(data->BufTextLen);
                    data->Buf = str->data();
                }
                return 0;
            }, &modal_edit_text_buffer_)) {
            // Save text changes immediately
            if (annotation_manager_) {
                annotation_manager_->UpdateNoteText(modal_edit_timecode_, modal_edit_text_buffer_.c_str());
            }
        }

        ImGui::Separator();
        ImGui::Spacing();

        // === Toolbar ===
        if (annotation_toolbar_ && viewport_annotator_) {
            bool can_undo = can_undo_callback_ ? can_undo_callback_() : false;
            bool can_redo = can_redo_callback_ ? can_redo_callback_() : false;

            ImFont* icon_font = font_icons;
            annotation_toolbar_->Render(viewport_annotator_, can_undo, can_redo,
                                        icon_font, accent_regular, accent_muted_dark);
        }

        // Delete Note button (accent dark, flush right)
        ImGui::SameLine(content_w - ImGui::CalcTextSize("Delete Note").x - S(24.0f));
        ImVec4 del_hover(
            std::min(accent_muted_dark.x * 1.25f, 1.0f),
            std::min(accent_muted_dark.y * 1.25f, 1.0f),
            std::min(accent_muted_dark.z * 1.25f, 1.0f),
            accent_muted_dark.w);
        ImVec4 del_active(
            accent_muted_dark.x * 0.8f,
            accent_muted_dark.y * 0.8f,
            accent_muted_dark.z * 0.8f,
            accent_muted_dark.w);
        ImGui::PushStyleColor(ImGuiCol_Button, accent_muted_dark);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, del_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, del_active);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12.0f), ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button("Delete Note")) {
            std::string tc = modal_edit_timecode_;
            edit_modal_open_ = false;
            ImGui::CloseCurrentPopup();
            if (delete_note_callback_) delete_note_callback_(tc);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        ImGui::EndPopup();
    } else {
        // Modal was closed via Escape or clicking outside
        if (edit_modal_open_) {
            if (save_modal_edit_callback_) save_modal_edit_callback_();
            edit_modal_open_ = false;
        }
    }

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
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

    // Scroll the list to the newly added note on the next frame
    scroll_to_timecode_ = timecode;

    Debug::Log("Note added successfully at " + timecode);
}

void AnnotationPanel::HandleDeleteNote(const std::string& timecode) {
    // TODO: Add confirmation dialog
    annotation_manager_->DeleteNote(timecode);
    resolved_path_cache_.erase(timecode);
    if (selected_timecode_ == timecode) {
        selected_timecode_.clear();
    }
}

void AnnotationPanel::SetSelectedNote(const std::string& timecode) {
    selected_timecode_ = timecode;
}

uintptr_t AnnotationPanel::LoadThumbnail(const std::string& image_path) {
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

    // Create texture
#ifdef QCVIEW_USE_VULKAN
    uint64_t pool_id = qcview::VulkanTexturePool::Instance().CreateTextureFromPixels(
        width, height, VK_FORMAT_R8G8B8A8_UNORM,
        image_data.data(), image_data.size());
    uintptr_t texture_id = static_cast<uintptr_t>(pool_id);
#elif defined(QCVIEW_USE_METAL)
    // Bypass MetalTexturePool: note thumbnails are small and must never be
    // LRU-evicted under video/annotation frame pressure. Own the MTLTexture
    // directly so ImTextureID stays valid for the panel's lifetime.
    void* mtl_texture = qcview::CreateStandaloneAnnotationThumbnailMetal(
        width, height, image_data.data());
    uintptr_t texture_id = reinterpret_cast<uintptr_t>(mtl_texture);
#else
    GLuint gl_texture_id;
    glGenTextures(1, &gl_texture_id);
    glBindTexture(GL_TEXTURE_2D, gl_texture_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data.data());
    uintptr_t texture_id = static_cast<uintptr_t>(gl_texture_id);
#endif

    // Cache the texture and aspect ratio
    thumbnail_cache_[image_path] = texture_id;
    thumbnail_aspect_cache_[image_path] = static_cast<float>(width) / static_cast<float>(height);

    Debug::Log("Loaded thumbnail: " + image_path + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

    return texture_id;
}

std::string AnnotationPanel::ResolveThumbnailPath(const AnnotationNote& note) {
    if (!annotation_manager_) return "";

    // Check resolved path cache first — avoids filesystem calls on every frame
    auto cache_it = resolved_path_cache_.find(note.timecode);
    if (cache_it != resolved_path_cache_.end()) {
        return cache_it->second;
    }

    std::string images_folder = annotation_manager_->GetImagesFolder();
    std::string filename = note.image_path.substr(note.image_path.find_last_of('/') + 1);
    std::string clean_path = images_folder + "/" + filename;

    // Prefer annotated thumbnail if it exists
    std::string annotated_path = Annotations::GetAnnotatedThumbnailPath(clean_path);
    if (std::filesystem::exists(annotated_path)) {
        resolved_path_cache_[note.timecode] = annotated_path;
        return annotated_path;
    }

    // Lazy generation: if annotation_data has strokes but no annotated thumbnail yet, generate it
    if (!note.annotation_data.empty() && std::filesystem::exists(clean_path)) {
        if (Annotations::GenerateAnnotatedThumbnail(clean_path, annotated_path, note.annotation_data)) {
            resolved_path_cache_[note.timecode] = annotated_path;
            return annotated_path;
        }
    }

    resolved_path_cache_[note.timecode] = clean_path;
    return clean_path;
}

void AnnotationPanel::InvalidateThumbnail(const std::string& image_path) {
    auto it = thumbnail_cache_.find(image_path);
    if (it != thumbnail_cache_.end()) {
#if defined(QCVIEW_USE_METAL)
        // Metal: we own the MTLTexture directly (pool is bypassed), release it now.
        qcview::DestroyStandaloneAnnotationThumbnailMetal(reinterpret_cast<void*>(it->second));
#elif defined(QCVIEW_USE_VULKAN)
        // Vulkan: pool-owned, cleaned up on pool shutdown.
#else
        GLuint gl_id = static_cast<GLuint>(it->second);
        glDeleteTextures(1, &gl_id);
#endif
        thumbnail_cache_.erase(it);
        thumbnail_aspect_cache_.erase(image_path);
    }
    // Clear resolved path cache entries that point to this image
    for (auto pit = resolved_path_cache_.begin(); pit != resolved_path_cache_.end(); ) {
        if (pit->second == image_path) {
            pit = resolved_path_cache_.erase(pit);
        } else {
            ++pit;
        }
    }
}

void AnnotationPanel::CleanupThumbnails() {
#if defined(QCVIEW_USE_METAL)
    // Metal: panel owns retained MTLTexture refs directly; release each.
    for (auto& pair : thumbnail_cache_) {
        qcview::DestroyStandaloneAnnotationThumbnailMetal(reinterpret_cast<void*>(pair.second));
    }
#elif defined(QCVIEW_USE_VULKAN)
    // Vulkan: pool-owned, cleaned up on pool shutdown.
#else
    for (auto& pair : thumbnail_cache_) {
        GLuint gl_id = static_cast<GLuint>(pair.second);
        glDeleteTextures(1, &gl_id);
    }
#endif
    thumbnail_cache_.clear();
    thumbnail_aspect_cache_.clear();
    resolved_path_cache_.clear();
}

} // namespace qcview
