// ============================================================================
// Docking layout, panel arrangement, and share project popups
// ============================================================================

#include "app/application.h"
#include "app/app_ui_macros.h"
#include "app/ui_scale.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "player/video_player.h"
#include <imgui.h>
#include <imgui_internal.h>
#ifdef QCVIEW_USE_METAL
#include "app/macos_app_delegate.h"
#else
#include <GLFW/glfw3.h>
#endif

#ifdef QCVIEW_USE_VULKAN
#include "gpu/vulkan_texture_pool.h"
static inline ImTextureID PoolIDToImTexture(GLuint id) {
    if (id == 0) return ImTextureID{};
    return qcview::VulkanTexturePool::Instance().GetImTextureID(static_cast<uint64_t>(id));
}
#elif defined(QCVIEW_USE_METAL)
#include "gpu/metal_texture_pool.h"
static inline ImTextureID PoolIDToImTexture(GLuint id) {
    if (id == 0) return ImTextureID{};
    return qcview::MetalTexturePool::Instance().GetImTextureID(static_cast<uint64_t>(id));
}
#else
static inline ImTextureID PoolIDToImTexture(GLuint id) {
    return (ImTextureID)(intptr_t)id;
}
#endif

// Globals defined in main.cpp
extern bool otio_dual_view_mode;
extern bool otio_dual_view_split_mode;
extern float otio_dual_view_split_pos;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern std::unique_ptr<qcview::TimelinePlaybackController> scratch_timeline_controller;
extern qcview::TimelinePlaybackController::DualViewTextures cached_dual_view_textures;
extern bool show_delete_prefs_confirm;

    void Application::CreateDockingLayout() {
        ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();

#ifdef QCVIEW_USE_METAL
        // macOS native fullscreen: stay in the dockspace (don't use the separate
        // "Fullscreen Player" window). The dockspace already hides panels when
        // is_fullscreen is true (line ~344), so the viewport expands to fill.
        // This avoids swapping between two different ImGui windows during the
        // macOS fullscreen animation, which caused visual pops.
        {
            bool was_fullscreen = is_fullscreen;
            is_fullscreen = MacOS_IsNativeFullscreen();
            if (is_fullscreen != was_fullscreen) {
                if (!is_fullscreen) {
                    // Exiting fullscreen — restore panel visibility now that animation is complete.
                    // Panels were hidden in ToggleFullscreen() and stay hidden during the exit
                    // animation to prevent the timeline background from showing during zoom-out.
                    show_project_panel = saved_show_project_panel;
                    show_inspector_panel = saved_show_inspector_panel;
                    show_color_panels = saved_show_color_panels;
                    show_annotation_panel = saved_show_annotation_panel;
                    show_annotation_toolbar = saved_show_annotation_toolbar;
                    show_sidebar_panel = saved_show_sidebar_panel;
                    show_timeline_panel = saved_show_timeline_panel;
                    show_transport_controls = saved_show_transport_controls;
                    if (annotation_toolbar) annotation_toolbar->SetVisible(saved_show_annotation_toolbar);
                }
                first_time_setup = true;
            }
        }
        // Fall through to the dockspace path below — skip the "Fullscreen Player" window
#else
        // Windows/Linux: use the dedicated "Fullscreen Player" window (instant GLFW toggle)
#endif

#ifndef QCVIEW_USE_METAL
        if (is_fullscreen) {
            // Fullscreen ImGui window - use full monitor/window size (no decorations)
#ifdef _WIN32
            // Windows borderless: use monitor physical size (no content scaling mismatch)
            GLFWmonitor* monitor = fullscreen_monitor ? fullscreen_monitor : glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            int mon_x, mon_y;
            glfwGetMonitorPos(monitor, &mon_x, &mon_y);
            ImGui::SetNextWindowPos(ImVec2((float)mon_x, (float)mon_y));
            ImGui::SetNextWindowSize(ImVec2((float)mode->width, (float)mode->height));
#elif defined(QCVIEW_USE_METAL)
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
#else
            // Linux/Wayland: use framebuffer size divided by content scale to get
            // logical coordinates that match ImGui's coordinate space.
            int fb_w, fb_h;
            glfwGetFramebufferSize(window, &fb_w, &fb_h);
            float x_scale = 1.0f, y_scale = 1.0f;
            glfwGetWindowContentScale(window, &x_scale, &y_scale);
            float logical_w = (float)fb_w / x_scale;
            float logical_h = (float)fb_h / y_scale;
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(logical_w, logical_h));
#endif
            ImGui::SetNextWindowViewport(viewport->ID);

            ImGuiWindowFlags fullscreen_flags = ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

            bool fullscreen_open = true;
            ImGui::Begin("Fullscreen Player", &fullscreen_open, fullscreen_flags);
            ImGui::PopStyleVar(3);

            // Render video content in fullscreen
            ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
            ImVec2 canvas_size = ImGui::GetContentRegionAvail();
            DrawVideoBackground(canvas_pos, canvas_size, 40.0f);

            // Check if in dual view mode
            bool is_dual_view_fullscreen = otio_dual_view_mode && timeline_view && timeline_view->IsDualViewMode() &&
                                           scratch_timeline_controller && scratch_timeline_controller->IsDualViewMode();

            if (is_dual_view_fullscreen) {
                // Render dual view in fullscreen
                ImDrawList* draw_list = ImGui::GetWindowDrawList();

                // Use cached dual view textures
                bool use_unified = cached_dual_view_textures.is_unified;

                auto calculate_fit_size = [](int src_w, int src_h, float max_w, float max_h) -> ImVec2 {
                    if (src_w == 0 || src_h == 0) return ImVec2(max_w, max_h);
                    float aspect = (float)src_w / (float)src_h;
                    if (max_w / max_h > aspect) {
                        return ImVec2(max_h * aspect, max_h);
                    } else {
                        return ImVec2(max_w, max_w / aspect);
                    }
                };

                if (otio_dual_view_split_mode) {
                    // SPLIT MODE in fullscreen: vertical wipe
                    float split_x = canvas_pos.x + canvas_size.x * otio_dual_view_split_pos;

                    int left_w = cached_dual_view_textures.left_width;
                    int left_h = cached_dual_view_textures.left_height;
                    ImVec2 fit_size = calculate_fit_size(left_w, left_h, canvas_size.x, canvas_size.y);
                    ImVec2 display_pos = ImVec2(
                        canvas_pos.x + (canvas_size.x - fit_size.x) * 0.5f,
                        canvas_pos.y + (canvas_size.y - fit_size.y) * 0.5f
                    );

                    // Left side (clipped to left of split)
                    if (cached_dual_view_textures.left_texture != 0) {
                        draw_list->PushClipRect(canvas_pos, ImVec2(split_x, canvas_pos.y + canvas_size.y), true);
                        draw_list->AddImage(
                            PoolIDToImTexture(cached_dual_view_textures.left_texture),
                            display_pos,
                            ImVec2(display_pos.x + fit_size.x, display_pos.y + fit_size.y)
                        );
                        draw_list->PopClipRect();
                    }

                    // Right side (clipped to right of split)
                    if (cached_dual_view_textures.right_texture != 0) {
                        draw_list->PushClipRect(ImVec2(split_x, canvas_pos.y), ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), true);
                        draw_list->AddImage(
                            PoolIDToImTexture(cached_dual_view_textures.right_texture),
                            display_pos,
                            ImVec2(display_pos.x + fit_size.x, display_pos.y + fit_size.y)
                        );
                        draw_list->PopClipRect();
                    }

                    // Draw split divider line
                    draw_list->AddLine(
                        ImVec2(split_x, canvas_pos.y),
                        ImVec2(split_x, canvas_pos.y + canvas_size.y),
                        IM_COL32(255, 255, 255, 180), S(2.0f)
                    );
                } else {
                    // SIDE-BY-SIDE MODE in fullscreen
                    float half_width = canvas_size.x * 0.5f;

                    // Left side
                    if (cached_dual_view_textures.left_texture != 0) {
                        int left_w = cached_dual_view_textures.left_width;
                        int left_h = cached_dual_view_textures.left_height;
                        ImVec2 left_fit = calculate_fit_size(left_w, left_h, half_width - S(2), canvas_size.y);
                        ImVec2 left_pos = ImVec2(
                            canvas_pos.x + (half_width - left_fit.x) * 0.5f,
                            canvas_pos.y + (canvas_size.y - left_fit.y) * 0.5f
                        );
                        draw_list->AddImage(
                            PoolIDToImTexture(cached_dual_view_textures.left_texture),
                            left_pos,
                            ImVec2(left_pos.x + left_fit.x, left_pos.y + left_fit.y)
                        );
                    }

                    // Right side
                    if (cached_dual_view_textures.right_texture != 0) {
                        int right_w = cached_dual_view_textures.right_width;
                        int right_h = cached_dual_view_textures.right_height;
                        ImVec2 right_fit = calculate_fit_size(right_w, right_h, half_width - S(2), canvas_size.y);
                        ImVec2 right_pos = ImVec2(
                            canvas_pos.x + half_width + (half_width - right_fit.x) * 0.5f,
                            canvas_pos.y + (canvas_size.y - right_fit.y) * 0.5f
                        );
                        draw_list->AddImage(
                            PoolIDToImTexture(cached_dual_view_textures.right_texture),
                            right_pos,
                            ImVec2(right_pos.x + right_fit.x, right_pos.y + right_fit.y)
                        );
                    }

                    // Draw center divider
                    float mid_x = canvas_pos.x + half_width;
                    draw_list->AddLine(
                        ImVec2(mid_x, canvas_pos.y),
                        ImVec2(mid_x, canvas_pos.y + canvas_size.y),
                        IM_COL32(80, 80, 80, 255), S(2.0f)
                    );
                }
            } else if (video_player) {
                // Normal single video fullscreen
                video_player->RenderVideoFrame();
            }

            ImGui::End();
            return;
        }
