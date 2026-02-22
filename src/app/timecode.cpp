// ============================================================================
// Timecode formatting, parsing, state management, and Go To modal
// ============================================================================

#include "app/timecode.h"
#include "app/application.h"
#include "app/app_ui_macros.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include "utils/frame_indexing.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include <imgui.h>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

// ============================================================================
// Timecode global variable definitions
// ============================================================================
bool timecode_mode_enabled = false;
std::string cached_start_timecode = "";
bool start_timecode_checked = false;
TimecodeState timecode_state = NOT_CHECKED;

// Go To Timecode/Frame modal state
bool show_goto_timecode_modal = false;
bool goto_modal_preserve_pause_state = false;
bool goto_modal_was_playing = false;
char goto_timecode_hours[3] = "00";
char goto_timecode_minutes[3] = "00";
char goto_timecode_seconds[3] = "00";
char goto_timecode_frames[3] = "00";
char goto_frame_buffer[16] = "0";
bool goto_use_frame_input = false;  // false = timecode, true = frame number

// Globals defined in main.cpp
extern ImFont* font_regular;
extern bool otio_timeline_mode;
extern std::unique_ptr<qcview::TimelineView> timeline_view;

// ============================================================================
// TIMECODE METHODS
// ============================================================================

std::string Application::GetTimecodeOffset() const {
    // Parse cached_start_timecode (format: HH:MM:SS:FF or HH:MM:SS.sss)
    // Return the offset in seconds as a double, then format

    if (cached_start_timecode.empty()) return "00:00:00:00";

    // Simple parsing for HH:MM:SS:FF format
    std::istringstream ss(cached_start_timecode);
    std::string segment;
    std::vector<std::string> parts;

    while (std::getline(ss, segment, ':')) {
        parts.push_back(segment);
    }

    if (parts.size() >= 3) {
        int hours = std::stoi(parts[0]);
        int minutes = std::stoi(parts[1]);
        int seconds = std::stoi(parts[2]);
        int frames = (parts.size() > 3) ? std::stoi(parts[3]) : 0;

        // Calculate total offset in seconds
        double offset_seconds = hours * 3600 + minutes * 60 + seconds;

        return cached_start_timecode; // Return the original for now
    }

    return "00:00:00:00";
}

double Application::ParseTimecodeToSeconds(const std::string& timecode_str, double fps) {
    // Handle formats like "01:23:45:12" or "01:23:45.500"
    std::istringstream ss(timecode_str);
    std::string segment;
    std::vector<std::string> parts;

    // Parse colon-separated format: HH:MM:SS:FF
    while (std::getline(ss, segment, ':')) {
        parts.push_back(segment);
    }

    if (parts.size() >= 3) {
        double hours = std::stod(parts[0]);
        double minutes = std::stod(parts[1]);
        double seconds = std::stod(parts[2]);
        double frames = (parts.size() > 3) ? std::stod(parts[3]) : 0.0;

        // Convert frames to fractional seconds using actual FPS
        double frame_fraction = frames / fps;

        return hours * 3600 + minutes * 60 + seconds + frame_fraction;
    }

    return 0.0; // Default if parsing fails
}

void Application::ResetTimecodeState() {
    timecode_state = NOT_CHECKED;
    start_timecode_checked = false;
    timecode_mode_enabled = false;
    cached_start_timecode = "";
    Debug::Log("Timecode state reset for new file");
}

// ============================================================================
// GO TO TIMECODE/FRAME HELPER FUNCTIONS
// ============================================================================

