// ============================================================================
// Sidebar panels (sidebar, trim toolbar, Frame.io import, safety overlay, colorspace)
// ============================================================================

#include "app/application.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/ui_scale.h"
#include "app/app_config.h"
#include "project/project_manager.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "player/video_player.h"
#include "player/pipeline_mode.h"
#include "ui/timeline_manager.h"
#include "nodes/node_manager.h"
#include "audio/audio_mixer.h"
#ifdef _WIN32
#include "hdr/hdr_output_manager.h"
#endif
#include "annotations/annotation_manager.h"
#include "integrations/frameio_url_parser.h"
#include "integrations/frameio_client.h"
#include "integrations/frameio_converter.h"
#include "utils/asset_path.h"
#include <imgui.h>
#include <imgui_internal.h>

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern bool otio_dual_view_mode;
extern CacheSettings cache_settings;

    void Application::RenderSidebarPanel() {
        if (!show_sidebar_panel) return;

        // Scale dimensions with font
        const float ui_scale = ImGui::GetIO().FontGlobalScale;
        const float button_size = S(28.0f);
        const float button_spacing = S(4.0f);

        ImGuiWindowFlags panel_flags =
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoTitleBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6.0f), S(8.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));  // Remove button padding for centered icons
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);       // No borders on buttons

        if (ImGui::Begin("Sidebar", &show_sidebar_panel, panel_flags)) {
            ImGui::PushFont(font_icons);
            ImGui::SetWindowFontScale(1.2f);

            // Use white for active, gray for inactive
            ImVec4 active_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White
            ImVec4 inactive_color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);  // Gray

            // Calculate centering offset
            float window_width = ImGui::GetWindowWidth();
            float padding = ImGui::GetStyle().WindowPadding.x;
            float available_width = window_width - (padding * 2);
            float center_offset = (available_width - button_size) * 0.5f;

            // Helper lambda for toggle buttons (centered)
            auto RenderPanelToggle = [&](const char* icon, const char* id, bool& panel_state, const char* tooltip) {
                ImVec4 btn_color = panel_state ? active_color : inactive_color;
                ImGui::PushStyleColor(ImGuiCol_Text, btn_color);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 0.5f));

                // Center the button horizontally
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_offset);

                if (ImGui::Button(icon, ImVec2(button_size, button_size))) {
                    panel_state = !panel_state;
                    if (panel_state) minimal_view_mode = false;  // Exit minimal when opening panel
                    // Special handling for color panels - needs layout rebuild
                    if (&panel_state == &show_color_panels || &panel_state == &show_timeline_panel) {
                        first_time_setup = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::PopFont();
                    ImGui::SetTooltip("%s", tooltip);
                    ImGui::PushFont(font_icons);
                    ImGui::SetWindowFontScale(1.2f);
                }

                ImGui::PopStyleColor(4);
            };

            // Helper lambda for action buttons (non-toggle, centered)
            auto RenderActionButton = [&](const char* icon, const char* id, const char* tooltip, auto action) {
                ImGui::PushStyleColor(ImGuiCol_Text, active_color);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 0.5f));

                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_offset);

                if (ImGui::Button(icon, ImVec2(button_size, button_size))) {
                    action();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::PopFont();
                    ImGui::SetTooltip("%s", tooltip);
                    ImGui::PushFont(font_icons);
                    ImGui::SetWindowFontScale(1.2f);
                }

                ImGui::PopStyleColor(4);
            };

            // === TOP SECTION: Panel toggles ===
            RenderPanelToggle(ICON_TOPIC, "##sb_project", show_project_panel,
                show_project_panel ? "Hide Project Panel (Ctrl+1)" : "Show Project Panel (Ctrl+1)");

            ImGui::Dummy(ImVec2(0, button_spacing));

            RenderPanelToggle(ICON_INFO, "##sb_inspector", show_inspector_panel,
                show_inspector_panel ? "Hide Inspector (Ctrl+2)" : "Show Inspector (Ctrl+2)");

            ImGui::Dummy(ImVec2(0, button_spacing));

            RenderPanelToggle(ICON_VIEW_TIMELINE, "##sb_timeline", show_timeline_panel,
                show_timeline_panel ? "Hide Timeline (Ctrl+3)" : "Show Timeline (Ctrl+3)");

            ImGui::Dummy(ImVec2(0, button_spacing));

            RenderPanelToggle(ICON_PALETTE, "##sb_color", show_color_panels,
                show_color_panels ? "Hide Color Panels (Ctrl+4)" : "Show Color Panels (Ctrl+4)");

            ImGui::Dummy(ImVec2(0, button_spacing));

            RenderPanelToggle(ICON_DRAW, "##sb_annotations", show_annotation_panel,
                show_annotation_panel ? "Hide Annotations (Ctrl+5)" : "Show Annotations (Ctrl+5)");

            // Divider between panel toggles and file actions
            ImGui::Dummy(ImVec2(0, button_spacing * 2));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, button_spacing * 2));

            // === MIDDLE SECTION: File actions ===
            RenderActionButton(ICON_FOLDER_OPEN, "##sb_open_media", "Open Media (Ctrl+O)", [this]() {
                OpenFileDialog();
            });

            ImGui::Dummy(ImVec2(0, button_spacing));

            RenderActionButton(ICON_FOLDER_SPECIAL, "##sb_open_project", "Open Project (Ctrl+Shift+O)", [this]() {
                if (project_manager) {
                    project_manager->LoadProject();
                    show_project_panel = true;
                    show_inspector_panel = true;
                }
            });

            ImGui::Dummy(ImVec2(0, button_spacing));

            RenderActionButton(ICON_SAVE, "##sb_save_project", "Save Project (Ctrl+S)", [this]() {
                if (project_manager) {
                    project_manager->SaveProject();
                }
            });

            // === BOTTOM SECTION: HDR indicator/toggle ===
            // Calculate space to push HDR to bottom
            float current_y = ImGui::GetCursorPosY();
            float window_height = ImGui::GetWindowHeight();
            float bottom_margin = 8.0f;
            float hdr_y = window_height - button_size - bottom_margin;

            if (hdr_y > current_y + 8.0f) {
                ImGui::SetCursorPosY(hdr_y);

                // HDR status
#ifdef _WIN32
                bool hdr_active = qcview::HDROutputManager::Instance().IsHDRActive();
                bool hdr_supported = hdr_active;  // Windows: follows system state
                bool hdr_toggleable = false;
#elif defined(QCVIEW_USE_VULKAN)
                bool hdr_active = vulkan_swapchain_ ? vulkan_swapchain_->IsHDRActive() : false;
                bool hdr_supported = vulkan_swapchain_ ? vulkan_swapchain_->IsHDRSupported() : false;
                bool hdr_toggleable = hdr_supported;
#elif defined(QCVIEW_USE_METAL)
                bool hdr_active = metal_swapchain_ ? metal_swapchain_->IsHDRActive() : false;
                bool hdr_supported = metal_swapchain_ ? metal_swapchain_->IsHDRSupported() : false;
                bool hdr_toggleable = hdr_supported;
#else
                bool hdr_active = false;
                bool hdr_supported = false;
                bool hdr_toggleable = false;
#endif
                const char* hdr_icon = hdr_active ? ICON_HDR_ON : ICON_HDR_OFF;
                // Use accent color for active, gray for inactive
                ImVec4 hdr_color = hdr_active
                    ? GetWindowsAccentColor()
                    : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, hdr_color);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hdr_toggleable ? ImVec4(0.3f, 0.3f, 0.3f, 0.5f) : ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, hdr_toggleable ? ImVec4(0.2f, 0.2f, 0.2f, 0.5f) : ImVec4(0, 0, 0, 0));

                // Center the HDR button
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_offset);

                if (!hdr_toggleable) ImGui::BeginDisabled();
                if (ImGui::Button(hdr_icon, ImVec2(button_size, button_size))) {
#ifdef QCVIEW_USE_VULKAN
                    if (vulkan_swapchain_ && hdr_supported) {
                        vulkan_swapchain_->SetHDREnabled(!hdr_active);
                        SaveSettings();
                    }
#elif defined(QCVIEW_USE_METAL)
                    if (metal_swapchain_ && hdr_supported) {
                        metal_swapchain_->SetHDREnabled(!hdr_active);
                        SaveSettings();
                    }
#endif
                }
                if (!hdr_toggleable) ImGui::EndDisabled();
                bool hover_hdr = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                ImGui::PopStyleColor(4);