#endif // !QCVIEW_USE_METAL

        // Normal windowed/dockspace mode (also used for Metal fullscreen)
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
        // Hide menu bar during fullscreen and transitions (same condition as borders)
        bool hide_menu = is_fullscreen
            || (!show_timeline_panel && !show_sidebar_panel && !show_project_panel && !show_inspector_panel)
#ifdef QCVIEW_USE_METAL
            || MacOS_IsFullscreenAnimating()
#endif
            ;
        if (!hide_menu) {
            window_flags |= ImGuiWindowFlags_MenuBar;
        }
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        bool p_open = true;
        ImGui::Begin("QCView Dockspace", &p_open, window_flags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
        // Hide all visual borders/separators when panels are hidden (fullscreen or transitioning).
        // During enter animation, is_fullscreen is still false but panels are already hidden.
        // Also hide during the macOS fullscreen exit animation to mask the rebuild frame.
        bool hide_borders = is_fullscreen
            || (!show_timeline_panel && !show_sidebar_panel && !show_project_panel && !show_inspector_panel)
#ifdef QCVIEW_USE_METAL
            || MacOS_IsFullscreenAnimating()
#endif
            ;
        if (hide_borders) {
            ImGui::PushStyleVar(ImGuiStyleVar_DockingSeparatorSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
            // Match all backgrounds to video background so nothing peeks through
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(27, 27, 27, 255));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(27, 27, 27, 255));
            ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, IM_COL32(27, 27, 27, 255));
        }
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
            ImGuiDockNodeFlags_NoTabBar |
            ImGuiDockNodeFlags_NoUndocking |
            ImGuiDockNodeFlags_NoDockingSplit);
        if (hide_borders) {
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(3);
        }

        if (first_time_setup) {

            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

            // Sidebar removed — panel toggles are now in the menu bar toolbar

            if (show_color_panels) {
                // COLOR VIEW: Color panel at very bottom, video/timeline above it
                auto bottom_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.4f, nullptr, &dockspace_id);

                // Top area can have side panels if needed
                if (show_project_panel || show_inspector_panel) {
                    auto dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f, nullptr, &dockspace_id);
                    auto dock_id_project = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.46f, nullptr, &dock_id_left);
                    auto dock_id_inspector = dock_id_left;

                    // Timeline now inside Video Viewport as child window
                    // Annotation toolbar now floats over viewport (not docked)
                    ImGuiID dock_id_video = dockspace_id;

                    // Split video area for annotations/playlist on the right
                    auto dock_id_right = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);
                    // Split right panel: annotations on top, playlist below
                    auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Up, 0.6f, nullptr, &dock_id_right);
                    auto dock_id_playlist = dock_id_right;

                    // Dock windows

                    ImGui::DockBuilderDockWindow("Project", dock_id_project);
                    ImGui::DockBuilderDockWindow("Inspector", dock_id_inspector);
                    ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
                    ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
                    ImGui::DockBuilderDockWindow("Playlist", dock_id_playlist);
                    ImGui::DockBuilderDockWindow("Color", bottom_dock);
                }
                else {
                    // Timeline now inside Video Viewport as child window
                    // Annotation toolbar now floats over viewport (not docked)
                    ImGuiID dock_id_video = dockspace_id;

                    // Split video area for annotations/playlist on the right
                    auto dock_id_right = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);
                    // Split right panel: annotations on top, playlist below
                    auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Up, 0.6f, nullptr, &dock_id_right);
                    auto dock_id_playlist = dock_id_right;

                    // Dock windows

                    ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
                    ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
                    ImGui::DockBuilderDockWindow("Playlist", dock_id_playlist);
                    ImGui::DockBuilderDockWindow("Color", bottom_dock);
                }
            }
            else {
                // DEFAULT VIEW: Side panels on left, timeline under viewport (if shown), no color panel
                auto dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f, nullptr, &dockspace_id);
                auto dock_id_project = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.46f, nullptr, &dock_id_left);
                auto dock_id_inspector = dock_id_left;

                // Timeline now inside Video Viewport as child window
                // Annotation toolbar now floats over viewport (not docked)
                ImGuiID dock_id_video = dockspace_id;

                // Split video area for annotations/playlist on the right
                auto dock_id_right = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);
                // Split right panel: annotations on top, playlist below
                auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Up, 0.6f, nullptr, &dock_id_right);
                auto dock_id_playlist = dock_id_right;

                // Dock windows
                ImGui::DockBuilderDockWindow("Project", dock_id_project);
                ImGui::DockBuilderDockWindow("Inspector", dock_id_inspector);
                ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
                ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
                ImGui::DockBuilderDockWindow("Playlist", dock_id_playlist);
            }

            ImGui::DockBuilderFinish(dockspace_id);
            first_time_setup = false;
        }

        // Hide menu bar during export and fullscreen transitions
        if (!export_state.active && !hide_menu) {
            CreateMenuBar();
        }
        ImGui::End();

        // Render panels based on visibility — with per-panel timing during playback
        auto _pp0 = std::chrono::steady_clock::now();
        CreateVideoViewport();  // Timeline now renders inside viewport
        auto _pp1 = std::chrono::steady_clock::now();
        if (!is_fullscreen) {
            if (show_project_panel) CreateProjectPanel();
            if (show_inspector_panel) CreateInspectorPanel();
            if (show_annotation_panel) CreateAnnotationPanel();
            CreateAnnotationToolbar();
            if (show_color_panels) CreateColorPanels();
            CreateCacheStatsWindow();
            CreateAudioDiagnosticsWindow();
            CreateCacheSettingsWindow();
            CreateFontSettingsWindow();
            CreateKeyboardShortcutsPopup();
            CreateLutExportPopup();
        }
        auto _pp2 = std::chrono::steady_clock::now();
        {
            (void)_pp0; (void)_pp1; (void)_pp2; // PANELS logging disabled
        }
        RenderBackgroundSelectionPanel(video_background_type, show_background_panel);
        RenderSafetyOverlayPanel(show_safety_overlay_panel);
        RenderColorspacePresetsPanel(show_colorspace_panel);
        RenderTrimToolbarPanel();  // Trim toolbar overlay for dual view modes
        if (!is_fullscreen) {
            // Sidebar panel replaced by toolbar buttons in menu bar
            // RenderSidebarPanel();
        }

        // Handle project manager dialogs (including image sequence frame rate dialog)
        if (project_manager) {
            project_manager->HandleProjectDialogs();
            project_manager->RenderTranscodeQueueWindow();

            // Handle pending dialog requests from context menus
            if (project_manager->IsPendingOpenMediaDialog()) {
                project_manager->ClearPendingOpenMediaDialog();
                OpenFileDialog();
            }
            if (project_manager->IsPendingOpenProjectDialog()) {
                project_manager->ClearPendingOpenProjectDialog();
                project_manager->LoadProject();  // Empty path triggers file dialog
            }
        }

        // Render top-level dialogs (outside any parent modal context for proper centering)
        CreateTranscodeProgressDialog(); // EXR transcode progress dialog
        CreateCacheClearDialogs(); // Cache clear dialogs (error + success)
        RenderPressureCriticalDialog(); // System critical emergency dialog with auto-recovery

        // Render Frame.io import dialog
        RenderFrameioImportDialog();

        // Share Project popups
        HandleShareProjectPopups();

        // Render loading overlay if media is loading
        if (IsLoadingMedia()) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImVec2 display_size = viewport->Size;
            ImDrawList* draw_list = ImGui::GetForegroundDrawList(viewport);

            // Fullscreen dim
            draw_list->AddRectFilled(
                viewport->Pos,
                ImVec2(viewport->Pos.x + display_size.x, viewport->Pos.y + display_size.y),
                IM_COL32(0, 0, 0, 160)
            );

            // Modal box
            const float modal_width = S(320.0f);
            const float modal_height = S(80.0f);
            ImVec2 modal_pos = ImVec2(center.x - modal_width * 0.5f, center.y - modal_height * 0.5f);
            ImVec2 modal_end = ImVec2(modal_pos.x + modal_width, modal_pos.y + modal_height);

            draw_list->AddRectFilled(modal_pos, modal_end, IM_COL32(33, 33, 33, 240), S(4.0f));
            draw_list->AddRect(modal_pos, modal_end, IM_COL32(77, 77, 89, 255), S(4.0f), 0, S(1.0f));

            // Message
            const char* msg = loading_message_.empty() ? "Loading..." : loading_message_.c_str();
            ImVec2 msg_size = ImGui::CalcTextSize(msg);
            float msg_x = modal_pos.x + (modal_width - msg_size.x) * 0.5f;
            float msg_y = modal_pos.y + (modal_height - msg_size.y) * 0.5f;
            draw_list->AddText(ImVec2(msg_x, msg_y), UI_WHITE, msg);
        }

        // Render Go To Timecode/Frame modal
        RenderGotoTimecodeModal();
    }

    void Application::HandleShareProjectPopups() {
        // Success popup - URI copied to clipboard
        if (ImGui::BeginPopupModal("URI Copied##ShareProject", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Project URI copied to clipboard!");
            ImGui::Separator();
            ImGui::TextWrapped("Share this link with others to open the project.");
            ImGui::TextWrapped("Format: qcview:///path/to/project.qcvproj");
            ImGui::Separator();
            ImGui::Spacing();

            float btnPadding = S(8.0f) * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW);
            PushOutlineButtonStyle();
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }

        // Error popup - No project saved
        if (ImGui::BeginPopupModal("No Project Saved##ShareProject", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("No project file has been saved yet.");
            ImGui::Separator();
            ImGui::TextWrapped("Please save your project first before sharing.");
            ImGui::Separator();
            ImGui::Spacing();

            float btnPadding = S(8.0f) * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW);
            PushOutlineButtonStyle();
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }

        // Delete preferences confirmation popup
        if (show_delete_prefs_confirm) {
            ImGui::OpenPopup("Confirm Delete Preferences");
            show_delete_prefs_confirm = false;
        }
        if (ImGui::BeginPopupModal("Confirm Delete Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to delete all preferences?");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.3f, 1.0f), "This will reset all settings to defaults on next launch.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            PushOutlineButtonStyle();
            if (ImGui::Button("Cancel", ImVec2(S(120), 0))) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            if (ImGui::Button("Delete All", ImVec2(S(120), 0))) {
                ImGui::CloseCurrentPopup();
                DeleteAllPreferences();
            }
            ImGui::PopStyleColor(2);
            ImGui::EndPopup();
        }

        // Preferences deleted popup
        if (ImGui::BeginPopupModal("Preferences Deleted", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("All preferences have been deleted.");
            ImGui::Separator();
            ImGui::TextWrapped("Default settings will be used on next launch.");
            ImGui::Separator();
            ImGui::Spacing();

            float btnPadding = S(8.0f) * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x - okW) * 0.5f);
            PushOutlineButtonStyle();
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }
    }

    void Application::SetupDefaultLayout(ImGuiID dockspace_id) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        // Side panels on left
        auto dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f, nullptr, &dockspace_id);
        auto dock_id_project = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.5f, nullptr, &dock_id_left);
        auto dock_id_inspector = dock_id_left;

        // Timeline now inside Video Viewport as child window
        ImGuiID dock_id_video = dockspace_id;

        // Split video area for annotations/playlist on the right
        auto dock_id_right = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);
        // Split right panel: annotations on top, playlist below
        auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Up, 0.6f, nullptr, &dock_id_right);
        auto dock_id_playlist = dock_id_right;

        // Dock windows
        ImGui::DockBuilderDockWindow("Project", dock_id_project);
        ImGui::DockBuilderDockWindow("Inspector", dock_id_inspector);
        ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
        ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
        ImGui::DockBuilderDockWindow("Playlist", dock_id_playlist);

        ImGui::DockBuilderFinish(dockspace_id);
    }
