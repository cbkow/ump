// ============================================================================
// Inspector panel
// ============================================================================

#include "app/application.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "project/project_manager.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "player/video_player.h"
#include "ui/timeline_manager.h"
#include "nodes/node_manager.h"
#include <imgui.h>

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern std::unique_ptr<ump::TimelineView> timeline_view;
extern bool otio_dual_view_mode;

    void Application::CreateInspectorPanel() {
        if (!show_inspector_panel) return;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.141f, 0.141f, 0.141f, 1.0f));  // #242424
        ImGui::PushStyleColor(ImGuiCol_Border, kTransparentBorder);
        if (ImGui::Begin("Inspector", &show_inspector_panel)) {
            // Header row with close button
            {
                // Determine context-aware title based on current content type
                const char* inspector_context = "Media Properties";

                // Get current media item - check multiple sources in priority order:
                // 1. Media loaded into TimelineView (IMAGE_SEQUENCE, PLAYLIST, VIDEO_FILE modes)
                // 2. Timelines (via GetCurrentTimelineItem)
                // 3. Videos/other media (via GetMediaItemFromCurrentPath)
                ump::MediaItem* current_item = nullptr;
                if (timeline_view) {
                    auto source_mode = timeline_view->GetSourceMode();
                    if (source_mode == ump::TimelineSourceMode::IMAGE_SEQUENCE ||
                        source_mode == ump::TimelineSourceMode::PLAYLIST ||
                        source_mode == ump::TimelineSourceMode::VIDEO_FILE) {
                        current_item = timeline_view->GetSourceMediaItem();
                    }
                }
                if (!current_item && project_manager) {
                    // Check for active timeline first (current_file_path is cleared for timelines)
                    current_item = project_manager->GetCurrentTimelineItem();
                }
                if (!current_item && project_manager) {
                    // Fall back to path-based lookup for videos
                    current_item = project_manager->GetMediaItemFromCurrentPath();
                }

                if (otio_dual_view_mode || (current_item && current_item->type == ump::MediaType::DUAL_VIEW)) {
                    inspector_context = "Dual View Comparison";
                } else if (current_item && current_item->type == ump::MediaType::PLAYLIST) {
                    inspector_context = "Playlist";
                } else if (current_item && current_item->type == ump::MediaType::EXR_SEQUENCE) {
                    inspector_context = "EXR Sequence";
                } else if (current_item && current_item->type == ump::MediaType::IMAGE_SEQUENCE) {
                    inspector_context = "Image Sequence";
                } else if (current_item && current_item->type == ump::MediaType::VIDEO) {
                    inspector_context = "Video";
                } else if (current_item && current_item->type == ump::MediaType::AUDIO) {
                    inspector_context = "Audio";
                }

                ImGui::PushStyleColor(ImGuiCol_Text, UI_GRAY_VEC4);
                if (font_icons) {
                    ImGui::PushFont(font_icons);
                    ImGui::Text(ICON_INFO);
                    ImGui::PopFont();
                    ImGui::SameLine();
                }
                if (font_bold) ImGui::PushFont(font_bold);
                ImGui::Text("Inspector: %s", inspector_context);
                if (font_bold) ImGui::PopFont();
                ImGui::PopStyleColor();

                // Close button on the right
                float button_size = ImGui::GetFontSize() + 4.0f;  // Compact size
                ImGui::SameLine(ImGui::GetWindowWidth() - button_size - ImGui::GetStyle().WindowPadding.x);
                ImVec2 button_pos = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##CloseInspector", ImVec2(button_size, button_size));
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
                if (clicked) {
                    show_inspector_panel = false;
                }
                if (hovered) {
                    ImGui::SetTooltip("Close Inspector (Ctrl+2)");
                }
            }
            ImGui::Separator();

            // Determine content type for Inspector body
            // Get current media item - check multiple sources in priority order:
            // 1. Media loaded into TimelineView (IMAGE_SEQUENCE, PLAYLIST, VIDEO_FILE, DUAL_VIEW modes)
            // 2. Timelines (via GetCurrentTimelineItem)
            // 3. Videos/other media (via GetMediaItemFromCurrentPath)
            ump::MediaItem* inspector_item = nullptr;
            if (timeline_view) {
                auto source_mode = timeline_view->GetSourceMode();
                if (source_mode == ump::TimelineSourceMode::IMAGE_SEQUENCE ||
                    source_mode == ump::TimelineSourceMode::PLAYLIST ||
                    source_mode == ump::TimelineSourceMode::VIDEO_FILE) {
                    inspector_item = timeline_view->GetSourceMediaItem();
                }
            }
            if (!inspector_item && project_manager) {
                // Check for active timeline first (current_file_path is cleared for timelines)
                inspector_item = project_manager->GetCurrentTimelineItem();
            }
            if (!inspector_item && project_manager && otio_dual_view_mode) {
                // Check for active dual view (uses is_active flag instead of current_timeline_id)
                inspector_item = project_manager->GetActiveDualViewItem();
            }
            if (!inspector_item && project_manager) {
                // Fall back to path-based lookup for videos
                inspector_item = project_manager->GetMediaItemFromCurrentPath();
            }
            bool show_dual_view_inspector = inspector_item && inspector_item->type == ump::MediaType::DUAL_VIEW;
            bool show_playlist_inspector = inspector_item && inspector_item->type == ump::MediaType::PLAYLIST;
            bool show_sequence_inspector = inspector_item && (inspector_item->type == ump::MediaType::IMAGE_SEQUENCE ||
                                                               inspector_item->type == ump::MediaType::EXR_SEQUENCE ||
                                                               (inspector_item->path.size() > 6 && inspector_item->path.substr(0, 6) == "exr://"));

            // Dual View Mode - show basic dual view properties
            if (show_dual_view_inspector && timeline_view) {
                // Get dual view info from timeline_view and inspector_item
                double frame_rate = timeline_view->GetFrameRate();
                double duration = timeline_view->GetDuration();
                int width = inspector_item ? inspector_item->timeline_width : 1920;
                int height = inspector_item ? inspector_item->timeline_height : 1080;
                std::string dv_name = inspector_item ? inspector_item->name : "Untitled";

                // Show dual view name as subheader
                ImGui::Text("%s", dv_name.c_str());
                ImGui::Spacing();

                // Properties table
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
                if (ImGui::BeginTable("DualViewProps", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
                    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    // Resolution
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(UI_GRAY_VEC4, "Resolution:");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%dx%d", width, height);

                    // Frame Rate
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(UI_GRAY_VEC4, "Frame Rate:");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f fps", frame_rate);

                    // Duration (HH:MM:SS:FF format)
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(UI_GRAY_VEC4, "Duration:");
                    ImGui::TableSetColumnIndex(1);
                    int total_secs = (int)duration;
                    int hours = total_secs / 3600;
                    int mins = (total_secs % 3600) / 60;
                    int secs = total_secs % 60;
                    int frames = (int)((duration - total_secs) * frame_rate);
                    if (hours > 0) {
                        ImGui::Text("%02d:%02d:%02d:%02d", hours, mins, secs, frames);
                    } else {
                        ImGui::Text("%02d:%02d:%02d", mins, secs, frames);
                    }

                    // Total Frames
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(UI_GRAY_VEC4, "Total Frames:");
                    ImGui::TableSetColumnIndex(1);
                    int total_frames = (int)(duration * frame_rate);
                    ImGui::Text("%d", total_frames);

                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();  // CellPadding
            }
            // Playlist Mode - show simple playlist info (name and clip count)
            else if (show_playlist_inspector && inspector_item) {
                // Show playlist name
                ImGui::Text("%s", inspector_item->name.c_str());
                ImGui::Spacing();

                // Show clip count from playlist_items
                int clip_count = static_cast<int>(inspector_item->playlist_items.size());
                ImGui::TextColored(UI_GRAY_VEC4, "%d clip%s", clip_count, clip_count == 1 ? "" : "s");
            }
            else if (show_sequence_inspector) {
                // Image Sequence / EXR Mode - show properties from MediaItem and TimelineView
                // (video_player->HasVideo() is false in timeline mode, so we use MediaItem data)
                if (inspector_item) {
                    // Check if this is an EXR sequence (by type or path format)
                    bool is_exr = inspector_item->type == ump::MediaType::EXR_SEQUENCE ||
                                  (inspector_item->path.size() > 6 && inspector_item->path.substr(0, 6) == "exr://");
                    if (is_exr) {
                        // EXR sequence - show EXR-specific properties with layer info
                        project_manager->DisplayEXRPropertiesForItem(inspector_item, timeline_view.get());
                    } else {
                        // Regular image sequence - show basic properties
                        ImGui::Spacing();
                        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Sequence Properties");
        if (font_bold) ImGui::PopFont();
                        ImGui::Separator();

                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
                        if (ImGui::BeginTable("ImageSeqProps", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
                            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                            // Get source directory from TimelineView or MediaItem
                            std::string source_dir = timeline_view ? timeline_view->GetSourceDirectory() : "";
                            // Fallback: extract directory from MediaItem path (mf://path format)
                            if (source_dir.empty() && !inspector_item->path.empty()) {
                                std::string path = inspector_item->path;
                                if (path.substr(0, 5) == "mf://") {
                                    path = path.substr(5);
                                }
                                size_t last_slash = path.find_last_of("/\\");
                                if (last_slash != std::string::npos) {
                                    source_dir = path.substr(0, last_slash);
                                }
                            }

                            // Path row
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextDisabled("Path:");
                            ImGui::TableSetColumnIndex(1);
                                ImGui::TextWrapped("%s", source_dir.c_str());
                                project_manager->RenderPathButtons(source_dir, "SeqPath");

                            // Image type (from path extension)
                            std::string image_type = "Unknown";
                            if (!inspector_item->path.empty()) {
                                size_t dot_pos = inspector_item->path.find_last_of('.');
                                if (dot_pos != std::string::npos) {
                                    std::string ext = inspector_item->path.substr(dot_pos + 1);
                                    // Remove any glob pattern characters
                                    size_t star_pos = ext.find('*');
                                    if (star_pos != std::string::npos) ext = ext.substr(0, star_pos);
                                    std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
                                    image_type = ext;
                                }
                            }

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextDisabled("Image Type:");
                            ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%s", image_type.c_str());

                            // Resolution - from MediaItem (image_seq or legacy) or TimelineView
                            int width = inspector_item->image_seq.width > 0 ? inspector_item->image_seq.width :
                                        (inspector_item->sequence_width > 0 ? inspector_item->sequence_width :
                                        (timeline_view ? timeline_view->GetCanvasWidth() : 0));
                            int height = inspector_item->image_seq.height > 0 ? inspector_item->image_seq.height :
                                         (inspector_item->sequence_height > 0 ? inspector_item->sequence_height :
                                         (timeline_view ? timeline_view->GetCanvasHeight() : 0));
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextDisabled("Resolution:");
                            ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%d x %d", width, height);

                            // Frame Rate - from MediaItem or TimelineView
                            double fps = inspector_item->frame_rate > 0 ? inspector_item->frame_rate : (timeline_view ? timeline_view->GetFrameRate() : 24.0);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextDisabled("Frame Rate:");
                            ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%.3f fps", fps);

                            // Frame count - from MediaItem or TimelineView
                            int frame_count = inspector_item->image_seq.frame_count > 0 ? inspector_item->image_seq.frame_count :
                                              (inspector_item->frame_count > 0 ? inspector_item->frame_count :
                                              (timeline_view ? static_cast<int>(timeline_view->GetDuration() * fps) : 0));
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextDisabled("Frames:");
                            ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%d", frame_count);

                            // Duration
                            double duration = timeline_view ? timeline_view->GetDuration() : (frame_count > 0 && fps > 0 ? frame_count / fps : 0);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextDisabled("Duration:");
                            ImGui::TableSetColumnIndex(1);
                                int total_secs = (int)duration;
                            int mins = total_secs / 60;
                            int secs = total_secs % 60;
                            int frames = (int)((duration - total_secs) * fps);
                            ImGui::Text("%02d:%02d:%02d (%.2fs)", mins, secs, frames, duration);

                            ImGui::EndTable();
                        }
                        ImGui::PopStyleVar();  // CellPadding
                    }
                } else {
                    ImGui::TextColored(UI_GRAY_VEC4, "No sequence selected");
                }
            }
            else {
                // Video mode - use existing CreatePropertiesSection for full metadata display
                project_manager->CreatePropertiesSection();
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();  // kTransparentBorder
        ImGui::PopStyleColor();  // WindowBg #242424
    }