#ifdef QCVIEW_USE_METAL
                // Right-click context menu for EDR colorspace selection
                if (metal_swapchain_ && hdr_active) {
                    if (ImGui::BeginPopupContextItem("##EDRColorspaceMenu")) {
                        ImGui::SetWindowFontScale(1.0f);
                        if (font_icons) ImGui::PopFont();
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "EDR Colorspace");
                        ImGui::Separator();
                        auto current_cs = metal_swapchain_->GetEDRColorspace();
                        for (int i = 0; i < static_cast<int>(qcview::EDRColorspace::Count); i++) {
                            auto cs = static_cast<qcview::EDRColorspace>(i);
                            bool selected = (cs == current_cs);
                            if (ImGui::MenuItem(qcview::EDRColorspaceName(cs), nullptr, selected)) {
                                metal_swapchain_->SetEDRColorspace(cs);
                                SaveSettings();
                            }
                        }
                        ImGui::EndPopup();
                        if (font_icons) ImGui::PushFont(font_icons);
                        ImGui::SetWindowFontScale(1.2f);
                    }
                }
#endif

                if (hover_hdr) {
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::PopFont();
#ifdef _WIN32
                    ImGui::SetTooltip(hdr_active ?
                        "Windows HDR enabled. Images need color transformation." :
                        "Windows HDR disabled. Enable in Windows settings and restart QCView for HDR.");
#elif defined(QCVIEW_USE_VULKAN)
                    if (hdr_supported) {
                        ImGui::SetTooltip(hdr_active ?
                            "HDR output active (PQ/ST.2084). Click to disable.\nEnsure HDR is also enabled in your system display settings." :
                            "Click to enable HDR output.\nEnsure HDR is also enabled in your system display settings.");
                    } else {
                        ImGui::SetTooltip("HDR not available on this display.");
                    }
#elif defined(QCVIEW_USE_METAL)
                    if (hdr_supported) {
                        ImGui::SetTooltip(hdr_active ?
                            "EDR output active (Extended Dynamic Range). Click to disable." :
                            "Click to enable EDR (Extended Dynamic Range) output.");
                    } else {
                        ImGui::SetTooltip("EDR not available on this display.");
                    }
#else
                    ImGui::SetTooltip("HDR not available on this platform.");
#endif
                    ImGui::PushFont(font_icons);
                    ImGui::SetWindowFontScale(1.2f);
                }
            }

            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
        }
        ImGui::End();

        ImGui::PopStyleVar(3);  // WindowPadding + FramePadding + FrameBorderSize
    }

    void Application::RenderTrimToolbarPanel() {
        // No-op: trim toolbar is no longer used
    }


    void Application::RenderFrameioImportDialog() {
        if (!frameio_import_state.show_dialog) return;

        // Token is automatically loaded from settings on app start via LoadSettings()
        // No need to manually load it here

        ImGui::OpenPopup("Import from Frame.io");

        float scale = ImGui::GetIO().FontGlobalScale;
        ImGui::SetNextWindowSize(ImVec2(S(450) + S(50), 0), ImGuiCond_Always);

        if (ImGui::BeginPopupModal("Import from Frame.io", &frameio_import_state.show_dialog, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Import annotations from Frame.io review links");
            ImGui::Separator();
            ImGui::Spacing();

            // API Token input (password style)
            ImGui::Text("API Token:");
            ImGui::PushItemWidth(-1);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputText("##token", frameio_import_state.token_buffer, 256, ImGuiInputTextFlags_Password);
            ImGui::PopItemWidth();

            // Token management buttons (side by side)
            float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            // Save Token button
            bool token_not_empty = strlen(frameio_import_state.token_buffer) > 0;
            if (!token_not_empty) {
                ImGui::BeginDisabled();
            }
            PushOutlineButtonStyle();
            if (ImGui::Button("Save Token", ImVec2(button_width, 0))) {
                SaveSettings();
                frameio_import_state.status_message = "Token saved successfully";
                frameio_import_state.import_success = true;
            }
            PopOutlineButtonStyle();
            if (!token_not_empty) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();

            // Clear Saved Token button
            bool has_saved_token = HasSavedFrameioToken();
            if (!has_saved_token) {
                ImGui::BeginDisabled();
            }
            PushOutlineButtonStyle();
            if (ImGui::Button("Clear Saved Token", ImVec2(button_width, 0))) {
                ClearSavedToken();
                frameio_import_state.token_buffer[0] = '\0';
                frameio_import_state.status_message = "Saved token cleared";
                frameio_import_state.import_success = true;
            }
            PopOutlineButtonStyle();
            if (!has_saved_token) {
                ImGui::EndDisabled();
            }
            ImGui::Spacing();

            // URL input
            ImGui::Text("Frame.io URL or Asset ID:");
            ImGui::PushItemWidth(-1);
            ImGui::InputText("##url", frameio_import_state.url_buffer, 512);
            ImGui::PopItemWidth();
            ImGui::Spacing();

            // Status message
            if (!frameio_import_state.status_message.empty()) {
                if (frameio_import_state.import_success) {
                    // Use bright system accent color for success messages
                    ImVec4 bright_accent = Bright(GetWindowsAccentColor());
                    ImGui::TextColored(bright_accent, "%s", frameio_import_state.status_message.c_str());
                } else {
                    // Keep red for error messages
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", frameio_import_state.status_message.c_str());
                }
                ImGui::Spacing();
            }

            ImGui::Separator();
            ImGui::Spacing();

            // Buttons (flush right)
            float btnPadding = 8.0f * 2;
            float importW = ImGui::CalcTextSize("Import").x + btnPadding;
            float closeW = ImGui::CalcTextSize("Close").x + btnPadding;
            float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - importW - closeW - btnSpacing);

            if (frameio_import_state.importing) {
                ImGui::BeginDisabled();
            }

            PushOutlineButtonStyle();
            if (ImGui::Button("Import")) {
                // Start import
                frameio_import_state.importing = true;
                frameio_import_state.status_message = "Importing...";

                std::string token = frameio_import_state.token_buffer;
                std::string url = frameio_import_state.url_buffer;

                // Parse URL to get asset_id
                auto parse_result = qcview::Integrations::FrameioUrlParser::Parse(url);

                if (!parse_result.success) {
                    frameio_import_state.status_message = "Error: " + parse_result.error_message;
                    frameio_import_state.importing = false;
                    frameio_import_state.import_success = false;
                } else {
                    // Fetch comments from Frame.io
                    auto fetch_result = qcview::Integrations::FrameioClient::GetAssetComments(
                        parse_result.asset_id,
                        token
                    );

                    if (!fetch_result.success) {
                        frameio_import_state.status_message = "Error: " + fetch_result.error_message;
                        frameio_import_state.importing = false;
                        frameio_import_state.import_success = false;
                    } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                        // Convert Frame.io comments to QCView annotations
                        double framerate = video_player ? video_player->GetFrameRate() : 24.0;
                        auto convert_result = qcview::Integrations::FrameioConverter::ConvertComments(
                            fetch_result.comments,
                            framerate
                        );

                        if (!convert_result.success) {
                            frameio_import_state.status_message = "Error: " + convert_result.error_message;
                            frameio_import_state.importing = false;
                            frameio_import_state.import_success = false;
                        } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                            // Add notes to annotation manager
                            if (annotation_manager) {
                                for (const auto& note : convert_result.notes) {
                                    annotation_manager->AddNote(
                                        note.timestamp_seconds,
                                        note.timecode,
                                        note.frame,
                                        note.text
                                    );
                                    // Update annotation data if present
                                    if (!note.annotation_data.empty()) {
                                        annotation_manager->UpdateNoteAnnotationData(
                                            note.timecode,
                                            note.annotation_data
                                        );
                                    }
                                }
                            }

                            frameio_import_state.status_message = "Successfully imported " +
                                std::to_string(convert_result.notes.size()) + " note(s) (" +
                                std::to_string(convert_result.converted_count) + " with drawings, " +
                                std::to_string(convert_result.skipped_count) + " text-only)";
                            frameio_import_state.importing = false;
                            frameio_import_state.import_success = true;

                            // Note: Token is auto-saved when user clicks "Save Token" button
                            // or automatically on app close via SaveSettings()

                            // Start thumbnail generation
                            frameio_import_state.generating_thumbnails = true;
                            frameio_import_state.imported_notes = convert_result.notes;
                            frameio_import_state.current_thumbnail_index = 0;
                            frameio_import_state.waiting_for_seek = false;
                            frameio_import_state.frames_to_wait_after_seek = 0;
                            frameio_import_state.status_message += "\nGenerating thumbnails...";

                            // Enable batch mode to skip auto-saves during thumbnail generation
                            if (annotation_manager) {
                                annotation_manager->SetBatchMode(true);
                                Debug::Log("Enabled batch mode for thumbnail generation");
                            }
                        }
                    }
                }
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Close")) {
                frameio_import_state.show_dialog = false;
            }
            PopOutlineButtonStyle();

            if (frameio_import_state.importing) {
                ImGui::EndDisabled();
            }

            ImGui::EndPopup();
        }
    }



    void Application::RenderSafetyOverlayPanel(bool& show_panel) {
        if (!show_panel) return;

        ImGuiWindow* video_window = ImGui::FindWindowByName("Video Viewport");
        if (!video_window) return;

        ImVec2 video_pos = video_window->Pos;
        ImVec2 video_size = video_window->Size;

        // Scale panel dimensions with font (full width, dampened height)
        const float ui_scale = ImGui::GetIO().FontGlobalScale;
        const float height_scale = 1.0f + (ui_scale - 1.0f) * 0.75f;
        const float panel_width = S(315.0f);
        const float panel_height = S(655.0f);
        const float margin = S(10.0f);

        // Position in top-right corner (same as background panel)
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

        if (ImGui::Begin("##SafetyOverlayPanel", &show_panel, panel_flags)) {
            panel_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

            ImGui::Text("Safety Overlays");
            ImGui::Separator();

            // SVG Selection Dropdown (always show when safety panel is open)
            if (video_player) {
                auto svg_renderer = video_player->GetSVGRenderer();
                static bool debug_logged = false;
                if (!debug_logged) {
                    Debug::Log("Checking SVG renderer: " + std::string(svg_renderer ? "exists" : "null"));
                    debug_logged = true;
                }
                if (svg_renderer) {
                    auto available_svgs = svg_renderer->GetAvailableSVGs();
                    auto display_names = svg_renderer->GetSVGDisplayNames();

                    static bool svg_list_logged = false;
                    if (!svg_list_logged) {
                        Debug::Log("Found " + std::to_string(available_svgs.size()) + " SVG files");
                        for (size_t i = 0; i < available_svgs.size(); ++i) {
                            Debug::Log("  SVG " + std::to_string(i) + ": " + available_svgs[i]);
                        }
                        svg_list_logged = true;
                    }

                    if (!available_svgs.empty()) {
                        // Get current active SVG for highlighting active buttons
                        std::string current_svg = svg_renderer->GetCurrentSVGPath();

                        // 16x9 Formats
                        ImGui::TextDisabled("16:9:");

                        // Highlight if this SVG is currently active
                        bool is_16x9_active = (current_svg == GetAssetPath("assets/safety/16x9.svg"));
                        if (is_16x9_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("16:9 Broadcast")) {
                            Debug::Log("*** BUTTON CLICKED: 16:9 Standard ***");
                            Debug::Log("Loading 16:9 standard SVG");
                            bool result = svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/16x9.svg"));
                            Debug::Log("LoadSafetyOverlaySVG result: " + std::string(result ? "SUCCESS" : "FAILED"));
                            video_player->EnableSafetyOverlays(true);
                            Debug::Log("EnableSafetyOverlays(true) called");
                        }

                        if (is_16x9_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_youtube_masthead_active = (current_svg == GetAssetPath("assets/safety/Youtube_16x9_Masthead.svg"));
                        if (is_youtube_masthead_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("YouTube Masthead 16:9")) {
                            Debug::Log("Loading YouTube Masthead SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Youtube_16x9_Masthead.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_masthead_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();

                        bool is_youtube_16x9_active = (current_svg == GetAssetPath("assets/safety/Youtube_16x9.svg"));
                        if (is_youtube_16x9_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("YouTube 16:9")) {
                            Debug::Log("Loading YouTube 16:9 SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Youtube_16x9.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_16x9_active) {
                            ImGui::PopStyleColor();
                        }

                        ImGui::Spacing();
                        ImGui::TextDisabled("9:16 Vertical:");

                        if (ImGui::SmallButton("TikTok 9:16")) {
                            Debug::Log("*** BUTTON CLICKED: TikTok 9:16 ***");
                            Debug::Log("Loading TikTok SVG");
                            bool result = svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/TikTok_9x16.svg"));
                            Debug::Log("LoadSafetyOverlaySVG result: " + std::string(result ? "SUCCESS" : "FAILED"));
                            video_player->EnableSafetyOverlays(true);
                            Debug::Log("EnableSafetyOverlays(true) called");
                        }
                        ImGui::SameLine();

                        bool is_youtube_9x16_active = (current_svg == GetAssetPath("assets/safety/Youtube_9x16.svg"));
                        if (is_youtube_9x16_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("YouTube 9:16")) {
                            Debug::Log("Loading YouTube 9:16 SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Youtube_9x16.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_9x16_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_meta_reels_active = (current_svg == GetAssetPath("assets/safety/Meta_Reels_9x16.svg"));
                        if (is_meta_reels_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("Meta Reels 9:16")) {
                            Debug::Log("Loading Meta Reels SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Meta_Reels_9x16.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_meta_reels_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();

                        bool is_meta_stories_active = (current_svg == GetAssetPath("assets/safety/Meta_Stories_9x16.svg"));
                        if (is_meta_stories_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("Meta Stories 9x16")) {
                            Debug::Log("Loading Meta Stories SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Meta_Stories_9x16.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_meta_stories_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_pinterest_9x16_active = (current_svg == GetAssetPath("assets/safety/Pinterest_9x16.svg"));
                        if (is_pinterest_9x16_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("Pinterest 9:16")) {
                            Debug::Log("Loading Pinterest 9:16 SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Pinterest_9x16.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_pinterest_9x16_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();
                        bool is_samsung_active = (current_svg == GetAssetPath("assets/safety/Samsung_9x16_Safety.svg"));
                        if (is_samsung_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("Samsung Safety 9:16")) {
                            Debug::Log("Loading Samsung Safety SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Samsung_9x16_Safety.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_samsung_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_snapchat_active = (current_svg == GetAssetPath("assets/safety/Snapchat_9x16_unofficial.svg"));
                        if (is_snapchat_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("Snapchat 9:16")) {
                            Debug::Log("Loading Snapchat SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Snapchat_9x16_unofficial.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_snapchat_active) {
                            ImGui::PopStyleColor();
                        }

                        ImGui::Spacing();
                        ImGui::TextDisabled("1x1 Square:");

                        bool is_youtube_1x1_active = (current_svg == GetAssetPath("assets/safety/Youtube_1x1.svg"));
                        if (is_youtube_1x1_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("YouTube 1:1")) {
                            Debug::Log("Loading YouTube 1:1 SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Youtube_1x1.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_1x1_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();
                        bool is_pinterest_1x1_active = (current_svg == GetAssetPath("assets/safety/Pinterest_PremiumSpotlight_1x1.svg"));
                        if (is_pinterest_1x1_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("Pinterest Spotlight 1:1")) {
                            Debug::Log("Loading Pinterest Premium Spotlight SVG");
                            svg_renderer->LoadSafetyOverlaySVG(GetAssetPath("assets/safety/Pinterest_PremiumSpotlight_1x1.svg"));
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_pinterest_1x1_active) {
                            ImGui::PopStyleColor();
                        }

                        ImGui::Spacing();
                        ImGui::TextDisabled("No Guides:");

                        // Highlight disable button if no overlays are active (consistent with other buttons)
                        bool is_disabled = current_svg.empty() || !video_player->IsSafetyOverlaysEnabled();
                        if (is_disabled) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::SmallButton("None / Disable")) {
                            Debug::Log("Disabling safety overlays");
                            video_player->EnableSafetyOverlays(false);
                            // Clear the loaded SVG so other buttons become unhighlighted
                            svg_renderer->ClearSVG();
                        }

                        if (is_disabled) {
                            ImGui::PopStyleColor();
                        }

                        // Add controls directly in this panel
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Text("Overlay Controls:");

                        // Opacity slider
                        if (ImGui::SliderFloat("Opacity", &safety_settings.opacity, 0.1f, 1.0f, "%.1f")) {
                            if (video_player) {
                                video_player->SetSafetyOverlaySettings(safety_settings);
                            }
                        }

                        // Line width slider
                        if (ImGui::SliderFloat("Line Width", &safety_settings.line_width, 1.0f, 5.0f, "%.1f")) {
                            if (video_player) {
                                video_player->SetSafetyOverlaySettings(safety_settings);
                            }
                        }

                        // Embedded color picker
                        ImGui::Text("Color:");
                        ImGuiColorEditFlags color_flags = ImGuiColorEditFlags_NoInputs |
                            ImGuiColorEditFlags_NoLabel |
                            ImGuiColorEditFlags_NoPicker |
                            ImGuiColorEditFlags_AlphaPreview;

                        if (ImGui::ColorEdit3("##overlay_color", safety_settings.color, color_flags)) {
                            if (video_player) {
                                video_player->SetSafetyOverlaySettings(safety_settings);
                            }
                        }

                        // Add embedded color picker widget
                        ImGui::SameLine();
                        if (ImGui::ColorPicker3("##overlay_picker", safety_settings.color,
                            ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_NoSmallPreview |
                            ImGuiColorEditFlags_NoInputs)) {
                            if (video_player) {
                                video_player->SetSafetyOverlaySettings(safety_settings);
                            }
                        }
                   
                    }
                }
            }

        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // Handle click-outside-to-close
        static int safety_panel_open_frames = 0;
        if (show_panel) {
            safety_panel_open_frames++;
            if (safety_panel_open_frames > 3 && !panel_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                show_panel = false;
            }
        }
        else {
            safety_panel_open_frames = 0;
        }
    }


    void Application::RenderColorspacePresetsPanel(bool& show_panel) {
        if (!show_panel) return;

        ImGuiWindow* video_window = ImGui::FindWindowByName("Video Viewport");
        if (!video_window) return;

        ImVec2 video_pos = video_window->Pos;
        ImVec2 video_size = video_window->Size;

        // Scale panel dimensions with font (full width, dampened height)
        const float ui_scale = ImGui::GetIO().FontGlobalScale;
        const float height_scale = 1.0f + (ui_scale - 1.0f) * 0.65f;
        const float panel_width = S(400.0f);
        const float panel_height = S(480.0f);
        const float margin = S(10.0f);

        // Position in top-right corner (same as background panel)
        ImGui::SetNextWindowPos(ImVec2(
            video_pos.x + video_size.x - panel_width - margin,
            video_pos.y + margin
        ), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height), ImGuiCond_Always);

        ImGuiWindowFlags panel_flags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 2.0f));

        bool panel_hovered = false;

        if (ImGui::Begin("##ColorspacePresetsPanel", &show_panel, panel_flags)) {
            panel_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

            // Style for all collapsing headers
            PushOutlineHeaderStyle();

            // Standard Presets Section (using Blender config internally)
            if (ImGui::CollapsingHeader("Standard Workflows##PresetPanel", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushID("StandardPresets##Panel");
                ImGui::Indent(S(8.0f));
                CreateStandardPresets();
                ImGui::Unindent(8.0f);
                ImGui::PopID();
            }

            // ACES 1.3 Presets Section
            if (ImGui::CollapsingHeader("ACES 1.3 Workflows##PresetPanel", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushID("ACES13Presets##Panel");
                ImGui::Indent(S(8.0f));
                CreateACESPresets();
                ImGui::Unindent(8.0f);
                ImGui::PopID();
            }

            // ACES 2.0 Presets Section
            if (ImGui::CollapsingHeader("ACES 2.0 Workflows##PresetPanel")) {
                ImGui::PushID("ACES20Presets##Panel");
                ImGui::Indent(S(8.0f));
                CreateACES20Presets();
                ImGui::Unindent(8.0f);
                ImGui::PopID();
            }

            // Blender 5.0 Presets Section
            if (ImGui::CollapsingHeader("Blender 5.0 Workflows##PresetPanel", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushID("Blender50Presets##Panel");
                ImGui::Indent(S(8.0f));
                CreateBlender5Presets();
                ImGui::Unindent(8.0f);
                ImGui::PopID();
            }

            PopOutlineHeaderStyle();
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // Handle click-outside-to-close
        static int colorspace_panel_open_frames = 0;
        if (show_panel) {
            colorspace_panel_open_frames++;
            if (colorspace_panel_open_frames > 5) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !panel_hovered) {
                    show_panel = false;
                }
            }
        }
        else {
            colorspace_panel_open_frames = 0;
        }
    }