// Smart timecode parsing - handles various formats:
// - "00:00:01:24" or "00:00:01;24" (full timecode)
// - "01:24" or "0124" or "000124" (frames or seconds:frames)
bool Application::ParseFlexibleTimecode(const std::string& input, int& hours, int& minutes, int& seconds, int& frames) {
    // Remove leading/trailing whitespace
    std::string trimmed = input;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);

    if (trimmed.empty()) return false;

    // Replace semicolons with colons (drop-frame timecode format)
    for (char& c : trimmed) {
        if (c == ';') c = ':';
    }

    // Split by colons
    std::vector<int> parts;
    std::istringstream ss(trimmed);
    std::string segment;

    while (std::getline(ss, segment, ':')) {
        if (!segment.empty()) {
            try {
                parts.push_back(std::stoi(segment));
            } catch (...) {
                return false;
            }
        }
    }

    // If no colons (or only one part), try parsing as pure digits
    if (parts.size() <= 1) {
        // Check if it's all digits
        bool all_digits = true;
        for (char c : trimmed) {
            if (!isdigit(c)) {
                all_digits = false;
                break;
            }
        }

        if (all_digits && trimmed.length() >= 2) {
            try {
                int value = std::stoi(trimmed);

                // Interpret based on length:
                // 2 digits: frames only (e.g., "24")
                // 4 digits: seconds:frames (e.g., "0124" = 01:24)
                // 6 digits: minutes:seconds:frames (e.g., "010124" = 01:01:24)
                if (trimmed.length() == 2) {
                    hours = 0; minutes = 0; seconds = 0;
                    frames = value;
                    return true;
                } else if (trimmed.length() == 4) {
                    hours = 0; minutes = 0;
                    seconds = value / 100;
                    frames = value % 100;
                    return true;
                } else if (trimmed.length() == 6) {
                    hours = 0;
                    minutes = value / 10000;
                    seconds = (value / 100) % 100;
                    frames = value % 100;
                    return true;
                } else if (trimmed.length() == 8) {
                    hours = value / 1000000;
                    minutes = (value / 10000) % 100;
                    seconds = (value / 100) % 100;
                    frames = value % 100;
                    return true;
                }
            } catch (...) {
                return false;
            }
        }
        return false;
    }

    // Parse based on number of colon-separated parts
    if (parts.size() == 2) {
        // SS:FF format
        hours = 0; minutes = 0;
        seconds = parts[0];
        frames = parts[1];
        return true;
    } else if (parts.size() == 3) {
        // MM:SS:FF format
        hours = 0;
        minutes = parts[0];
        seconds = parts[1];
        frames = parts[2];
        return true;
    } else if (parts.size() == 4) {
        // HH:MM:SS:FF format
        hours = parts[0];
        minutes = parts[1];
        seconds = parts[2];
        frames = parts[3];
        return true;
    }

    return false;
}

// Parse flexible frame number formats: "12", "012", "000000012"
bool Application::ParseFlexibleFrameNumber(const std::string& input, int& frame_number) {
    std::string trimmed = input;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);

    if (trimmed.empty()) return false;

    // Check if all digits
    for (char c : trimmed) {
        if (!isdigit(c)) return false;
    }

    try {
        frame_number = std::stoi(trimmed);
        return true;
    } catch (...) {
        return false;
    }
}

