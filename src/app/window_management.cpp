// ============================================================================
// Window management, file operations, and utility methods
// ============================================================================

#include "app/application.h"
#include "app/app_config.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/timecode.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "annotations/annotation_manager.h"
#include "ui/annotation_panel.h"
#include "ui/timeline_manager.h"
#include "player/video_player.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <nfd.h>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iostream>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <GLFW/glfw3native.h>
#endif

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_icons;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern bool auto_play_buffering;
extern std::chrono::steady_clock::time_point auto_play_buffer_start;
extern bool pending_seek_cache_start;
extern std::chrono::steady_clock::time_point seek_cache_start_timer;

    // ------------------------------------------------------------------------
    // FILE OPERATIONS
    // ------------------------------------------------------------------------
    void Application::OpenFileDialog() {
        const nfdpathset_t* outPaths = nullptr;

        // Supported formats: Video (MP4/AVI/MKV/MOV/etc), Audio (WAV/MP3/etc), Images (JPEG/PNG/TIFF/EXR)
        nfdfilteritem_t filterItem[1] = {
            { "Media Files", "mp4,avi,mkv,mov,wmv,flv,webm,m4v,3gp,ogv,ts,mts,m2ts,mxf,gif,wav,mp3,aac,flac,ogg,m4a,wma,jpg,jpeg,png,tiff,tif,exr,hdr" }
        };

        // Retry logic for cloud storage resilience
        const int max_attempts = 3;
        const int retry_delay_ms = 500;
        nfdresult_t result = NFD_ERROR;
        int attempt = 0;

        for (attempt = 1; attempt <= max_attempts; attempt++) {
            if (attempt > 1) {
                Debug::Log("OpenFileDialog: Retrying NFD (attempt " + std::to_string(attempt) + "/" + std::to_string(max_attempts) + ")");
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
            }

            // Use multi-select dialog
            result = NFD_OpenDialogMultiple(&outPaths, filterItem, 1, nullptr);

            if (result == NFD_OKAY || result == NFD_CANCEL) {
                break;  // Success or user cancelled - stop retrying
            }

            // NFD_ERROR - may be due to cloud sync issues, try again
            Debug::Log("OpenFileDialog: NFD error on attempt " + std::to_string(attempt) + ": " + std::string(NFD_GetError()));
        }

        if (result == NFD_OKAY && outPaths) {
            // Get number of selected files
            nfdpathsetsize_t count = 0;
            NFD_PathSet_GetCount(outPaths, &count);

            Debug::Log("OpenFileDialog: " + std::to_string(count) + " file(s) selected");

            if (count == 1) {
                // Single file - load and select it (like before)
                nfdchar_t* path = nullptr;
                if (NFD_PathSet_GetPath(outPaths, 0, &path) == NFD_OKAY && path) {
                    std::string selected_file = std::string(path);
                    Debug::Log("*** Opening single file: " + selected_file);

                    if (project_manager) {
                        project_manager->LoadSingleFileFromDrop(selected_file);
                    } else {
                        current_file_path = selected_file;
                        if (video_player) {
                            video_player->LoadFile(selected_file);
                        }
                    }
                    NFD_PathSet_FreePath(path);
                }
            } else if (count > 1) {
                // Multiple files - add to project manager without loading any
                std::vector<std::string> filePaths;
                for (nfdpathsetsize_t i = 0; i < count; i++) {
                    nfdchar_t* path = nullptr;
                    if (NFD_PathSet_GetPath(outPaths, i, &path) == NFD_OKAY && path) {
                        filePaths.push_back(std::string(path));
                        NFD_PathSet_FreePath(path);
                    }
                }

                Debug::Log("*** Opening multiple files (" + std::to_string(filePaths.size()) + ") - adding to project");
                if (project_manager && !filePaths.empty()) {
                    project_manager->LoadMultipleFilesFromDrop(filePaths);
                }
            }

            NFD_PathSet_Free(outPaths);
        }
        else if (result == NFD_CANCEL) {
            Debug::Log("OpenFileDialog: User cancelled");
        }
        else {
            Debug::Log("OpenFileDialog: Failed after " + std::to_string(max_attempts) + " attempts");
            std::cerr << "Error opening file dialog: " << NFD_GetError() << std::endl;
        }
    }

    void Application::TriggerAutoPlay(qcview::MediaType media_type) {
        // Auto-play is buffer-aware: waits for 90% cache fill before starting
        // All media starts PAUSED, then auto-plays once buffer is ready (if enabled)
        (void)media_type;  // Unused now - all types supported

        if (cache_settings.auto_play_on_load) {
            auto_play_buffering = true;
            auto_play_buffer_start = std::chrono::steady_clock::now();
            Debug::Log("Auto-play: Buffering started - will play when 90% cache fill reached");
        } else {
            Debug::Log("Auto-play: Disabled by user settings - staying paused");
        }
    }

    void Application::TriggerSeekCacheStart() {
        pending_seek_cache_start = true;
        seek_cache_start_timer = std::chrono::steady_clock::now();
        Debug::Log("Seek cache: Delayed start timer set (1000ms delay)");
    }

    void Application::AddToRecentFiles(const std::string& file_path) {
        recent_files.erase(
            std::remove(recent_files.begin(), recent_files.end(), file_path),
            recent_files.end()
        );

        recent_files.insert(recent_files.begin(), file_path);

        if (recent_files.size() > max_recent_files) {
            recent_files.resize(max_recent_files);
        }
    }

    // ------------------------------------------------------------------------
    // UTILITY METHODS
    // ------------------------------------------------------------------------

    void Application::ResetTimecodeStateForNewVideo() {
        timecode_state = NOT_CHECKED;
        start_timecode_checked = false;
        timecode_mode_enabled = false;  // Disable timecode mode when switching videos
        cached_start_timecode = "";
        Debug::Log("Timecode state reset for new video");
    }

    void Application::OnVideoChanged(const std::string& new_file_path) {
        Debug::Log("=== OnVideoChanged CALLBACK ===");
        Debug::Log("  New file path: " + new_file_path);
        Debug::Log("  Is EDL: " + std::string(new_file_path.find("edl://") == 0 ? "YES" : "NO"));

        // View state caching is handled by PreVideoChangeCallback BEFORE the load happens
        // Update current file path (includes EDL paths)
        current_file_path = new_file_path;
        Debug::Log("  current_file_path updated");

        if (video_player) {
            Debug::Log("  Video player state:");
            Debug::Log("    IsAudioOnly: " + std::string(video_player->IsAudioOnly() ? "true" : "false"));
            Debug::Log("    HasVideo: " + std::string(video_player->HasVideo() ? "true" : "false"));
            Debug::Log("    HasAudio: " + std::string(video_player->HasAudio() ? "true" : "false"));
            Debug::Log("    Duration: " + std::to_string(video_player->GetDuration()));
        }

        ResetTimecodeStateForNewVideo();

        // Reset trim mode when video changes (unless we're loading a trimmed version)
        if (trim_mode_left && original_video_path_left != new_file_path &&
            new_file_path.rfind("edl://", 0) != 0) {
            trim_mode_left = false;
            original_video_path_left.clear();
            if (project_manager) {
                project_manager->ClearInOutPoints();  // Clear loop points when exiting trimmed playback
            }
            Debug::Log("Trim mode (left) reset due to video change");
        }

        if (trim_mode_right) {
            // Note: Right player trim mode is handled by comparison video load, not OnVideoChanged
            // Only reset if dual view mode is disabled
            if (!(timeline_view && timeline_view->IsDualViewMode())) {
                trim_mode_right = false;
                original_video_path_right.clear();
                if (project_manager) {
                    project_manager->ClearInOutPoints();  // Clear loop points when exiting trimmed playback
                }
                Debug::Log("Trim mode (right) reset due to dual view mode disabled");
            }
        }

        // Check if this is an audio file (no video frames to cache)
        bool is_audio_file = false;
        if (project_manager) {
            // Use file extension to check if audio
            size_t dot_pos = new_file_path.find_last_of('.');
            if (dot_pos != std::string::npos) {
                std::string ext = new_file_path.substr(dot_pos);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                is_audio_file = (ext == ".wav" || ext == ".mp3" || ext == ".aac" ||
                                ext == ".flac" || ext == ".ogg" || ext == ".wma" || ext == ".m4a" ||
                                ext == ".aiff" || ext == ".aif");
            }
        }

        // Notify timeline manager about the new video file for cache handling
        // But avoid redundant calls for the same file to prevent performance issues
        // Skip for audio files (no video frames to cache)
        static std::string last_notified_path;
        if (timeline_manager && new_file_path != last_notified_path && !is_audio_file) {
            timeline_manager->SetVideoFile(new_file_path);
            last_notified_path = new_file_path;
        } else if (is_audio_file) {
            Debug::Log("OnVideoChanged: Skipping timeline cache for audio file");
            last_notified_path = new_file_path; // Still update to avoid redundant checks
        }

        // Load annotations for the new media file
        // Handle different timeline source modes for annotation availability
        if (annotation_manager && annotation_panel) {
            if (timeline_view) {
                auto source_mode = timeline_view->GetSourceMode();

                if (source_mode == qcview::TimelineSourceMode::DUAL_VIEW) {
                    // Dual view - disable annotations
                    annotation_panel->SetAvailability(qcview::AnnotationAvailability::DUAL_VIEW_DISABLED);
                    annotation_manager->ClearNotes();
                    Debug::Log("Annotations disabled for Dual View mode");
                }
                else if (source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                    // Playlist - disable annotations (not supported in playlist mode)
                    annotation_panel->SetAvailability(qcview::AnnotationAvailability::PLAYLIST_DISABLED);
                    annotation_manager->ClearNotes();
                    Debug::Log("Annotations disabled for Playlist mode");
                }
                else {
                    // VIDEO_FILE, IMAGE_SEQUENCE, AUDIO_FILE - normal media-relative path
                    annotation_panel->SetAvailability(qcview::AnnotationAvailability::AVAILABLE);
                    std::string annotation_path = new_file_path;
                    if (project_manager) {
                        annotation_path = project_manager->GetAnnotationPathForMedia(new_file_path);
                    }
                    annotation_manager->LoadNotesForMedia(annotation_path);
                    Debug::Log("Loaded annotations for: " + annotation_path);
                }
            }
            else {
                // No timeline view - use basic annotation loading
                annotation_panel->SetAvailability(qcview::AnnotationAvailability::AVAILABLE);
                std::string annotation_path = new_file_path;
                if (project_manager) {
                    annotation_path = project_manager->GetAnnotationPathForMedia(new_file_path);
                }
                annotation_manager->LoadNotesForMedia(annotation_path);
                Debug::Log("Loaded annotations for: " + annotation_path);
            }
        }

        // NEW: Detect media type from file path for deliberate autoplay control
        qcview::MediaType media_type = qcview::MediaType::VIDEO;  // Default
        if (new_file_path.substr(0, 5) == "mf://") {
            media_type = qcview::MediaType::IMAGE_SEQUENCE;
        } else if (new_file_path.substr(0, 6) == "exr://") {
            media_type = qcview::MediaType::EXR_SEQUENCE;
        } else if (is_audio_file) {
            media_type = qcview::MediaType::AUDIO;
        }

        // Trigger auto-play if enabled (with 500ms delay)
        // Image sequences will be skipped automatically by TriggerAutoPlay
        TriggerAutoPlay(media_type);

        // Trigger delayed seek cache start (2s delay to reduce initial load contention)
        TriggerSeekCacheStart();
    }

    std::string Application::FormatTime(double seconds) {
        int hours = (int)(seconds / 3600);
        int minutes = (int)(fmod(seconds, 3600.0) / 60);
        int secs = (int)fmod(seconds, 60.0);

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, secs);
        return std::string(buffer);
    }

    void Application::ShowAllPanels() {
        // Exit minimal view if active
        minimal_view_mode = false;

        // Show standard panels
        show_project_panel = true;
        show_inspector_panel = true;
        show_timeline_panel = true;
        show_annotation_panel = true;
        show_sidebar_panel = true;

        // Show color panels
        show_color_panels = true;

        first_time_setup = true;
        Debug::Log("All panels visible");
    }

    void Application::SetDefaultView() {
        // Exit minimal view if active
        minimal_view_mode = false;

        // Standard panels visible, others hidden
        show_project_panel = true;
        show_inspector_panel = true;
        show_timeline_panel = true;
        show_annotation_panel = false;
        show_color_panels = false;
        show_sidebar_panel = true;

        first_time_setup = true;
        Debug::Log("Default view activated");
    }

    void Application::ApplyBackgroundColor() {
        switch (video_background_type) {
        case VideoBackgroundType::DEFAULT:
            glClearColor(0.08f, 0.08f, 0.08f, 1.0f); // Dark professional background
            break;
        case VideoBackgroundType::BLACK:
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Pure black
            break;
        case VideoBackgroundType::DARK_CHECKERBOARD:
            glClearColor(0.15f, 0.15f, 0.15f, 1.0f); // Dark grey for checkerboard base
            break;
        case VideoBackgroundType::LIGHT_CHECKERBOARD:
            glClearColor(0.85f, 0.85f, 0.85f, 1.0f); // Light grey for checkerboard base
            break;
        }
    }

    void Application::ToggleMute() {
        if (!is_muted) {
            volume_before_mute = current_volume;
            current_volume = 0;
            // Mute audio
            if (timeline_view) {
                if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                    ctrl->SetMuted(true);
                }
            }
            is_muted = true;
            Debug::Log("Muted - stored volume: " + std::to_string(volume_before_mute));
        }
        else {
            current_volume = volume_before_mute;
            // Unmute audio
            if (timeline_view) {
                if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                    ctrl->SetMuted(false);
                    ctrl->SetVolume(current_volume / 100.0);
                }
            }
            is_muted = false;
            Debug::Log("Unmuted - restored volume: " + std::to_string(current_volume));
        }
    }

    void Application::ToggleLoop() {
        // Determine which mode we're in and get current loop state
        bool current_loop;
        bool is_timeline_mode = timeline_view && timeline_view->HasPlaybackController();

        if (is_timeline_mode) {
            auto* controller = timeline_view->GetEffectivePlaybackController();
            current_loop = controller ? controller->IsLooping() : video_player->IsLooping();
        } else {
            current_loop = video_player->IsLooping();
        }

        // Toggle both video player and timeline controller
        video_player->SetLoop(!current_loop);
        video_player->SetLoopMode(false);

        if (is_timeline_mode) {
            auto* controller = timeline_view->GetEffectivePlaybackController();
            if (controller) {
                controller->SetLooping(!current_loop);
            }
        }

        const char* mode = is_timeline_mode ? "Timeline" : "Single File";
        const char* state = !current_loop ? "ON" : "OFF";
        Debug::Log(std::string(mode) + " Loop toggled: " + state);
    }


    // Custom window procedure to intercept close button in fullscreen AND handle WM_COPYDATA
    LRESULT CALLBACK Application::CustomWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_SYSCOMMAND && wParam == SC_CLOSE && app_instance && app_instance->is_fullscreen) {
            // Intercept close button click in fullscreen - exit fullscreen instead of closing app
            app_instance->ToggleFullscreen();
            return 0;
        }

        // Handle inter-process file/URI messages from other instances
        if (uMsg == WM_COPYDATA && app_instance) {
            COPYDATASTRUCT* pcds = (COPYDATASTRUCT*)lParam;
            if (pcds->dwData == 1) {  // Our message ID
                std::string received_data = (char*)pcds->lpData;
                Debug::Log("Received command from another instance: " + received_data);

                // Parse the pipe-separated file paths
                std::vector<std::string> files;
                std::istringstream ss(received_data);
                std::string file;
                while (std::getline(ss, file, '|')) {
                    files.push_back(file);
                }

                // Process the files
                if (!files.empty() && app_instance->project_manager) {
                    if (files.size() == 1) {
                        std::string arg = files[0];

                        // Check if it's a qcview:// URI
                        if (arg.substr(0, 10) == "qcview:///") {
                            Debug::Log("Received qcview:// URI - parsing and loading project");
                            std::string project_path = app_instance->ParseProjectURI(arg);
                            if (!project_path.empty()) {
                                app_instance->project_manager->LoadProject(project_path);

                                // Show project panels for context
                                app_instance->show_project_panel = true;
                                app_instance->show_inspector_panel = true;
                                Debug::Log("Opened Project Manager and Inspector panels");
                            }
                        }
                        // Direct project file
                        else if (arg.find(".qcvproj") != std::string::npos || arg.find(".umproj") != std::string::npos) {
                            Debug::Log("Received project file - loading");
                            app_instance->project_manager->LoadProject(arg);
                        }
                        // Regular media file
                        else {
                            Debug::Log("Received media file - loading");
                            app_instance->project_manager->LoadSingleFileFromDrop(arg);
                        }
                    } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                        Debug::Log("Received multiple files - loading");
                        app_instance->show_project_panel = true;
                        app_instance->project_manager->LoadMultipleFilesFromDrop(files);
                    }
                }

                return TRUE;  // Message handled
            }
        }

        return CallWindowProc(original_wndproc, hwnd, uMsg, wParam, lParam);
    }

    void Application::SetupSingleInstanceMessaging(HWND hwnd) {
        // Store a unique property on the window so other instances can identify it
        // This is a simple, reliable method that doesn't require changing window classes
        SetPropW(hwnd, L"qcview_SingleInstanceWindow", (HANDLE)0x514356);  // "QCV" in hex

        // Hook window procedure to handle WM_COPYDATA
        app_instance = this;
        if (!original_wndproc) {
            original_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);
        }

        Debug::Log("Single-instance messaging setup complete - window tagged for IPC");
    }

    void Application::ToggleFullscreen() {
        static int saved_x, saved_y, saved_width, saved_height;

        is_fullscreen = !is_fullscreen;

        if (is_fullscreen) {
            Debug::Log("Entering borderless fullscreen");

            // Save current window state
            glfwGetWindowPos(window, &saved_x, &saved_y);
            glfwGetWindowSize(window, &saved_width, &saved_height);

            // Find the monitor the window is currently on
            int win_cx = saved_x + saved_width / 2;
            int win_cy = saved_y + saved_height / 2;
            int monitor_count = 0;
            GLFWmonitor** monitors = glfwGetMonitors(&monitor_count);
            GLFWmonitor* best_monitor = glfwGetPrimaryMonitor();
            for (int i = 0; i < monitor_count; i++) {
                int mx, my, mw, mh;
                glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
                if (win_cx >= mx && win_cx < mx + mw && win_cy >= my && win_cy < my + mh) {
                    best_monitor = monitors[i];
                    break;
                }
            }
            fullscreen_monitor = best_monitor;

            // Remove window decorations (no context rebuild)
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);

            // Cover entire monitor
            int mon_x, mon_y;
            glfwGetMonitorPos(fullscreen_monitor, &mon_x, &mon_y);
            const GLFWvidmode* mode = glfwGetVideoMode(fullscreen_monitor);
            glfwSetWindowPos(window, mon_x, mon_y);
            glfwSetWindowSize(window, mode->width, mode->height);
        }
        else {
            Debug::Log("Exiting borderless fullscreen");
            fullscreen_monitor = nullptr;

            // Restore window decorations
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);

            // Restore size and position
            glfwSetWindowSize(window, saved_width > 0 ? saved_width : 1914, saved_height > 0 ? saved_height : 1060);
            glfwSetWindowPos(window, saved_x, saved_y);
        }
    }

    void Application::CopyToClipboard(const std::string& text) {
        if (text.empty()) return;

#ifdef _WIN32
        if (OpenClipboard(NULL)) {
            EmptyClipboard();

            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.length() + 1);
            if (hMem) {
                char* pMem = static_cast<char*>(GlobalLock(hMem));
                strcpy_s(pMem, text.length() + 1, text.c_str());
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            }
            CloseClipboard();

            Debug::Log("Copied to clipboard: " + text);
        }
