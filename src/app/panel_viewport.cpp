// ============================================================================
// Video viewport panel (viewport, background selection, background drawing)
// ============================================================================

#include "app/application.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/app_config.h"
#include "app/drawing_utils.h"
#include "project/project_manager.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "player/video_player.h"
#include "player/pipeline_mode.h"
#include "ui/timeline_manager.h"
#include "ui/annotation_panel.h"
#include "annotations/annotation_manager.h"
#include "nodes/node_manager.h"
#include "hdr/hdr_output_manager.h"
#include "annotations/annotation_serializer.h"
#include "timeline/timeline_cache.h"
#include "timeline/timeline_thumbnail_cache.h"
#include "timeline/timeline_commands.h"
#include "timeline/media_linker.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>
#include <nanovg.h>

// OTIO Timeline UI Constants (duplicated from panel_timeline.cpp — shared constants)
namespace OTIOTimeline {
    constexpr float TRACK_HEADER_WIDTH = 140.0f;
    constexpr float TRACK_LANE_HEIGHT = 32.0f;
    constexpr float TIMELINE_RULER_HEIGHT = 48.0f;
    constexpr float TRACK_SEPARATOR_HEIGHT = 4.0f;
    constexpr float OVERVIEW_TRACK_HEIGHT = 28.0f;
    constexpr float MIN_PIXELS_PER_SECOND = 2.5f;
    constexpr float MAX_PIXELS_PER_SECOND = 1400.0f;
    constexpr float DEFAULT_PIXELS_PER_SECOND = 50.0f;
}

// ColorCorrectedTextureCache struct (defined in main.cpp)
struct ColorCorrectedTextureCache {
    GLuint left_texture = 0;
    GLuint right_texture = 0;
    int left_width = 0;
    int left_height = 0;
    int right_width = 0;
    int right_height = 0;
    GLuint cached_frame_texture = 0;
    int cached_frame_width = 0;
    int cached_frame_height = 0;
    GLuint cached_frame_source_id = 0;
    GLuint composite_texture = 0;
    int composite_width = 0;
    int composite_height = 0;
    GLuint prev_left_texture = 0;
    GLuint prev_right_texture = 0;
    GLuint prev_cached_frame_texture = 0;
    GLuint prev_composite_texture = 0;
    GLsync upload_fence = nullptr;
    bool current_ready = true;
    GLuint GetLeftTexture() const { return current_ready ? left_texture : prev_left_texture; }
    GLuint GetRightTexture() const { return current_ready ? right_texture : prev_right_texture; }
    GLuint GetCachedFrameTexture() const { return current_ready ? cached_frame_texture : prev_cached_frame_texture; }
    GLuint GetCompositeTexture() const { return current_ready ? composite_texture : prev_composite_texture; }
};