// Open the goto timecode modal with current position
void Application::OpenGotoTimecodeModal() {
    // Check if we're in OTIO timeline mode
    bool is_otio_mode = otio_timeline_mode && timeline_view && timeline_view->HasPlaybackController();

    if (!is_otio_mode && (!video_player || !video_player->HasVideo())) return;

    // Check if we're in solo video timecode mode
    bool is_solo_video_tc_mode = !is_otio_mode &&
        timecode_mode_enabled &&
        timecode_state == AVAILABLE &&
        !cached_start_timecode.empty();

    // Preserve pause state and pause playback
    if (is_otio_mode) {
        if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
            goto_modal_was_playing = ctrl->IsPlaying();
            if (goto_modal_was_playing) {
                ctrl->Pause();
            }
        }
    } else {
        goto_modal_was_playing = video_player->IsPlaying();
        if (goto_modal_was_playing) {
            video_player->Pause();
        }
    }

    // Get current position and fps
    double fps, current_pos;
    int current_frame;
    if (is_otio_mode) {
        fps = timeline_view->GetFrameRate();
        if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
            current_pos = ctrl->GetPosition();
            current_frame = ctrl->GetCurrentFrame();
        } else {
            current_pos = 0.0;
            current_frame = 0;
        }
    } else {
        fps = video_player->GetFrameRate();
        current_pos = video_player->GetPosition();
        current_frame = video_player->GetCurrentFrame();
    }

    // In solo video timecode mode, add the start timecode offset
    // so we display embedded timecode values
    double display_pos = current_pos;
    if (is_solo_video_tc_mode) {
        double timecode_offset = ParseTimecodeToSeconds(cached_start_timecode, fps);
        display_pos = current_pos + timecode_offset;
    }

    // Populate timecode fields with current position (or embedded timecode)
    int hours = (int)(display_pos / 3600);
    int minutes = (int)(fmod(display_pos, 3600.0) / 60);
    int secs = (int)fmod(display_pos, 60.0);
    int frames = (int)((display_pos - (int)display_pos) * fps);

    snprintf(goto_timecode_hours, sizeof(goto_timecode_hours), "%02d", hours);
    snprintf(goto_timecode_minutes, sizeof(goto_timecode_minutes), "%02d", minutes);
    snprintf(goto_timecode_seconds, sizeof(goto_timecode_seconds), "%02d", secs);
    snprintf(goto_timecode_frames, sizeof(goto_timecode_frames), "%02d", frames);

    // Populate frame field
    int display_frame = current_frame;
    if (is_otio_mode) {
        // OTIO timelines use 0-based internal frames, convert to 1-based display
        display_frame = qcview::FrameIndexing::InternalToDisplay(current_frame);
    } else if (video_player->IsInEXRMode() || video_player->IsImageSequence()) {
        int start_frame = video_player->IsInEXRMode()
            ? video_player->GetEXRSequenceStartFrame()
            : video_player->GetImageSequenceStartFrame();
        display_frame = qcview::FrameIndexing::InternalToSequenceDisplay(current_frame, start_frame);
    } else if (is_solo_video_tc_mode) {
        // In timecode mode, show embedded frame number (offset by start timecode)
        double timecode_offset = ParseTimecodeToSeconds(cached_start_timecode, fps);
        int offset_frames = static_cast<int>(std::round(timecode_offset * fps));
        display_frame = current_frame + offset_frames + 1;  // +1 for 0-based to 1-based
    } else {
        display_frame = qcview::FrameIndexing::InternalToDisplay(current_frame);
    }
    snprintf(goto_frame_buffer, sizeof(goto_frame_buffer), "%d", display_frame);

    // Default to timecode input
    goto_use_frame_input = false;

    // Open modal
    show_goto_timecode_modal = true;
}