#else
        // Other platforms placeholder
#endif
    }

    void Application::OpenFileInExplorer(const std::string& file_path) {
        if (file_path.empty()) return;

#ifdef _WIN32
        std::string windows_path = file_path;
        std::replace(windows_path.begin(), windows_path.end(), '/', '\\');

        std::wstring wide_path(windows_path.begin(), windows_path.end());
        std::wstring params = L"/select,\"" + wide_path + L"\"";
        ShellExecuteW(NULL, L"open", L"explorer.exe", params.c_str(), NULL, SW_SHOWNORMAL);
#else
        // Placeholder for other platforms
#endif
    }

    void Application::RenderPathWithButtons(const std::string& path, const std::string& id, bool show_open_button) {
        ImGui::BeginGroup();

        float button_width = 28.0f;
        int button_count = show_open_button ? 2 : 1;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float available_width = ImGui::GetContentRegionAvail().x - (button_width * button_count) - (spacing * button_count);

        // Path text
        if (font_regular) ImGui::PushFont(font_regular);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + available_width);
        ImGui::TextWrapped("%s", path.c_str());
        ImGui::PopTextWrapPos();
        if (font_regular) ImGui::PopFont();

        // Open button (only on Windows for Windows paths)
        if (show_open_button) {
            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (font_icons) {
                ImGui::PushFont(font_icons);
                if (ImGui::Button((std::string(ICON_FOLDER_OPEN) + "##open_" + id).c_str(), ImVec2(button_width, 0))) {
                    OpenFileInExplorer(path);
                }
                ImGui::PopFont();
            }
            else {
                if (ImGui::Button((std::string("..##open_") + id).c_str(), ImVec2(button_width, 0))) {
                    OpenFileInExplorer(path);
                }
            }
            PopOutlineButtonStyle();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open in Explorer");
            }
        }

        // Copy button
        ImGui::SameLine();

        PushOutlineButtonStyle();
        if (font_icons) {
            ImGui::PushFont(font_icons);
            if (ImGui::Button((std::string(ICON_CONTENT_COPY) + "##copy_" + id).c_str(), ImVec2(button_width, 0))) {
                CopyToClipboard(path);

                ImGui::SetTooltip("Copied!");
            }
            ImGui::PopFont();
        }
        else {
            if (ImGui::Button((std::string("C##copy_") + id).c_str(), ImVec2(button_width, 0))) {
                CopyToClipboard(path);
                ImGui::SetTooltip("Copied!");
            }
        }
        PopOutlineButtonStyle();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
            ImGui::SetTooltip("Copy to clipboard");
        }

        ImGui::EndGroup();
    }

    std::string Application::GetFileName(const std::string& path) {
        size_t pos = path.find_last_of("/\\");
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

// Static member definitions
WNDPROC Application::original_wndproc = nullptr;
Application* Application::app_instance = nullptr;