// ViewportDualViewDropState struct (defined in main.cpp)
struct ViewportDualViewDropState {
    bool popup_open = false;
    std::string pending_media_id;
    std::vector<std::string> pending_media_ids;
};

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern std::unique_ptr<ump::TimelineView> timeline_view;
extern std::unique_ptr<ump::TimelinePlaybackController> scratch_timeline_controller;
extern std::unique_ptr<ump::TimelineThumbnailCache> timeline_thumbnail_cache;
extern std::unique_ptr<ump::TimelineCommandManager> timeline_command_manager;
extern ump::TimelinePlaybackController::DualViewTextures cached_dual_view_textures;
extern ColorCorrectedTextureCache g_color_corrected_cache;
extern ViewportDualViewDropState g_viewport_dv_drop;
extern bool otio_timeline_mode;
extern bool otio_dual_view_mode;
extern bool otio_dual_view_split_mode;
extern float otio_dual_view_split_pos;
extern CacheSettings cache_settings;
extern float g_font_scale;
extern bool g_skip_viewport_render_frame;

    void Application::CreateVideoViewport() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 2.0f));  // Override global 8,8 for viewport controls
        ImGui::PushStyleColor(ImGuiCol_Border, kTransparentBorder);
        if (ImGui::Begin("Video Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {

            // Calculate space for toolbar (only in annotation mode) and timeline at bottom
            // Transport and bottom toolbar rows use dampened scaling (less aggressive than full ui_scale)
            const float ui_scale = ImGui::GetIO().FontGlobalScale;
            const float scale_factor = 1.0f + (ui_scale - 1.0f) * 0.5f;  // Half the scaling effect
            const bool modal_edit_open = annotation_panel && annotation_panel->IsEditModalOpen();
            const float toolbar_height = (viewport_annotator && viewport_annotator->IsAnnotationMode() && !modal_edit_open) ? 36.0f * scale_factor : 0.0f;

            // Timeline height: dynamic based on mode
            // OTIO mode: expanded height for multi-track view
            // Dual view mode: expanded height for two tracks + bottom toolbar
            const float transport_row_h = 50.0f * scale_factor;  // Base 50px
            const float bottom_row_h = 36.0f * scale_factor;     // Base 36px
            float timeline_height = 0.0f;
            if (show_timeline_panel) {
                if (timeline_view && timeline_view->IsDualViewMode()) {
                    // Dual view timeline: overview + 2 tracks + transport + ruler + bottom toolbar
                    // Must match RenderOTIOTimelinePanel layout which includes overview minimap
                    timeline_height = transport_row_h +
                                     OTIOTimeline::OVERVIEW_TRACK_HEIGHT +    // Overview minimap track
                                     OTIOTimeline::TRACK_SEPARATOR_HEIGHT +   // Separator below overview
                                     2 * OTIOTimeline::TRACK_LANE_HEIGHT +    // LEFT + RIGHT tracks
                                     OTIOTimeline::TRACK_SEPARATOR_HEIGHT +   // Separator below tracks
                                     OTIOTimeline::TIMELINE_RULER_HEIGHT +
                                     bottom_row_h;
                } else if (otio_timeline_mode) {
                    // OTIO timeline needs more height for tracks
                    // Hide audio tracks in audio-only mode (VIDEO_FILE with no video tracks)
                    float track_count = 0.0f;
                    if (timeline_view) {
                        bool is_audio_only = timeline_view->GetSourceMode() == ump::TimelineSourceMode::VIDEO_FILE &&
                                             timeline_view->GetVideoTrackCount() == 0;
                        if (!is_audio_only) {
                            track_count = (float)(timeline_view->GetVideoTrackCount() + timeline_view->GetAudioTrackCount());
                        }
                    } else {
                        track_count = 5.0f;
                    }
                    // Add breadcrumb height when viewing nested timeline
                    float breadcrumb_height = (timeline_view && timeline_view->IsViewingNestedTimeline()) ? 58.0f : 0.0f;
                    timeline_height = transport_row_h +
                                     OTIOTimeline::OVERVIEW_TRACK_HEIGHT +    // Overview minimap track
                                     OTIOTimeline::TRACK_SEPARATOR_HEIGHT +   // Separator below overview
                                     (track_count > 0 ? track_count * OTIOTimeline::TRACK_LANE_HEIGHT +
                                      OTIOTimeline::TRACK_SEPARATOR_HEIGHT : 0) +
                                     OTIOTimeline::TIMELINE_RULER_HEIGHT +
                                     breadcrumb_height +
                                     bottom_row_h;
                } else {
                    // Standard timeline: transport + ruler + timeline + bottom toolbar
                    // Check if this is an image sequence (needs extra height for audio track)
                    bool is_image_sequence = video_player &&
                        (video_player->IsImageSequence() || video_player->IsInEXRMode());
                    float track_area_height = is_image_sequence ? 64.0f : 40.0f;  // Extra 24px for audio track

                    timeline_height = transport_row_h +
                                     OTIOTimeline::TIMELINE_RULER_HEIGHT +
                                     track_area_height +
                                     bottom_row_h;
                }
            }
            ImVec2 total_region = ImGui::GetContentRegionAvail();

            // Set up annotation toolbar callbacks (always when in annotation mode, so modal toolbar can use them too)
            if (viewport_annotator && viewport_annotator->IsAnnotationMode() && annotation_toolbar) {
                ump::Annotations::AnnotationToolbar::Callbacks callbacks;

                callbacks.on_tool_changed = [this](ump::Annotations::DrawingTool tool) {
                    if (viewport_annotator) {
                        viewport_annotator->SetActiveTool(tool);
                        Debug::Log("Tool changed");
                    }
                };

                callbacks.on_color_changed = [this](const ImVec4& color) {
                    if (viewport_annotator) {
                        viewport_annotator->SetDrawingColor(color);
                    }
                };

                callbacks.on_stroke_width_changed = [this](float width) {
                    if (viewport_annotator) {
                        viewport_annotator->SetStrokeWidth(width);
                    }
                };

                callbacks.on_fill_changed = [this](bool enabled) {
                    if (viewport_annotator) {
                        viewport_annotator->SetFillEnabled(enabled);
                    }
                };

                callbacks.on_done = [this]() {
                    Debug::Log("Done - saving annotation and exiting edit mode");

                    // Finalize any active stroke being drawn
                    auto active_stroke = viewport_annotator->FinalizeStroke();
                    if (active_stroke) {
                        // Add to current strokes
                        current_annotation_strokes_.push_back(*active_stroke);
                        Debug::Log("Added final stroke to annotation");
                    }

                    // Serialize all strokes to JSON
                    std::string json_data = ump::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);

                    // Save to annotation manager
                    if (annotation_manager && !current_editing_timecode_.empty()) {
                        annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);
                        Debug::Log("Saved " + std::to_string(current_annotation_strokes_.size()) + " strokes to annotation");
                    }

                    // Clear editing state
                    current_annotation_strokes_.clear();
                    current_editing_timecode_.clear();

                    // Exit annotation mode
                    viewport_annotator->SetMode(ump::Annotations::ViewportMode::PLAYBACK);
                    viewport_annotator->SetAllowInputInPopup(false);
                    annotation_toolbar->SetVisible(false);

                    // Close modal if open
                    if (annotation_panel && annotation_panel->IsEditModalOpen()) {
                        annotation_panel->CloseEditModal();
                    }
                };

                callbacks.on_cancel = [this]() {
                    Debug::Log("Cancel - discarding changes and exiting edit mode");

                    // Clear any active stroke
                    viewport_annotator->ClearActiveStroke();

                    // Discard all changes
                    current_annotation_strokes_.clear();
                    current_editing_timecode_.clear();

                    // Exit annotation mode without saving
                    viewport_annotator->SetMode(ump::Annotations::ViewportMode::PLAYBACK);
                    viewport_annotator->SetAllowInputInPopup(false);
                    annotation_toolbar->SetVisible(false);

                    // Close modal if open
                    if (annotation_panel && annotation_panel->IsEditModalOpen()) {
                        annotation_panel->CloseEditModal();
                    }
                };

                callbacks.on_undo = [this]() {
                    if (!annotation_undo_stack_.empty()) {
                        // Save current state to redo stack
                        annotation_redo_stack_.push_back(current_annotation_strokes_);

                        // Restore previous state
                        current_annotation_strokes_ = annotation_undo_stack_.back();
                        annotation_undo_stack_.pop_back();

                        Debug::Log("Undo - restored to " + std::to_string(current_annotation_strokes_.size()) + " strokes");
                    }
                };

                callbacks.on_redo = [this]() {
                    if (!annotation_redo_stack_.empty()) {
                        // Save current state to undo stack
                        annotation_undo_stack_.push_back(current_annotation_strokes_);

                        // Restore next state
                        current_annotation_strokes_ = annotation_redo_stack_.back();
                        annotation_redo_stack_.pop_back();

                        Debug::Log("Redo - restored to " + std::to_string(current_annotation_strokes_.size()) + " strokes");
                    }
                };

                callbacks.on_clear_all = [this]() {
                    Debug::Log("Clear all - clearing all strokes");
                    // Save current state to undo stack before clearing
                    if (!current_annotation_strokes_.empty()) {
                        annotation_undo_stack_.push_back(current_annotation_strokes_);
                        // Clear redo stack when a new action is performed
                        annotation_redo_stack_.clear();
                    }
                    // Clear all strokes
                    current_annotation_strokes_.clear();
                    viewport_annotator->ClearActiveStroke();
                };

                annotation_toolbar->SetCallbacks(callbacks);

                // Only render viewport toolbar when modal is NOT open (modal has its own toolbar)
                if (!(annotation_panel && annotation_panel->IsEditModalOpen())) {
                    ImGui::BeginChild("##ToolbarArea", ImVec2(0, toolbar_height), true,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                    // Add vertical spacing at top (scale with toolbar height)
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0f);
                    ImGui::AlignTextToFramePadding();

                    // Add left padding
                    const float toolbar_h_padding = 4.0f;
                    ImGui::Dummy(ImVec2(toolbar_h_padding, 0));
                    ImGui::SameLine();

                    // Render the toolbar inline
                    bool can_undo = !annotation_undo_stack_.empty();
                    bool can_redo = !annotation_redo_stack_.empty();

                    // Get system accent colors
                    ImVec4 accent_regular = GetWindowsAccentColor();
                    ImVec4 accent_muted_dark = MutedDark(GetWindowsAccentColor());

                    annotation_toolbar->Render(viewport_annotator.get(), can_undo, can_redo,
                                               font_icons, accent_regular, accent_muted_dark);

                    ImGui::EndChild();  // ##ToolbarArea
                }
            }

            // Create child window for video area
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::BeginChild("##VideoArea", ImVec2(0, total_region.y - timeline_height - toolbar_height), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            ImVec2 content_region = ImGui::GetContentRegionAvail();
            ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
            ImVec2 canvas_size = ImGui::GetContentRegionAvail();

            DrawVideoBackground(canvas_pos, canvas_size, 20.0f);

            // Process annotation drawing input if in annotation mode
            if (viewport_annotator && viewport_annotator->IsAnnotationMode() && video_player) {
                int video_width = video_player->GetVideoWidth();
                int video_height = video_player->GetVideoHeight();

                if (video_width > 0 && video_height > 0) {
                    ImVec2 display_pos, display_size;

                    // When modal is open, route input to modal image coordinates
                    if (annotation_panel && annotation_panel->IsEditModalOpen()) {
                        display_pos = annotation_panel->GetModalImageScreenPos();
                        display_size = annotation_panel->GetModalImageScreenSize();
                    } else {
                        // Calculate display bounds (same logic as video rendering)
                        float aspect_ratio = (float)video_width / video_height;
                        display_size = canvas_size;

                        if (canvas_size.x / canvas_size.y > aspect_ratio) {
                            display_size.x = canvas_size.y * aspect_ratio;
                        } else {
                            display_size.y = canvas_size.x / aspect_ratio;
                        }

                        display_pos = ImVec2(
                            canvas_pos.x + (canvas_size.x - display_size.x) * 0.5f,
                            canvas_pos.y + (canvas_size.y - display_size.y) * 0.5f
                        );
                    }

                    // Store display area for consistent NanoVG rendering later
                    nvg_display_pos_ = display_pos;
                    nvg_display_size_ = display_size;

                    // Process drawing input
                    viewport_annotator->ProcessInput(display_pos, display_size);

                    // Handle keyboard shortcuts for undo/redo
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                        // Ctrl+Z - Undo
                        if (!annotation_undo_stack_.empty()) {
                            // Save current state to redo stack
                            annotation_redo_stack_.push_back(current_annotation_strokes_);

                            // Restore previous state
                            current_annotation_strokes_ = annotation_undo_stack_.back();
                            annotation_undo_stack_.pop_back();

                            Debug::Log("Undo (Ctrl+Z) - restored to " + std::to_string(current_annotation_strokes_.size()) + " strokes");
                        }
                    }
                    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                        // Ctrl+Y - Redo
                        if (!annotation_redo_stack_.empty()) {
                            // Save current state to undo stack
                            annotation_undo_stack_.push_back(current_annotation_strokes_);

                            // Restore next state
                            current_annotation_strokes_ = annotation_redo_stack_.back();
                            annotation_redo_stack_.pop_back();

                            Debug::Log("Redo (Ctrl+Y) - restored to " + std::to_string(current_annotation_strokes_.size()) + " strokes");
                        }
                    }

                    // Check if a stroke was just completed
                    const auto* active_stroke = viewport_annotator->GetActiveStroke();
                    if (active_stroke && active_stroke->is_complete) {
                        // Stroke is complete - add it to our collection
                        auto finalized_stroke = viewport_annotator->FinalizeStroke();
                        if (finalized_stroke) {
                            // Save current state to undo stack before adding new stroke
                            annotation_undo_stack_.push_back(current_annotation_strokes_);
                            // Clear redo stack when a new action is performed
                            annotation_redo_stack_.clear();

                            current_annotation_strokes_.push_back(*finalized_stroke);
                            Debug::Log("Stroke completed and added to annotation");
                        }
                    }
                }
            }

            // Try to use cached frame during scrubbing for instant feedback
            // Skip seek cache in dual view mode - let normal video rendering handle both sides
            bool used_cached_frame = false;
            if (timeline_manager &&
                (timeline_manager->IsScrubbing() || timeline_manager->IsHoldingCachedFrame()) &&
                !(timeline_view && timeline_view->IsDualViewMode())) {
                GLuint cached_texture_id = 0;
                int cached_width = 0, cached_height = 0;
                double current_time = timeline_manager->GetUIPosition();

                if (timeline_manager->GetCachedFrameForScrubbing(current_time, cached_texture_id, cached_width, cached_height)) {
                    // Use cached frame for instant scrubbing feedback
                    // Calculate display size maintaining aspect ratio
                    float aspect_ratio = (float)cached_width / cached_height;
                    ImVec2 display_size = canvas_size;

                    if (canvas_size.x / aspect_ratio <= canvas_size.y) {
                        display_size.y = canvas_size.x / aspect_ratio;
                    }
                    else {
                        display_size.x = canvas_size.y * aspect_ratio;
                    }

                    // Center the image
                    ImVec2 display_pos = ImVec2(
                        canvas_pos.x + (canvas_size.x - display_size.x) * 0.5f,
                        canvas_pos.y + (canvas_size.y - display_size.y) * 0.5f
                    );

                    // Render cached frame with OCIO color correction if available
                    // NOTE: Color correction is pre-rendered before ImGui::NewFrame() to avoid GL ops during render
                    GLuint display_texture = cached_texture_id;  // Default to original texture

                    if (video_player && video_player->HasColorPipeline()) {
                        // Use pre-rendered color-corrected texture (with fence-sync fallback)
                        GLuint cc_texture = g_color_corrected_cache.GetCachedFrameTexture();
                        if (cc_texture != 0 && g_color_corrected_cache.cached_frame_source_id == cached_texture_id) {
                            display_texture = cc_texture;
                        }
                    }

                    // Render the display texture (either original or color-corrected)
                    ImGui::GetWindowDrawList()->AddImage(
                        (void*)(intptr_t)display_texture,
                        display_pos,
                        ImVec2(display_pos.x + display_size.x, display_pos.y + display_size.y)
                    );

                    used_cached_frame = true;
                }
            }

            // Fallback to normal video rendering if no cached frame available
            if (!used_cached_frame) {
                // In dual view mode, always render both videos (don't show black screen)
                bool is_dual_view = timeline_view && timeline_view->IsDualViewMode();
                bool is_otio_dual_view = otio_dual_view_mode && is_dual_view;

                // Timeline Playback Mode: Frame injection now handled by VideoPlayer::InjectCurrentTimelineFrame()
                // which is called from UpdateVideoTexture() (matches EXR pattern for smooth playback)
                // The timeline frame is injected into video_texture, so normal video rendering works

                // OTIO Dual View Mode: Render split viewport with LEFT/RIGHT tracks
                if (is_otio_dual_view) {
                    // Skip viewport rendering for one frame after ForceReloadCurrentMedia
                    // This prevents using stale/recycled texture IDs that could cause GL corruption
                    if (g_skip_viewport_render_frame) {
                        g_skip_viewport_render_frame = false;
                        // Just draw background, don't try to render any textures
                        DrawVideoBackground(canvas_pos, canvas_size);
                        // Continue to next frame
                    } else {
                    ImDrawList* draw_list = ImGui::GetWindowDrawList();

                    // Get textures from dual view playback controller
                    // NOTE: Using cached textures that were prepared BEFORE ImGui::NewFrame()
                    // to avoid GL state corruption during render
                    GLuint left_texture = 0, right_texture = 0;
                    int left_width = 0, left_height = 0;
                    int right_width = 0, right_height = 0;

                    // Unified composite texture mode (single D3D11 interop)
                    bool use_unified_composite = false;
                    GLuint composite_texture = 0;
                    int composite_width = 0, composite_height = 0;

                    if (scratch_timeline_controller && scratch_timeline_controller->IsDualViewMode()) {
                        // Check if using unified composite pipeline
                        use_unified_composite = cached_dual_view_textures.is_unified;

                        if (use_unified_composite) {
                            // Unified mode: single composite texture with UV sampling
                            composite_texture = cached_dual_view_textures.composite_texture;
                            composite_width = cached_dual_view_textures.composite_width;
                            composite_height = cached_dual_view_textures.composite_height;
                        }

                        // Always get individual dimensions for aspect calculations
                        left_texture = cached_dual_view_textures.left_texture;
                        left_width = cached_dual_view_textures.left_width;
                        left_height = cached_dual_view_textures.left_height;
                        right_texture = cached_dual_view_textures.right_texture;
                        right_width = cached_dual_view_textures.right_width;
                        right_height = cached_dual_view_textures.right_height;
                    }

                    // Calculate aspect-ratio-correct size for full canvas (split mode)
                    auto calculate_fit_size = [](int src_w, int src_h, float max_w, float max_h) -> ImVec2 {
                        if (src_w == 0 || src_h == 0) return ImVec2(max_w, max_h);
                        float aspect = (float)src_w / (float)src_h;
                        if (max_w / max_h > aspect) {
                            return ImVec2(max_h * aspect, max_h);
                        } else {
                            return ImVec2(max_w, max_w / aspect);
                        }
                    };

                    // Fill background using user's selected background type
                    DrawVideoBackground(canvas_pos, canvas_size);

                    // NOTE: Color-corrected textures are now queued to g_pending_texture_deletions
                    // and deleted BEFORE ImGui::NewFrame() to avoid GL ops during render

                    if (otio_dual_view_split_mode) {
                        // ============================================================
                        // SPLIT MODE: Vertical wipe with draggable divider
                        // Both textures rendered at full size, split by divider
                        // ============================================================
                        float split_x = canvas_pos.x + canvas_size.x * otio_dual_view_split_pos;

                        // Calculate full-canvas fit size (both sides use same size/position)
                        ImVec2 full_size = calculate_fit_size(
                            left_width > 0 ? left_width : right_width,
                            left_height > 0 ? left_height : right_height,
                            canvas_size.x, canvas_size.y
                        );
                        ImVec2 img_pos = ImVec2(
                            canvas_pos.x + (canvas_size.x - full_size.x) * 0.5f,
                            canvas_pos.y + (canvas_size.y - full_size.y) * 0.5f
                        );

                        // LEFT side (up to split line)
                        // NOTE: Color correction is pre-rendered with fence-sync fallback
                        if ((use_unified_composite && composite_texture != 0) ||
                            (left_texture != 0 && left_width > 0 && left_height > 0)) {

                            GLuint display_left = left_texture;
                            GLuint cc_left = g_color_corrected_cache.GetLeftTexture();
                            GLuint cc_composite = g_color_corrected_cache.GetCompositeTexture();

                            // In unified mode, use color-corrected composite texture if available
                            if (use_unified_composite && composite_texture != 0) {
                                if (video_player && video_player->HasColorPipeline() && cc_composite != 0) {
                                    display_left = cc_composite;
                                } else {
                                    display_left = composite_texture;
                                }
                            } else if (video_player && video_player->HasColorPipeline() && cc_left != 0) {
                                display_left = cc_left;
                            }

                            // Clip to left of split line
                            ImVec2 clip_min = ImVec2(canvas_pos.x, canvas_pos.y);
                            ImVec2 clip_max = ImVec2(split_x, canvas_pos.y + canvas_size.y);
                            draw_list->PushClipRect(clip_min, clip_max, true);

                            if (use_unified_composite && composite_texture != 0) {
                                // Use UV coordinates to sample left half from composite
                                ImVec2 uv0(cached_dual_view_textures.left_uv_min_x,
                                           cached_dual_view_textures.left_uv_min_y);
                                ImVec2 uv1(cached_dual_view_textures.left_uv_max_x,
                                           cached_dual_view_textures.left_uv_max_y);
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_left,
                                    img_pos,
                                    ImVec2(img_pos.x + full_size.x, img_pos.y + full_size.y),
                                    uv0, uv1
                                );
                            } else {
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_left,
                                    img_pos,
                                    ImVec2(img_pos.x + full_size.x, img_pos.y + full_size.y)
                                );
                            }

                            draw_list->PopClipRect();
                        }

                        // RIGHT side (from split line)
                        // NOTE: Color correction is pre-rendered with fence-sync fallback
                        if ((use_unified_composite && composite_texture != 0) ||
                            (right_texture != 0 && right_width > 0 && right_height > 0)) {

                            GLuint display_right = right_texture;
                            GLuint cc_right = g_color_corrected_cache.GetRightTexture();
                            GLuint cc_composite = g_color_corrected_cache.GetCompositeTexture();

                            // In unified mode, use color-corrected composite texture if available
                            if (use_unified_composite && composite_texture != 0) {
                                if (video_player && video_player->HasColorPipeline() && cc_composite != 0) {
                                    display_right = cc_composite;
                                } else {
                                    display_right = composite_texture;
                                }
                            } else if (video_player && video_player->HasColorPipeline() && cc_right != 0) {
                                display_right = cc_right;
                            }

                            // Clip to right of split line
                            ImVec2 clip_min = ImVec2(split_x, canvas_pos.y);
                            ImVec2 clip_max = ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y);
                            draw_list->PushClipRect(clip_min, clip_max, true);

                            if (use_unified_composite && composite_texture != 0) {
                                // Use UV coordinates to sample right half from composite
                                ImVec2 uv0(cached_dual_view_textures.right_uv_min_x,
                                           cached_dual_view_textures.right_uv_min_y);
                                ImVec2 uv1(cached_dual_view_textures.right_uv_max_x,
                                           cached_dual_view_textures.right_uv_max_y);
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_right,
                                    img_pos,
                                    ImVec2(img_pos.x + full_size.x, img_pos.y + full_size.y),
                                    uv0, uv1
                                );
                            } else {
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_right,
                                    img_pos,
                                    ImVec2(img_pos.x + full_size.x, img_pos.y + full_size.y)
                                );
                            }

                            draw_list->PopClipRect();
                        }

                        // Draw vertical split line (draggable)
                        float divider_half_width = 2.0f;
                        ImVec2 divider_min = ImVec2(split_x - divider_half_width, canvas_pos.y);
                        ImVec2 divider_max = ImVec2(split_x + divider_half_width, canvas_pos.y + canvas_size.y);

                        // Invisible button for dragging
                        ImGui::SetCursorScreenPos(ImVec2(split_x - 8, canvas_pos.y));
                        ImGui::InvisibleButton("##split_divider", ImVec2(16, canvas_size.y));
                        bool divider_hovered = ImGui::IsItemHovered();
                        bool divider_active = ImGui::IsItemActive();

                        if (divider_hovered || divider_active) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        }

                        if (divider_active && ImGui::IsMouseDragging(0)) {
                            float new_x = ImGui::GetMousePos().x;
                            otio_dual_view_split_pos = std::clamp(
                                (new_x - canvas_pos.x) / canvas_size.x,
                                0.05f, 0.95f
                            );
                        }

                        // Draw divider line
                        ImU32 divider_color = (divider_hovered || divider_active) ?
                            IM_COL32(200, 200, 200, 255) : IM_COL32(120, 120, 120, 255);
                        draw_list->AddLine(
                            ImVec2(split_x, canvas_pos.y),
                            ImVec2(split_x, canvas_pos.y + canvas_size.y),
                            divider_color, divider_active ? 3.0f : 2.0f
                        );

                    } else {
                        // ============================================================
                        // DUAL MODE: Side-by-side with each video in its own half
                        // ============================================================
                        float half_width = canvas_size.x * 0.5f;
                        float mid_x = canvas_pos.x + half_width;

                        // LEFT half rendering with OCIO color correction
                        // NOTE: Color correction is pre-rendered before ImGui::NewFrame()
                        if ((use_unified_composite && composite_texture != 0) ||
                            (left_texture != 0 && left_width > 0 && left_height > 0)) {

                            ImVec2 left_size = calculate_fit_size(left_width, left_height, half_width - 4, canvas_size.y);
                            ImVec2 left_pos = ImVec2(
                                canvas_pos.x + (half_width - left_size.x) * 0.5f,
                                canvas_pos.y + (canvas_size.y - left_size.y) * 0.5f
                            );

                            GLuint display_left = left_texture;
                            GLuint cc_left = g_color_corrected_cache.GetLeftTexture();
                            GLuint cc_composite = g_color_corrected_cache.GetCompositeTexture();

                            // In unified mode, use color-corrected composite texture if available
                            if (use_unified_composite && composite_texture != 0) {
                                if (video_player && video_player->HasColorPipeline() && cc_composite != 0) {
                                    display_left = cc_composite;
                                } else {
                                    display_left = composite_texture;
                                }
                            } else if (video_player && video_player->HasColorPipeline() && cc_left != 0) {
                                display_left = cc_left;
                            }

                            if (use_unified_composite && composite_texture != 0) {
                                // Use UV coordinates to sample left half from composite
                                ImVec2 uv0(cached_dual_view_textures.left_uv_min_x,
                                           cached_dual_view_textures.left_uv_min_y);
                                ImVec2 uv1(cached_dual_view_textures.left_uv_max_x,
                                           cached_dual_view_textures.left_uv_max_y);
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_left,
                                    left_pos,
                                    ImVec2(left_pos.x + left_size.x, left_pos.y + left_size.y),
                                    uv0, uv1
                                );
                            } else {
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_left,
                                    left_pos,
                                    ImVec2(left_pos.x + left_size.x, left_pos.y + left_size.y)
                                );
                            }
                        }

                        // RIGHT half rendering with OCIO color correction
                        // NOTE: Color correction is pre-rendered with fence-sync fallback
                        if ((use_unified_composite && composite_texture != 0) ||
                            (right_texture != 0 && right_width > 0 && right_height > 0)) {

                            ImVec2 right_size = calculate_fit_size(right_width, right_height, half_width - 4, canvas_size.y);
                            ImVec2 right_pos = ImVec2(
                                mid_x + (half_width - right_size.x) * 0.5f,
                                canvas_pos.y + (canvas_size.y - right_size.y) * 0.5f
                            );

                            GLuint display_right = right_texture;
                            GLuint cc_right = g_color_corrected_cache.GetRightTexture();
                            GLuint cc_composite = g_color_corrected_cache.GetCompositeTexture();

                            // In unified mode, use color-corrected composite texture if available
                            if (use_unified_composite && composite_texture != 0) {
                                if (video_player && video_player->HasColorPipeline() && cc_composite != 0) {
                                    display_right = cc_composite;
                                } else {
                                    display_right = composite_texture;
                                }
                            } else if (video_player && video_player->HasColorPipeline() && cc_right != 0) {
                                display_right = cc_right;
                            }

                            if (use_unified_composite && composite_texture != 0) {
                                // Use UV coordinates to sample right half from composite
                                ImVec2 uv0(cached_dual_view_textures.right_uv_min_x,
                                           cached_dual_view_textures.right_uv_min_y);
                                ImVec2 uv1(cached_dual_view_textures.right_uv_max_x,
                                           cached_dual_view_textures.right_uv_max_y);
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_right,
                                    right_pos,
                                    ImVec2(right_pos.x + right_size.x, right_pos.y + right_size.y),
                                    uv0, uv1
                                );
                            } else {
                                draw_list->AddImage(
                                    (ImTextureID)(intptr_t)display_right,
                                    right_pos,
                                    ImVec2(right_pos.x + right_size.x, right_pos.y + right_size.y)
                                );
                            }
                        }

                        // Draw vertical separator line in the middle
                        draw_list->AddLine(
                            ImVec2(mid_x, canvas_pos.y),
                            ImVec2(mid_x, canvas_pos.y + canvas_size.y),
                            IM_COL32(80, 80, 80, 255), 2.0f
                        );
                    }
                    }  // End of else block (skip viewport render check)
                }
                // Standard video rendering (works for both normal video and timeline mode)
                else {
                    if (timeline_manager && timeline_manager->IsHoldingCachedFrame() && !is_dual_view) {
                        // Don't render video frame while holding cached frame to avoid flashing old frame
                        // Draw solid black to prevent OpenGL texture flash
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        draw_list->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                            IM_COL32(0, 0, 0, 255));
                    }
                    else {
                        video_player->RenderVideoFrame();
                    }
                }

                // Render annotation overlays (MIDDLE LAYER - after video, before safety overlays)
                if (annotation_renderer && viewport_annotator) {
                    // Adjust canvas for dual view mode (annotations only on left side)
                    ImVec2 annotation_canvas_pos = canvas_pos;
                    ImVec2 annotation_canvas_size = canvas_size;

                    if (timeline_view && timeline_view->IsDualViewMode()) {
                        // Render annotations only on left half (primary video) in dual view mode
                        annotation_canvas_size.x = canvas_size.x * 0.5f;
                    }

                    // Calculate display size/position (same logic as video rendering)
                    if (video_player) {
                        int video_width = video_player->GetVideoWidth();
                        int video_height = video_player->GetVideoHeight();

                        if (video_width > 0 && video_height > 0) {
                            float aspect_ratio = (float)video_width / video_height;
                            ImVec2 display_size = annotation_canvas_size;

                            if (annotation_canvas_size.x / annotation_canvas_size.y > aspect_ratio) {
                                display_size.x = annotation_canvas_size.y * aspect_ratio;
                            } else {
                                display_size.y = annotation_canvas_size.x / aspect_ratio;
                            }

                            ImVec2 display_pos = ImVec2(
                                annotation_canvas_pos.x + (annotation_canvas_size.x - display_size.x) * 0.5f,
                                annotation_canvas_pos.y + (annotation_canvas_size.y - display_size.y) * 0.5f
                            );

                                // Save display area for export capture cropping
                                if (pending_capture.pending) {
                                    pending_capture.display_pos = display_pos;
                                    pending_capture.display_size = display_size;
                                    pending_capture.viewport_width_at_creation = display_size.x;  // Capture viewport width for line scaling
                                    Debug::Log("Set display area for capture: pos(" +
                                              std::to_string(display_pos.x) + "," + std::to_string(display_pos.y) +
                                              ") size(" + std::to_string(display_size.x) + "x" + std::to_string(display_size.y) +
                                              ") canvas_pos(" + std::to_string(canvas_pos.x) + "," + std::to_string(canvas_pos.y) +
                                              ") canvas_size(" + std::to_string(canvas_size.x) + "x" + std::to_string(canvas_size.y) + ")");
                                }

                                // Only render annotations if we're in annotation mode (editing)
                                // OR if there's a saved annotation at the current frame
                                bool should_render_annotations = false;
                                bool has_strokes_to_render = false;

                                if (viewport_annotator->IsAnnotationMode()) {
                                    // Always render in annotation mode
                                    should_render_annotations = true;
                                    has_strokes_to_render = true;
                                } else if (annotations_enabled && annotation_manager && video_player) {
                                    // Check if current frame has an annotation to display (only if annotations enabled)
                                    int current_frame = video_player->GetCurrentFrame();
                                    const auto& notes = annotation_manager->GetNotes();
                                    for (const auto& note : notes) {
                                        if (note.frame == current_frame && !note.annotation_data.empty()) {
                                            has_strokes_to_render = true;
                                            break;
                                        }
                                    }
                                }

                                // Also check pending capture strokes
                                if (pending_capture.pending && !pending_capture.strokes.empty()) {
                                    has_strokes_to_render = true;
                                }

                                // Collect strokes for deferred NanoVG rendering (after ImGui::Render)
                                if (has_strokes_to_render) {
                                    nvg_strokes_to_render_.clear();

                                    // When modal is open, NanoVG must render onto the modal image rect
                                    // (already set during input routing). Don't overwrite with viewport coords.
                                    if (annotation_panel && annotation_panel->IsEditModalOpen()) {
                                        nvg_display_pos_ = annotation_panel->GetModalImageScreenPos();
                                        nvg_display_size_ = annotation_panel->GetModalImageScreenSize();
                                    } else {
                                        nvg_display_pos_ = display_pos;
                                        nvg_display_size_ = display_size;
                                    }
                                    nvg_video_width_ = video_width;  // Store for line width scaling

                                    // Collect saved annotations at current frame (when not in edit mode)
                                    if (!viewport_annotator->IsAnnotationMode() && annotations_enabled && annotation_manager && video_player) {
                                        int current_frame = video_player->GetCurrentFrame();
                                        const auto& notes = annotation_manager->GetNotes();
                                        for (const auto& note : notes) {
                                            if (note.frame == current_frame && !note.annotation_data.empty()) {
                                                auto strokes = ump::Annotations::AnnotationSerializer::JsonStringToStrokes(note.annotation_data);
                                                for (const auto& stroke : strokes) {
                                                    nvg_strokes_to_render_.push_back(stroke);
                                                }
                                                break;
                                            }
                                        }
                                    }

                                    // Collect editing strokes (only in annotation mode)
                                    if (viewport_annotator->IsAnnotationMode()) {
                                        // Collect all stored strokes for the current annotation being edited
                                        for (const auto& stroke : current_annotation_strokes_) {
                                            nvg_strokes_to_render_.push_back(stroke);
                                        }

                                        // Collect active stroke being drawn (if any)
                                        const auto* active_stroke = viewport_annotator->GetActiveStroke();
                                        if (active_stroke && !active_stroke->points.empty()) {
                                            nvg_strokes_to_render_.push_back(*active_stroke);
                                        }
                                    }

                                    // Collect pending capture strokes (for export)
                                    if (pending_capture.pending && !pending_capture.strokes.empty()) {
                                        for (const auto& stroke : pending_capture.strokes) {
                                            nvg_strokes_to_render_.push_back(stroke);
                                        }
                                    }

                                    pending_nvg_render_ = !nvg_strokes_to_render_.empty();
                                } else {
                                    pending_nvg_render_ = false;
                                }
                            }
                        }
                    }

                    // Process export state machine (queues captures as needed)
                    ProcessExportStateMachine();

                    // Process Frame.io thumbnail generation (once per frame)
                    ProcessFrameioThumbnailGeneration();

                    // Capture frame if requested (after all annotations are rendered)
                    CaptureRenderedFrame();

                    // Render SVG safety overlays on top of video (TOP LAYER - always last!)
                    if (video_player->IsSafetyOverlaysEnabled()) {
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        ImU32 overlay_color = IM_COL32(
                            (int)(safety_settings.color[0] * 255),
                            (int)(safety_settings.color[1] * 255),
                            (int)(safety_settings.color[2] * 255),
                            255
                        );

                        // Adjust canvas for dual view mode
                        ImVec2 overlay_canvas_pos = canvas_pos;
                        ImVec2 overlay_canvas_size = canvas_size;

                        if (timeline_view && timeline_view->IsDualViewMode()) {
                            if (otio_dual_view_split_mode) {
                                // SPLIT MODE: One overlay covering full canvas (both panels as one)
                                // Use full canvas_size - no adjustment needed
                                video_player->RenderSVGOverlays(draw_list, overlay_canvas_pos, overlay_canvas_size,
                                    safety_settings.opacity, overlay_color, safety_settings.line_width);
                            } else {
                                // SIDE-BY-SIDE MODE: Render overlay on both halves independently
                                float half_width = canvas_size.x * 0.5f;

                                // LEFT half overlay
                                ImVec2 left_canvas_pos = canvas_pos;
                                ImVec2 left_canvas_size = ImVec2(half_width, canvas_size.y);
                                video_player->RenderSVGOverlays(draw_list, left_canvas_pos, left_canvas_size,
                                    safety_settings.opacity, overlay_color, safety_settings.line_width);

                                // RIGHT half overlay
                                ImVec2 right_canvas_pos = ImVec2(canvas_pos.x + half_width, canvas_pos.y);
                                ImVec2 right_canvas_size = ImVec2(half_width, canvas_size.y);
                                video_player->RenderSVGOverlays(draw_list, right_canvas_pos, right_canvas_size,
                                    safety_settings.opacity, overlay_color, safety_settings.line_width);
                            }
                        } else {
                            // Normal single video mode
                            video_player->RenderSVGOverlays(draw_list, overlay_canvas_pos, overlay_canvas_size,
                                safety_settings.opacity, overlay_color, safety_settings.line_width);
                        }
                    }
                }


            // ImGui requires a Dummy() item after using GetCursorScreenPos() to properly size the window
            ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y));
            ImGui::Dummy(ImVec2(0, 0));

            // =====================================================================
            // DROP TARGET FOR MEDIA FROM PROJECT PANEL
            // =====================================================================
            // Behavior depends on current mode:
            // - PLAYLIST mode: Add to playlist (unless dropping a Playlist/DualView)
            // - DUAL_VIEW mode: Show popup to choose LEFT or RIGHT track (unless dropping a Playlist/DualView)
            // - Other modes / nothing loaded: Load media normally
            // Place invisible button over entire video area for drop detection
            ImGui::SetCursorScreenPos(canvas_pos);
            ImGui::InvisibleButton("##ViewportDropTarget", canvas_size);

            if (ImGui::BeginDragDropTarget()) {
                // Helper lambda to check if item should be loaded normally (Playlist/DualView items always load)
                auto should_load_normally = [](ump::MediaItem* item, ump::TimelineSourceMode current_mode) {
                    if (!item) return true;
                    // Playlist and DualView items always load normally (replace current view)
                    if (item->type == ump::MediaType::PLAYLIST || item->type == ump::MediaType::DUAL_VIEW) {
                        return true;
                    }
                    // If not in special mode, load normally
                    if (current_mode != ump::TimelineSourceMode::PLAYLIST &&
                        current_mode != ump::TimelineSourceMode::DUAL_VIEW) {
                        return true;
                    }
                    return false;
                };

                // Helper lambda to add item to playlist
                auto add_to_playlist = [&](const std::string& media_id) {
                    ump::MediaItem* item = project_manager->GetMediaItem(media_id);
                    if (!item) return;
                    // Skip non-playable types
                    if (item->type == ump::MediaType::PLAYLIST ||
                        item->type == ump::MediaType::DUAL_VIEW ||
                        item->type == ump::MediaType::IMAGE) return;

                    ump::MediaItem* playlist = timeline_view->GetSourceMediaItem();
                    if (playlist && playlist->type == ump::MediaType::PLAYLIST) {
                        Debug::Log("[VIEWPORT DROP] Before add: playlist '" + playlist->name +
                                   "' has " + std::to_string(playlist->playlist_items.size()) + " items, ptr=" +
                                   std::to_string(reinterpret_cast<uintptr_t>(playlist)));
                        ump::PlaylistItemEntry entry;
                        entry.media_id = media_id;
                        entry.in_point = -1.0;
                        entry.out_point = -1.0;
                        playlist->playlist_items.push_back(entry);
                        Debug::Log("[VIEWPORT DROP] After add: playlist now has " +
                                   std::to_string(playlist->playlist_items.size()) + " items");
                    }
                };

                // Helper lambda to reload playlist after adding items
                auto reload_playlist = [&]() {
                    ump::MediaItem* playlist = timeline_view->GetSourceMediaItem();
                    if (!playlist) return;
                    timeline_view->SetProjectManager(project_manager.get());
                    timeline_view->LoadPlaylistAsTimeline(playlist);
                    timeline_view->InitializePlayback();
                    otio_timeline_mode = true;
                    if (video_player) {
                        video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                    }
                    if (auto* controller = timeline_view->GetPlaybackController()) {
                        controller->Pause();
                    }
                    timeline_view->RequestFitZoomOnNextRender();
                };

                // Get current source mode
                ump::TimelineSourceMode current_mode = timeline_view ? timeline_view->GetSourceMode() : ump::TimelineSourceMode::VIDEO_FILE;

                // Accept single media item
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
                    std::string media_id(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                    Debug::Log("[VIEWPORT DROP] Received media: " + media_id);

                    if (project_manager) {
                        ump::MediaItem* item = project_manager->GetMediaItem(media_id);
                        if (item) {
                            if (should_load_normally(item, current_mode)) {
                                // Load based on type (same as double-click behavior)
                                if (item->type == ump::MediaType::DUAL_VIEW) {
                                    project_manager->OpenDualViewInEditor(item->timeline_id);
                                } else if (item->type == ump::MediaType::PLAYLIST) {
                                    project_manager->OpenPlaylistInPanel(item->id);
                                } else {
                                    project_manager->LoadSingleMediaItem(*item);
                                }
                            } else if (current_mode == ump::TimelineSourceMode::PLAYLIST) {
                                // Add to current playlist
                                add_to_playlist(media_id);
                                reload_playlist();
                            } else if (current_mode == ump::TimelineSourceMode::DUAL_VIEW) {
                                // Open popup to choose LEFT or RIGHT track
                                g_viewport_dv_drop.pending_media_id = media_id;
                                g_viewport_dv_drop.pending_media_ids.clear();
                                g_viewport_dv_drop.popup_open = true;
                                ImGui::OpenPopup("ViewportDualViewDropPopup");
                            }
                        }
                    }
                }

                // Accept multi-select
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEMS_MULTI")) {
                    std::string payload_str(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                    Debug::Log("[VIEWPORT DROP] Received multi-select: " + payload_str);

                    // Parse all items
                    std::vector<std::string> media_ids;
                    std::istringstream ss(payload_str);
                    std::string id;
                    while (std::getline(ss, id, ';')) {
                        if (!id.empty()) media_ids.push_back(id);
                    }

                    if (project_manager && !media_ids.empty()) {
                        // Check first item to determine behavior
                        ump::MediaItem* first_item = project_manager->GetMediaItem(media_ids[0]);
                        if (first_item) {
                            if (should_load_normally(first_item, current_mode)) {
                                // Load first item (same as double-click behavior)
                                if (first_item->type == ump::MediaType::DUAL_VIEW) {
                                    project_manager->OpenDualViewInEditor(first_item->timeline_id);
                                } else if (first_item->type == ump::MediaType::PLAYLIST) {
                                    project_manager->OpenPlaylistInPanel(first_item->id);
                                } else {
                                    project_manager->LoadSingleMediaItem(*first_item);
                                }
                            } else if (current_mode == ump::TimelineSourceMode::PLAYLIST) {
                                // Add all items to playlist
                                for (const auto& mid : media_ids) {
                                    add_to_playlist(mid);
                                }
                                reload_playlist();
                            } else if (current_mode == ump::TimelineSourceMode::DUAL_VIEW) {
                                // For dual view, only use first item (can't add multiple)
                                g_viewport_dv_drop.pending_media_id = media_ids[0];
                                g_viewport_dv_drop.pending_media_ids.clear();
                                g_viewport_dv_drop.popup_open = true;
                                ImGui::OpenPopup("ViewportDualViewDropPopup");
                            }
                        }
                    }
                }

                ImGui::EndDragDropTarget();
            }

            // Dual View drop modal - appears after dropping media on viewport in DUAL_VIEW mode
            // Uses a proper modal with darkened background for visibility
            if (g_viewport_dv_drop.popup_open) {
                ImGui::OpenPopup("Add to Dual View");
                g_viewport_dv_drop.popup_open = false;
            }

            // Center the modal on screen
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

            // Add padding around modal content
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));

            if (ImGui::BeginPopupModal("Add to Dual View", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {

                // Get media item name for display
                std::string item_name = "Media";
                if (project_manager && !g_viewport_dv_drop.pending_media_id.empty()) {
                    ump::MediaItem* item = project_manager->GetMediaItem(g_viewport_dv_drop.pending_media_id);
                    if (item) item_name = item->name;
                }

                ImGui::Text("Add media to which track?");
                ImGui::Spacing();
                ImGui::TextDisabled("Item: %s", item_name.c_str());
                ImGui::Spacing();
                ImGui::Spacing();

                // Track selection buttons - side by side
                const float button_width = 140.0f;
                const float button_height = 32.0f;

                PushOutlineButtonStyle();
                bool add_to_left = ImGui::Button("LEFT Track", ImVec2(button_width, button_height));
                ImGui::SameLine();
                bool add_to_right = ImGui::Button("RIGHT Track", ImVec2(button_width, button_height));
                PopOutlineButtonStyle();

                if (add_to_left || add_to_right) {
                    int target_track = add_to_left ? 0 : 1;  // LEFT = track 0, RIGHT = track 1

                    if (project_manager && timeline_view && timeline_command_manager &&
                        !g_viewport_dv_drop.pending_media_id.empty()) {
                        ump::MediaItem* item = project_manager->GetMediaItem(g_viewport_dv_drop.pending_media_id);
                        if (item && item->type != ump::MediaType::PLAYLIST &&
                            item->type != ump::MediaType::DUAL_VIEW &&
                            item->type != ump::MediaType::IMAGE) {

                            // Create OTIOClip from MediaItem
                            ump::OTIOClip clip;
                            clip.id = "";
                            clip.name = item->name;

                            // Determine file path based on media type
                            if (item->type == ump::MediaType::IMAGE_SEQUENCE || item->type == ump::MediaType::EXR_SEQUENCE) {
                                clip.file_path = item->ffmpeg_pattern;
                                clip.is_sequence = true;
                                clip.sequence_directory = item->image_seq.directory;
                                clip.sequence_pattern = item->image_seq.pattern;
                                clip.sequence_start_frame = item->image_seq.start_frame;
                                clip.sequence_end_frame = item->image_seq.end_frame;
                                if (item->type == ump::MediaType::EXR_SEQUENCE) {
                                    clip.sequence_exr_layer = item->image_seq.layer;
                                }
                            } else {
                                clip.file_path = item->path;
                            }

                            // Dual view clips always start at 0
                            clip.start_time = 0.0;
                            clip.duration = item->duration;
                            clip.source_in = 0.0;
                            clip.source_out = item->duration;
                            clip.is_gap = false;
                            clip.linked_path = clip.file_path;
                            clip.is_linked = true;
                            clip.source_fps = item->frame_rate > 0 ? item->frame_rate : timeline_view->GetFrameRate();
                            clip.source_width = (item->sequence_width > 0) ? item->sequence_width : item->timeline_width;
                            clip.source_height = (item->sequence_height > 0) ? item->sequence_height : item->timeline_height;
                            clip.source_duration = item->duration;

                            // Register sequence metadata if needed
                            if (clip.is_sequence) {
                                auto* ctrl = timeline_view->GetEffectivePlaybackController();
                                if (ctrl && ctrl->GetCache()) {
                                    ump::SequenceMetadata seq_meta;
                                    seq_meta.directory = clip.sequence_directory;
                                    seq_meta.pattern = clip.sequence_pattern;
                                    seq_meta.start_frame = clip.sequence_start_frame;
                                    seq_meta.end_frame = clip.sequence_end_frame;
                                    seq_meta.exr_layer = clip.sequence_exr_layer;
                                    seq_meta.pipeline_mode = item->image_seq.pipeline_mode;
                                    seq_meta.valid = true;
                                    ctrl->GetCache()->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                                }
                            }

                            // Probe for audio
                            if (!clip.is_sequence && ump::MediaLinker::IsVideoFile(clip.file_path)) {
                                ump::VideoProbeResult probe = ump::MediaLinker::ProbeVideoFile(clip.file_path);
                                if (probe.valid) clip.has_audio = probe.has_audio;
                            }

                            // REPLACE the track content (only one clip per track allowed in dual view)
                            // Clear the track first, then add the new clip
                            auto& tracks = timeline_view->GetTracks();
                            if (target_track >= 0 && target_track < static_cast<int>(tracks.size())) {
                                tracks[target_track].clips.clear();
                                Debug::Log("[VIEWPORT DROP] Cleared " + std::string(add_to_left ? "LEFT" : "RIGHT") +
                                           " track for replacement");
                            }

                            // Execute via command for undo/redo
                            auto cmd = std::make_unique<ump::OverwriteEditCommand>(
                                timeline_view.get(), target_track, clip);
                            timeline_command_manager->Execute(std::move(cmd));
                            timeline_view->RecalculateDuration();
                            timeline_view->RequestFitZoomOnNextRender();

                            Debug::Log("[VIEWPORT DROP] Replaced " + std::string(add_to_left ? "LEFT" : "RIGHT") +
                                       " track with '" + clip.name + "'");
                        }
                    }

                    g_viewport_dv_drop.pending_media_id.clear();
                    ImGui::CloseCurrentPopup();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Cancel button - flush right
                float cancel_width = 100.0f;
                float avail_width = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_width - cancel_width);
                PushOutlineButtonStyle();
                if (ImGui::Button("Cancel", ImVec2(cancel_width, 0))) {
                    g_viewport_dv_drop.pending_media_id.clear();
                    ImGui::CloseCurrentPopup();
                }
                PopOutlineButtonStyle();

                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();  // WindowPadding

            ImGui::EndChild();  // End ##VideoArea
            ImGui::PopStyleVar();

            // Render timeline as child window at bottom (dynamic height based on playlist mode)
            if (show_timeline_panel) {
                ImGui::BeginChild("##TimelineArea", ImVec2(0, timeline_height), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                // Add left/right padding with inner child window
                const float h_padding = 12.0f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + h_padding);
                ImGui::BeginChild("##TimelineContent", ImVec2(-h_padding, 0), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                RenderTimelineContent();

                ImGui::EndChild();  // ##TimelineContent

                ImGui::EndChild();  // ##TimelineArea
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();  // kTransparentBorder
        ImGui::PopStyleVar(2);   // FramePadding, WindowPadding
    }

    void Application::RenderBackgroundSelectionPanel(VideoBackgroundType& bg_type, bool& show_panel) {
        if (!show_panel) return;

        ImGuiWindow* video_window = ImGui::FindWindowByName("Video Viewport");
        if (!video_window) return;

        ImVec2 video_pos = video_window->Pos;
        ImVec2 video_size = video_window->Size;

        // Scale panel dimensions with font (full width, dampened height)
        const float ui_scale = ImGui::GetIO().FontGlobalScale;
        const float height_scale = 1.0f + (ui_scale - 1.0f) * 0.65f;
        const float panel_width = 200.0f * ui_scale;
        const float panel_height = 116.0f * height_scale;
        const float margin = 10.0f;

        // Position in top-right corner
        ImGui::SetNextWindowPos(ImVec2(
            video_pos.x + video_size.x - panel_width - margin,
            video_pos.y + margin
        ), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height), ImGuiCond_Always);

        ImGuiWindowFlags panel_flags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 2.0f));

        bool panel_hovered = false;

        if (ImGui::Begin("##BackgroundPanel", &show_panel, panel_flags)) {
            panel_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

            // Save cursor position
            ImVec2 text_pos = ImGui::GetCursorPos();

            static const char* options[] = {
                "Black",
                "None",
                "Dark Checkerboard",
                "Light Checkerboard"
            };

            static const VideoBackgroundType types[] = {
                VideoBackgroundType::BLACK,
                VideoBackgroundType::DEFAULT,
                VideoBackgroundType::DARK_CHECKERBOARD,
                VideoBackgroundType::LIGHT_CHECKERBOARD
            };

            for (int i = 0; i < 4; i++) {
                bool is_selected = (bg_type == types[i]);

                if (is_selected) {
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.4f, 0.4f, 0.4f, 0.6f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
                }

                if (ImGui::Selectable(options[i], is_selected)) {
                    bg_type = types[i];
                    show_panel = false;
                    SaveSettings();
                    Debug::Log("Changed video background to: " + std::string(options[i]));
                }

                if (is_selected) {
                    ImGui::PopStyleColor(2);
                }
            }
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // Handle click-outside-to-close
        static int panel_open_frames = 0;
        if (show_panel) {
            panel_open_frames++;
            if (panel_open_frames > 5) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !panel_hovered) {
                    show_panel = false;
                    Debug::Log("Panel closed by clicking outside");
                }
            }
        }
        else {
            panel_open_frames = 0;
        }
    }


    void Application::DrawVideoBackground(ImVec2 canvas_pos, ImVec2 canvas_size, float tile_size) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        switch (video_background_type) {
        case VideoBackgroundType::DEFAULT:
            draw_list->AddRectFilled(
                canvas_pos,
                ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                IM_COL32(27, 27, 27, 255)
            );
            break;

        case VideoBackgroundType::BLACK:
            draw_list->AddRectFilled(
                canvas_pos,
                ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                IM_COL32(0, 0, 0, 255)
            );
            break;

        case VideoBackgroundType::DARK_CHECKERBOARD: {
            const ImU32 col_dark = IM_COL32(30, 30, 30, 255);
            const ImU32 col_darker = IM_COL32(20, 20, 20, 255);

            int cols = (int)(canvas_size.x / tile_size) + 1;
            int rows = (int)(canvas_size.y / tile_size) + 1;

            for (int row = 0; row < rows; row++) {
                for (int col = 0; col < cols; col++) {
                    bool is_even = ((row + col) % 2 == 0);
                    ImU32 color = is_even ? col_dark : col_darker;

                    ImVec2 p0 = ImVec2(canvas_pos.x + col * tile_size,
                        canvas_pos.y + row * tile_size);
                    ImVec2 p1 = ImVec2(p0.x + tile_size, p0.y + tile_size);

                    draw_list->AddRectFilled(p0, p1, color);
                }
            }
            break;
        }

        case VideoBackgroundType::LIGHT_CHECKERBOARD: {
            const ImU32 col_light = IM_COL32(200, 200, 200, 255);
            const ImU32 col_lighter = UI_LIGHT_GRAY;

            int cols = (int)(canvas_size.x / tile_size) + 1;
            int rows = (int)(canvas_size.y / tile_size) + 1;

            for (int row = 0; row < rows; row++) {
                for (int col = 0; col < cols; col++) {
                    bool is_even = ((row + col) % 2 == 0);
                    ImU32 color = is_even ? col_light : col_lighter;

                    ImVec2 p0 = ImVec2(canvas_pos.x + col * tile_size,
                        canvas_pos.y + row * tile_size);
                    ImVec2 p1 = ImVec2(p0.x + tile_size, p0.y + tile_size);

                    draw_list->AddRectFilled(p0, p1, color);
                }
            }
            break;
        }
        }
    }