// Render the Go To Timecode/Frame modal
void Application::RenderGotoTimecodeModal() {
    // Open the popup if flag is set (MUST be before BeginPopupModal)
    if (show_goto_timecode_modal && !ImGui::IsPopupOpen("Go To Timecode/Frame")) {
        ImGui::OpenPopup("Go To Timecode/Frame");
    }

    if (!show_goto_timecode_modal) return;

    // Center modal on screen
    float scale = ImGui::GetIO().FontGlobalScale;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(450 * scale, 0), ImGuiCond_Appearing);

    // Open modal with darkened background
    if (ImGui::BeginPopupModal("Go To Timecode/Frame", &show_goto_timecode_modal,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {

        ImGui::PushFont(font_regular);

        // Check if we're in solo video timecode mode
        bool is_otio_active = otio_timeline_mode && timeline_view && timeline_view->HasPlaybackController();
        bool is_solo_video_tc_mode = !is_otio_active &&
            timecode_mode_enabled &&
            timecode_state == AVAILABLE &&
            !cached_start_timecode.empty();

        // Toggle between timecode and frame input
        ImGui::Text("Input Mode:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Timecode", !goto_use_frame_input)) {
            goto_use_frame_input = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Frame Number", goto_use_frame_input)) {
            goto_use_frame_input = true;
        }

        // Show timecode mode indicator when active
        if (is_solo_video_tc_mode) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, Bright(GetWindowsAccentColor()));
            ImGui::TextWrapped("Timecode Mode: Using embedded timecode (starts at %s)",
                cached_start_timecode.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool navigate = false;

        if (goto_use_frame_input) {
            // FRAME NUMBER INPUT MODE
            if (is_solo_video_tc_mode) {
                ImGui::Text("Embedded Frame Number:");
            } else {
                ImGui::Text("Frame Number:");
            }
            ImGui::Spacing();

            ImGui::PushItemWidth(200);

            // Detect paste in frame input
            bool input_active = ImGui::InputText("##frame_input", goto_frame_buffer,
                sizeof(goto_frame_buffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);

            // Check for paste operation
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_V) &&
                (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))) {

                // Get clipboard text
                const char* clipboard_text = ImGui::GetClipboardText();
                if (clipboard_text) {
                    int frame_num;
                    if (ParseFlexibleFrameNumber(clipboard_text, frame_num)) {
                        snprintf(goto_frame_buffer, sizeof(goto_frame_buffer), "%d", frame_num);
                    }
                }
            }

            if (input_active) {
                navigate = true;
            }

            ImGui::PopItemWidth();

        } else {
            // TIMECODE INPUT MODE (HH:MM:SS:FF)
            if (is_solo_video_tc_mode) {
                ImGui::Text("Embedded Timecode (HH:MM:SS:FF):");
            } else {
                ImGui::Text("Timecode (HH:MM:SS:FF):");
            }
            ImGui::Spacing();

            ImGui::PushItemWidth(60);

            // Four separate input fields
            bool hours_changed = ImGui::InputText("##hours", goto_timecode_hours,
                sizeof(goto_timecode_hours),
                ImGuiInputTextFlags_CharsDecimal);

            // Detect paste in hours field
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_V) &&
                (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))) {

                const char* clipboard_text = ImGui::GetClipboardText();
                if (clipboard_text) {
                    int h, m, s, f;
                    if (ParseFlexibleTimecode(clipboard_text, h, m, s, f)) {
                        snprintf(goto_timecode_hours, sizeof(goto_timecode_hours), "%02d", h);
                        snprintf(goto_timecode_minutes, sizeof(goto_timecode_minutes), "%02d", m);
                        snprintf(goto_timecode_seconds, sizeof(goto_timecode_seconds), "%02d", s);
                        snprintf(goto_timecode_frames, sizeof(goto_timecode_frames), "%02d", f);
                    }
                }
            }

            ImGui::SameLine(); ImGui::Text(":");
            ImGui::SameLine();

            bool minutes_changed = ImGui::InputText("##minutes", goto_timecode_minutes,
                sizeof(goto_timecode_minutes),
                ImGuiInputTextFlags_CharsDecimal);

            // Detect paste in minutes field
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_V) &&
                (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))) {

                const char* clipboard_text = ImGui::GetClipboardText();
                if (clipboard_text) {
                    int h, m, s, f;
                    if (ParseFlexibleTimecode(clipboard_text, h, m, s, f)) {
                        snprintf(goto_timecode_hours, sizeof(goto_timecode_hours), "%02d", h);
                        snprintf(goto_timecode_minutes, sizeof(goto_timecode_minutes), "%02d", m);
                        snprintf(goto_timecode_seconds, sizeof(goto_timecode_seconds), "%02d", s);
                        snprintf(goto_timecode_frames, sizeof(goto_timecode_frames), "%02d", f);
                    }
                }
            }

            ImGui::SameLine(); ImGui::Text(":");
            ImGui::SameLine();

            bool seconds_changed = ImGui::InputText("##seconds", goto_timecode_seconds,
                sizeof(goto_timecode_seconds),
                ImGuiInputTextFlags_CharsDecimal);

            // Detect paste in seconds field
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_V) &&
                (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))) {

                const char* clipboard_text = ImGui::GetClipboardText();
                if (clipboard_text) {
                    int h, m, s, f;
                    if (ParseFlexibleTimecode(clipboard_text, h, m, s, f)) {
                        snprintf(goto_timecode_hours, sizeof(goto_timecode_hours), "%02d", h);
                        snprintf(goto_timecode_minutes, sizeof(goto_timecode_minutes), "%02d", m);
                        snprintf(goto_timecode_seconds, sizeof(goto_timecode_seconds), "%02d", s);
                        snprintf(goto_timecode_frames, sizeof(goto_timecode_frames), "%02d", f);
                    }
                }
            }

            ImGui::SameLine(); ImGui::Text(":");
            ImGui::SameLine();

            bool frames_changed = ImGui::InputText("##frames", goto_timecode_frames,
                sizeof(goto_timecode_frames),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);

            // Detect paste in frames field
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_V) &&
                (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))) {

                const char* clipboard_text = ImGui::GetClipboardText();
                if (clipboard_text) {
                    int h, m, s, f;
                    if (ParseFlexibleTimecode(clipboard_text, h, m, s, f)) {
                        snprintf(goto_timecode_hours, sizeof(goto_timecode_hours), "%02d", h);
                        snprintf(goto_timecode_minutes, sizeof(goto_timecode_minutes), "%02d", m);
                        snprintf(goto_timecode_seconds, sizeof(goto_timecode_seconds), "%02d", s);
                        snprintf(goto_timecode_frames, sizeof(goto_timecode_frames), "%02d", f);
                    }
                }
            }

            if (frames_changed) {
                navigate = true;
            }

            ImGui::PopItemWidth();

            ImGui::Spacing();
            ImGui::TextWrapped("Tip: Paste any timecode format (00:00:01:24, 0124, 01:24) in any field");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Buttons (flush right)
        float btnPadding = 8.0f * 2;
        float gotoW = ImGui::CalcTextSize("Go To").x + btnPadding;
        float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
        float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - gotoW - cancelW - btnSpacing);

        PushOutlineButtonStyle();
        if (ImGui::Button("Go To") || navigate) {
            // Perform navigation
            // Check if we're in OTIO timeline mode
            bool is_otio_mode = otio_timeline_mode && timeline_view && timeline_view->HasPlaybackController();

            // Check if we're in solo video mode with timecode mode enabled
            // (timecode mode only applies to VIDEO_FILE, not PLAYLIST/DUAL_VIEW/IMAGE_SEQUENCE)
            bool is_solo_video_timecode_mode = !is_otio_mode &&
                timecode_mode_enabled &&
                timecode_state == AVAILABLE &&
                !cached_start_timecode.empty();

            if (is_otio_mode || (video_player && video_player->HasVideo())) {
                // Get fps from timeline or video player
                double fps = is_otio_mode ? timeline_view->GetFrameRate() : video_player->GetFrameRate();
                double target_position = 0.0;

                // Get timecode offset for solo video timecode mode
                double timecode_offset = 0.0;
                if (is_solo_video_timecode_mode) {
                    timecode_offset = ParseTimecodeToSeconds(cached_start_timecode, fps);
                }

                if (goto_use_frame_input) {
                    // Parse frame number
                    int target_frame;
                    if (ParseFlexibleFrameNumber(goto_frame_buffer, target_frame)) {
                        // Convert display frame to internal frame
                        int internal_frame = target_frame;
                        if (is_otio_mode) {
                            // OTIO timelines use 0-based internal frames
                            internal_frame = qcview::FrameIndexing::DisplayToInternal(target_frame);
                        } else if (video_player->IsInEXRMode() || video_player->IsImageSequence()) {
                            int start_frame = video_player->IsInEXRMode()
                                ? video_player->GetEXRSequenceStartFrame()
                                : video_player->GetImageSequenceStartFrame();
                            internal_frame = qcview::FrameIndexing::FileFrameToInternal(target_frame, start_frame);
                        } else if (is_solo_video_timecode_mode) {
                            // In timecode mode, frame number is embedded frame (offset by start timecode)
                            // Convert to internal frame by subtracting the timecode offset in frames
                            int offset_frames = static_cast<int>(std::round(timecode_offset * fps));
                            internal_frame = target_frame - offset_frames - 1;  // -1 for 1-based to 0-based
                            if (internal_frame < 0) internal_frame = 0;
                        } else {
                            internal_frame = qcview::FrameIndexing::DisplayToInternal(target_frame);
                        }

                        // Convert frame to time position (use center of frame for robust seeking)
                        target_position = (static_cast<double>(internal_frame) + 0.5) / fps;
                    }
                } else {
                    // Parse timecode
                    int hours = atoi(goto_timecode_hours);
                    int minutes = atoi(goto_timecode_minutes);
                    int seconds = atoi(goto_timecode_seconds);
                    int frames = atoi(goto_timecode_frames);

                    // Convert to seconds (use center of frame for robust seeking)
                    double entered_timecode_seconds = hours * 3600.0 + minutes * 60.0 + seconds + ((static_cast<double>(frames) + 0.5) / fps);

                    // In solo video timecode mode, entered timecode is embedded timecode
                    // Subtract the start offset to get actual playback position
                    if (is_solo_video_timecode_mode) {
                        target_position = entered_timecode_seconds - timecode_offset;
                        if (target_position < 0) target_position = 0;
                    } else {
                        target_position = entered_timecode_seconds;
                    }
                }

                // Seek via appropriate controller
                AutoSaveAnnotationOnSeek();
                if (is_otio_mode) {
                    if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                        ctrl->Seek(target_position);
                    }
                } else {
                    video_player->Seek(target_position);
                }
            }

            // Close modal
            show_goto_timecode_modal = false;
            ImGui::CloseCurrentPopup();
        }
        PopOutlineButtonStyle();

        ImGui::SameLine();

        PushOutlineButtonStyle();
        if (ImGui::Button("Cancel")) {
            show_goto_timecode_modal = false;
            ImGui::CloseCurrentPopup();
        }
        PopOutlineButtonStyle();

        ImGui::PopFont();

        // Handle modal close - restore play state
        if (!show_goto_timecode_modal) {
            if (goto_modal_was_playing) {
                bool is_otio_mode = otio_timeline_mode && timeline_view && timeline_view->HasPlaybackController();
                if (is_otio_mode) {
                    if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                        ctrl->Play();
                    }
                } else if (video_player) {
                    video_player->Play();
                }
            }
        }

        ImGui::EndPopup();
    }
}

