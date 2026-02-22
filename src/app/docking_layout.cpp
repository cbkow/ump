// ============================================================================
// Docking layout, panel arrangement, and share project popups
// ============================================================================

#include "app/application.h"
#include "app/app_ui_macros.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "player/video_player.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>

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

        if (is_fullscreen) {
            // Fullscreen ImGui window - use full monitor size (no decorations)
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);

            // True borderless fullscreen - use entire screen
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2((float)mode->width, (float)mode->height));
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
                            (void*)(intptr_t)cached_dual_view_textures.left_texture,
                            display_pos,
                            ImVec2(display_pos.x + fit_size.x, display_pos.y + fit_size.y)
                        );
                        draw_list->PopClipRect();
                    }

                    // Right side (clipped to right of split)
                    if (cached_dual_view_textures.right_texture != 0) {
                        draw_list->PushClipRect(ImVec2(split_x, canvas_pos.y), ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), true);
                        draw_list->AddImage(
                            (void*)(intptr_t)cached_dual_view_textures.right_texture,
                            display_pos,
                            ImVec2(display_pos.x + fit_size.x, display_pos.y + fit_size.y)
                        );
                        draw_list->PopClipRect();
                    }

                    // Draw split divider line
                    draw_list->AddLine(
                        ImVec2(split_x, canvas_pos.y),
                        ImVec2(split_x, canvas_pos.y + canvas_size.y),
                        IM_COL32(255, 255, 255, 180), 2.0f
                    );
                } else {
                    // SIDE-BY-SIDE MODE in fullscreen
                    float half_width = canvas_size.x * 0.5f;

                    // Left side
                    if (cached_dual_view_textures.left_texture != 0) {
                        int left_w = cached_dual_view_textures.left_width;
                        int left_h = cached_dual_view_textures.left_height;
                        ImVec2 left_fit = calculate_fit_size(left_w, left_h, half_width - 2, canvas_size.y);
                        ImVec2 left_pos = ImVec2(
                            canvas_pos.x + (half_width - left_fit.x) * 0.5f,
                            canvas_pos.y + (canvas_size.y - left_fit.y) * 0.5f
                        );
                        draw_list->AddImage(
                            (void*)(intptr_t)cached_dual_view_textures.left_texture,
                            left_pos,
                            ImVec2(left_pos.x + left_fit.x, left_pos.y + left_fit.y)
                        );
                    }

                    // Right side
                    if (cached_dual_view_textures.right_texture != 0) {
                        int right_w = cached_dual_view_textures.right_width;
                        int right_h = cached_dual_view_textures.right_height;
                        ImVec2 right_fit = calculate_fit_size(right_w, right_h, half_width - 2, canvas_size.y);
                        ImVec2 right_pos = ImVec2(
                            canvas_pos.x + half_width + (half_width - right_fit.x) * 0.5f,
                            canvas_pos.y + (canvas_size.y - right_fit.y) * 0.5f
                        );
                        draw_list->AddImage(
                            (void*)(intptr_t)cached_dual_view_textures.right_texture,
                            right_pos,
                            ImVec2(right_pos.x + right_fit.x, right_pos.y + right_fit.y)
                        );
                    }

                    // Draw center divider
                    float mid_x = canvas_pos.x + half_width;
                    draw_list->AddLine(
                        ImVec2(mid_x, canvas_pos.y),
                        ImVec2(mid_x, canvas_pos.y + canvas_size.y),
                        IM_COL32(80, 80, 80, 255), 2.0f
                    );
                }
            } else if (video_player) {
                // Normal single video fullscreen
                video_player->RenderVideoFrame();
            }

            ImGui::End();
            return;
        }

        // Normal windowed mode
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        bool p_open = true;
        ImGui::Begin("QCView Dockspace", &p_open, window_flags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
            ImGuiDockNodeFlags_NoTabBar |
            ImGuiDockNodeFlags_NoUndocking |
            ImGuiDockNodeFlags_NoDockingSplit);

        if (first_time_setup) {

            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

            // Calculate sidebar width based on UI scale
            const float ui_scale = ImGui::GetIO().FontGlobalScale;
            const float sidebar_width = (28.0f * ui_scale) + 12.0f + 4.0f;  // button + padding + border
            const float sidebar_ratio = sidebar_width / ImGui::GetMainViewport()->Size.x;

            // Split sidebar on far left (if shown)
            ImGuiID dock_id_sidebar = 0;
            if (show_sidebar_panel) {
                dock_id_sidebar = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, sidebar_ratio, nullptr, &dockspace_id);
            }

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
                    if (show_sidebar_panel) ImGui::DockBuilderDockWindow("Sidebar", dock_id_sidebar);
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
                    if (show_sidebar_panel) ImGui::DockBuilderDockWindow("Sidebar", dock_id_sidebar);
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
                if (show_sidebar_panel) ImGui::DockBuilderDockWindow("Sidebar", dock_id_sidebar);
                ImGui::DockBuilderDockWindow("Project", dock_id_project);
                ImGui::DockBuilderDockWindow("Inspector", dock_id_inspector);
                ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
                ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
                ImGui::DockBuilderDockWindow("Playlist", dock_id_playlist);
            }

            ImGui::DockBuilderFinish(dockspace_id);
            first_time_setup = false;
        }

        // Hide menu bar during export for clean screenshots
        if (!export_state.active) {
            CreateMenuBar();
        }
        ImGui::End();

        // Render panels based on visibility
        CreateVideoViewport();  // Timeline now renders inside viewport
        if (!is_fullscreen) {
            if (show_project_panel) CreateProjectPanel();
            if (show_inspector_panel) CreateInspectorPanel();
            if (show_annotation_panel) CreateAnnotationPanel();
            CreateAnnotationToolbar(); // Always try to render toolbar (it handles visibility internally)
            if (show_color_panels) CreateColorPanels();
            CreateCacheStatsWindow(); // Add cache monitoring window
            CreateAudioDiagnosticsWindow(); // Add audio monitoring window (Ctrl+Shift+A)
            CreateCacheSettingsWindow(); // Add cache settings popup
            CreateFontSettingsWindow(); // Add font settings popup
            CreateKeyboardShortcutsPopup(); // Add keyboard shortcuts popup
            CreateLutExportPopup(); // Add LUT export progress popup
        }
        RenderBackgroundSelectionPanel(video_background_type, show_background_panel);
        RenderSafetyOverlayPanel(show_safety_overlay_panel);
        RenderColorspacePresetsPanel(show_colorspace_panel);
        RenderTrimToolbarPanel();  // Trim toolbar overlay for dual view modes
        RenderSidebarPanel();  // Left sidebar with panel toggles and HDR indicator

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
            const float modal_width = 320.0f;
            const float modal_height = 80.0f;
            ImVec2 modal_pos = ImVec2(center.x - modal_width * 0.5f, center.y - modal_height * 0.5f);
            ImVec2 modal_end = ImVec2(modal_pos.x + modal_width, modal_pos.y + modal_height);

            draw_list->AddRectFilled(modal_pos, modal_end, IM_COL32(33, 33, 33, 240), 4.0f);
            draw_list->AddRect(modal_pos, modal_end, IM_COL32(77, 77, 89, 255), 4.0f, 0, 1.0f);

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

            float btnPadding = 8.0f * 2;
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

            float btnPadding = 8.0f * 2;
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
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            if (ImGui::Button("Delete All", ImVec2(120, 0))) {
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

            float btnPadding = 8.0f * 2;
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