void Application::CheckStartTimecodeAvailability() {

    if (!project_manager || current_file_path.empty()) {
        Debug::Log("No project manager or empty file path");
        timecode_state = NOT_AVAILABLE;
        return;
    }

    // Skip timecode check for EXR sequences - they don't have embedded timecode
    if (current_file_path.substr(0, 6) == "exr://") {
        Debug::Log("Skipping timecode check for EXR sequence (no embedded timecode)");
        timecode_state = NOT_AVAILABLE;
        return;
    }

    // Skip timecode check for native image sequences (TIFF/PNG/JPEG) - they don't have embedded timecode
    if (current_file_path.substr(0, 5) == "mf://") {
        Debug::Log("Skipping timecode check for native image sequence (no embedded timecode)");
        timecode_state = NOT_AVAILABLE;
        return;
    }

    Debug::Log("Checking metadata for file: " + current_file_path);

    // Check if metadata is already cached
    const qcview::ProjectManager::CombinedMetadata* cached_meta =
        project_manager->GetCachedMetadata(current_file_path);

    if (cached_meta) {
        Debug::Log("Found cached metadata");

        if (cached_meta->adobe_meta) {
            Debug::Log("Adobe metadata exists");
            Debug::Log("Adobe metadata is_loaded: " + std::string(cached_meta->adobe_meta->is_loaded ? "TRUE" : "FALSE"));

            if (cached_meta->adobe_meta->is_loaded) {
                Debug::Log("Adobe metadata is fully loaded - checking for timecode");
                Debug::Log("HasAnyTimecode result: " + std::string(cached_meta->adobe_meta->HasAnyTimecode() ? "TRUE" : "FALSE"));
                Debug::Log("qt_start_timecode: '" + cached_meta->adobe_meta->qt_start_timecode + "'");

                // Helper to validate timecode format - rejects "0 s", "0", empty, and non-timecode values
                // Valid timecodes must contain ':' (e.g., "01:15:53:03" or "00:00:00:00")
                auto isValidTimecode = [](const std::string& tc) -> bool {
                    if (tc.empty()) return false;
                    // Reject "0 s", "0s", "0" - these are placeholder/invalid values from ExifTool
                    if (tc == "0 s" || tc == "0s" || tc == "0" || tc == "00:00:00:00") return false;
                    // Must contain at least one colon to be a valid HH:MM:SS:FF timecode
                    return tc.find(':') != std::string::npos;
                };

                if (cached_meta->adobe_meta->HasAnyTimecode()) {
                    // Get the first available timecode as our reference
                    // Use isValidTimecode() to reject placeholder values like "0 s"
                    if (isValidTimecode(cached_meta->adobe_meta->qt_start_timecode)) {
                        cached_start_timecode = cached_meta->adobe_meta->qt_start_timecode;
                        timecode_state = AVAILABLE;
                        Debug::Log("SUCCESS: Found QT StartTimecode in cache: " + cached_start_timecode);
                    }
                    else if (isValidTimecode(cached_meta->adobe_meta->qt_timecode)) {
                        cached_start_timecode = cached_meta->adobe_meta->qt_timecode;
                        timecode_state = AVAILABLE;
                        Debug::Log("SUCCESS: Found QT TimeCode in cache: " + cached_start_timecode);
                    }
                    else if (isValidTimecode(cached_meta->adobe_meta->xmp_alt_timecode_time_value)) {
                        cached_start_timecode = cached_meta->adobe_meta->xmp_alt_timecode_time_value;
                        timecode_state = AVAILABLE;
                        Debug::Log("SUCCESS: Found XMP AltTimecodeTimeValue in cache: " + cached_start_timecode);
                    }
                    else if (isValidTimecode(cached_meta->adobe_meta->xmp_alt_timecode)) {
                        cached_start_timecode = cached_meta->adobe_meta->xmp_alt_timecode;
                        timecode_state = AVAILABLE;
                        Debug::Log("SUCCESS: Found XMP AltTimecode in cache: " + cached_start_timecode);
                    }
                    else if (isValidTimecode(cached_meta->adobe_meta->mxf_start_timecode)) {
                        cached_start_timecode = cached_meta->adobe_meta->mxf_start_timecode;
                        timecode_state = AVAILABLE;
                        Debug::Log("SUCCESS: Found MXF StartTimecode in cache: " + cached_start_timecode);
                    }
                    else {
                        // All timecode fields are either empty or invalid (like "0 s")
                        Debug::Log("Has timecode fields but all are empty or invalid (e.g., '0 s')");
                        // Don't set NOT_AVAILABLE yet - let FFmpeg fallback try
                    }
                }
                else {
                    Debug::Log("No timecode found in Adobe metadata");
                    // Don't set NOT_AVAILABLE yet - let FFmpeg fallback try
                }
                start_timecode_checked = true;
            }
            else {
                Debug::Log("Adobe metadata exists but is_loaded = FALSE");
                timecode_state = CHECKING;
            }
        }

        // Fallback: Check FFmpeg-extracted timecode (video_meta) if Adobe metadata didn't have it
        // This handles MXF files where exiftool shows "0 s" but FFmpeg finds the real timecode
        if (timecode_state != AVAILABLE && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
            if (cached_meta->video_meta->has_embedded_timecode &&
                !cached_meta->video_meta->timecode_format.empty()) {
                cached_start_timecode = cached_meta->video_meta->timecode_format;
                timecode_state = AVAILABLE;
                start_timecode_checked = true;
                Debug::Log("SUCCESS: Found FFmpeg stream timecode: " + cached_start_timecode);
            }
        }

        // If still not found and no adobe_meta, try FFmpeg one more time
        if (timecode_state != AVAILABLE && !cached_meta->adobe_meta) {
            Debug::Log("Cached metadata exists but adobe_meta is NULL, checking FFmpeg");
            if (cached_meta->video_meta && cached_meta->video_meta->has_embedded_timecode &&
                !cached_meta->video_meta->timecode_format.empty()) {
                cached_start_timecode = cached_meta->video_meta->timecode_format;
                timecode_state = AVAILABLE;
                start_timecode_checked = true;
                Debug::Log("SUCCESS: Found FFmpeg stream timecode (no adobe_meta): " + cached_start_timecode);
            }
        }

        // Final check: If we've checked everything and still no valid timecode, mark as not available
        if (timecode_state != AVAILABLE && start_timecode_checked) {
            // Both metadata sources loaded, neither has valid timecode
            bool adobe_loaded = cached_meta->adobe_meta && cached_meta->adobe_meta->is_loaded;
            bool video_loaded = cached_meta->video_meta && cached_meta->video_meta->is_loaded;
            if (adobe_loaded || video_loaded) {
                timecode_state = NOT_AVAILABLE;
                Debug::Log("No valid timecode found in any source (Adobe or FFmpeg)");
            }
        }
    }
    else {
        Debug::Log("No cached metadata found");
        // No metadata cached yet - trigger extraction
        if (!start_timecode_checked) {
            timecode_state = CHECKING;
            Debug::Log("Triggering metadata extraction for timecode");
            project_manager->ExtractMetadataForClip(current_file_path);
            start_timecode_checked = true; // Prevent re-triggering
        }
        else {
            Debug::Log("Already triggered extraction, still waiting");
            timecode_state = CHECKING;
        }
    }

    Debug::Log("Final timecode_state: " + std::to_string((int)timecode_state));
}

// Format timecode without any offset (always regular playback time)
std::string Application::FormatRegularTimecode(double current_seconds) {
    double fps = video_player ? video_player->GetFrameRate() : 23.976;
    int hours = (int)(current_seconds / 3600);
    int minutes = (int)(fmod(current_seconds, 3600.0) / 60);
    int secs = (int)fmod(current_seconds, 60.0);
    int frames = (int)((current_seconds - (int)current_seconds) * fps);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
    return std::string(buffer);
}

// Format timecode with embedded timecode offset (if available)
std::string Application::FormatOffsetTimecode(double current_seconds) {
    double fps = video_player ? video_player->GetFrameRate() : 23.976;

    if (timecode_state != AVAILABLE || cached_start_timecode.empty()) {
        return "";  // Not available
    }

    double start_offset_seconds = ParseTimecodeToSeconds(cached_start_timecode, fps);
    double absolute_timecode_seconds = start_offset_seconds + current_seconds;

    int hours = (int)(absolute_timecode_seconds / 3600);
    int minutes = (int)(fmod(absolute_timecode_seconds, 3600.0) / 60);
    int secs = (int)fmod(absolute_timecode_seconds, 60.0);
    int frames = (int)((absolute_timecode_seconds - (int)absolute_timecode_seconds) * fps);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
    return std::string(buffer);
}

std::string Application::FormatCurrentTimecodeWithOffset(double current_seconds) {
    // Get actual FPS from video player
    double fps = video_player ? video_player->GetFrameRate() : 23.976;

    if (!timecode_mode_enabled || timecode_state != AVAILABLE || cached_start_timecode.empty()) {
        // Fallback to regular time format
        int hours = (int)(current_seconds / 3600);
        int minutes = (int)(fmod(current_seconds, 3600.0) / 60);
        int secs = (int)fmod(current_seconds, 60.0);
        // Truncate frames (not round) - show current frame, not closest frame
        int frames = (int)((current_seconds - (int)current_seconds) * fps);

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
        return std::string(buffer);
    }

    // Parse the start timecode to get offset using actual FPS
    double start_offset_seconds = ParseTimecodeToSeconds(cached_start_timecode, fps);

    // Add current playback time to start timecode
    double absolute_timecode_seconds = start_offset_seconds + current_seconds;

    // Convert back to timecode format
    int hours = (int)(absolute_timecode_seconds / 3600);
    int minutes = (int)(fmod(absolute_timecode_seconds, 3600.0) / 60);
    int secs = (int)fmod(absolute_timecode_seconds, 60.0);

    // Truncate frames (not round) - show current frame, not closest frame
    int frames = (int)((absolute_timecode_seconds - (int)absolute_timecode_seconds) * fps);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
    return std::string(buffer);
}

void Application::ToggleTimecodeMode() {
    Debug::Log("=== ToggleTimecodeMode() called ===");

    // Always check the current state first
    CheckStartTimecodeAvailability();

    if (timecode_state == AVAILABLE) {
        timecode_mode_enabled = !timecode_mode_enabled;
        Debug::Log("Timecode mode: " + std::string(timecode_mode_enabled ? "ENABLED" : "DISABLED"));
        Debug::Log("Using start timecode: " + cached_start_timecode);
    }
    else if (timecode_state == CHECKING) {
        Debug::Log("Timecode mode: Still checking for timecode...");
        // Could show a brief toast/notification here if you want
    }
    else if (timecode_state == NOT_AVAILABLE) {
        Debug::Log("Timecode mode: No embedded timecode found - button does nothing");
        // Could show a brief toast/notification here if you want
    }
}
