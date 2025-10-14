#include "project_manager.h"
#include "../player/video_player.h"
#include "../player/exr_transcoder.h"
#include "../player/image_loaders.h"
#include <imgui.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <set>
#include <regex>
#include "../utils/debug_utils.h"
#include <nfd.h>
#include <nlohmann/json.hpp>
#include <fstream>

#ifdef _WIN32
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#define ICON_MOVIE                  u8"\uE02C"
#define ICON_AUDIO_TRACK           u8"\uE3A1"
#define ICON_IMAGE                 u8"\uE1A6"
#define ICON_VIDEO_LIBRARY         u8"\uE02C"
#define ICON_PLAY_CIRCLE          u8"\uE1C4"
#define ICON_MOVIE_CREATION       u8"\uE404"
#define ICON_PLAYLIST_ADD         u8"\uE03B"
#define ICON_FOLDER_OPEN          u8"\uE2C8"
#define ICON_CONTENT_COPY         u8"\xE14D"
#define ICON_ARTICLE              u8"\uEF42"

extern ImFont* font_icons;

// EXR settings globals from main.cpp
extern int g_exr_transcode_threads;

// Disk cache settings globals from main.cpp
extern std::string g_custom_cache_path;
extern int g_cache_retention_days;
extern int g_dummy_cache_max_gb;
extern int g_transcode_cache_max_gb;
extern bool g_clear_cache_on_exit;

extern ImFont* font_mono;

ImVec4 GetFallbackYellowColor() {
    return ImVec4(0.65f, 0.55f, 0.15f, 1.0f); // Even darker softer yellow color
}

// External variable from main.cpp
extern bool use_windows_accent_color;

#ifdef _WIN32
ImVec4 GetWindowsAccentColor() {
    // Check if Windows accent color is enabled
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }

    DWORD colorization_color;
    BOOL opaque_blend;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
        float r = ((colorization_color >> 16) & 0xff) / 255.0f;
        float g = ((colorization_color >> 8) & 0xff) / 255.0f;
        float b = (colorization_color & 0xff) / 255.0f;
        return ImVec4(r, g, b, 1.0f);
    }
    return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback blue
}
#else
ImVec4 GetWindowsAccentColor() {
    // Check if Windows accent color is enabled
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }
    return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback for non-Windows
}
#endif

ImVec4 TintColor(const ImVec4& color, float brightness, float saturation = 1.0f) {
    ImVec4 result = color;
    result.x *= brightness;
    result.y *= brightness;
    result.z *= brightness;

    if (saturation < 1.0f) {
        float gray = result.x * 0.299f + result.y * 0.587f + result.z * 0.114f;
        result.x = gray + (result.x - gray) * saturation;
        result.y = gray + (result.y - gray) * saturation;
        result.z = gray + (result.z - gray) * saturation;
    }

    return result;
}

ImVec4 MutedLight(const ImVec4& accent) { return TintColor(accent, 1.5f, 0.8f); }
ImVec4 Bright(const ImVec4& accent) { return TintColor(accent, 2.2f, 0.5f); }

namespace ump {

    // ============================================================================
    // CONSTRUCTION / DESTRUCTION
    // ============================================================================

    ProjectManager::ProjectManager(VideoPlayer* player, std::string* current_file, bool* inspector_panel_flag)
        : video_player(player), current_file_path(current_file), show_inspector_panel(inspector_panel_flag) {

        // Initialize video cache manager
        video_cache_manager = std::make_unique<VideoCache>();

        CreateNewBin("Videos");
        CreateNewBin("Audio");
        CreateNewBin("Images");
        CreateNewBin("Sequences");

        CreateNewSequence("Main Playlist");

        if (video_player) {
            video_player->SetPlaylistPositionCallback([this]() {
                this->SyncPlaylistPosition();
                });
        }

        StartAdobeWorkerThread();
        StartVideoMetadataWorkerThread();
    }

    ProjectManager::~ProjectManager() {
        StopVideoMetadataWorkerThread();
        StopAdobeWorkerThread();
    }

    // ============================================================================
    // PROJECT MANAGEMENT
    // ============================================================================

    void ProjectManager::CreateNewProject(const std::string& name, const std::string& path) {
        bins.clear();
        media_pool.clear();
        sequences.clear();

        current_project_path = path + "/" + name + ".uproj";

        CreateNewBin("Videos");
        CreateNewBin("Audio");
        CreateNewBin("Images");
        CreateNewBin("Sequences");

        CreateNewSequence("Main Playlist");
    }

    void ProjectManager::SaveProject() {
        using json = nlohmann::json;

        // Show save dialog if no project path exists
        std::string save_path = current_project_path;
        if (save_path.empty()) {
            nfdu8char_t* out_path = nullptr;
            nfdfilteritem_t filter[1] = { { "Union Player Project", "umproj" } };
            nfdresult_t result = NFD_SaveDialogU8(&out_path, filter, 1, nullptr, "project.umproj");

            if (result != NFD_OKAY) {
                if (result == NFD_ERROR) {
                    Debug::Log("SaveProject: File dialog error");
                }
                return;
            }

            save_path = out_path;
            NFD_FreePathU8(out_path);

            // Ensure .umproj extension
            if (save_path.find(".umproj") == std::string::npos) {
                save_path += ".umproj";
            }
        }

        try {
            json project_data;

            // Project metadata
            project_data["version"] = "1.0";
            project_data["project_name"] = GetProjectName(save_path);

            // Serialize bins
            json bins_array = json::array();
            for (const auto& bin : bins) {
                json bin_obj;
                bin_obj["name"] = bin.name;
                bin_obj["is_open"] = bin.is_open;

                json bin_items = json::array();
                for (const auto& item : bin.items) {
                    bin_items.push_back(item.id);  // Store only IDs, full items in media_pool
                }
                bin_obj["items"] = bin_items;
                bins_array.push_back(bin_obj);
            }
            project_data["bins"] = bins_array;

            // Serialize media_pool
            json media_pool_array = json::array();
            for (const auto& item : media_pool) {
                json item_obj;
                item_obj["id"] = item.id;
                item_obj["name"] = item.name;
                item_obj["path"] = item.path;
                item_obj["type"] = static_cast<int>(item.type);
                item_obj["duration"] = item.duration;
                item_obj["sequence_id"] = item.sequence_id;
                item_obj["clip_count"] = item.clip_count;
                item_obj["is_active"] = item.is_active;

                // Image sequence fields
                item_obj["sequence_pattern"] = item.sequence_pattern;
                item_obj["ffmpeg_pattern"] = item.ffmpeg_pattern;
                item_obj["frame_count"] = item.frame_count;
                item_obj["start_frame"] = item.start_frame;
                item_obj["end_frame"] = item.end_frame;
                item_obj["frame_rate"] = item.frame_rate;

                // EXR fields
                item_obj["exr_layer"] = item.exr_layer;
                item_obj["exr_layer_display"] = item.exr_layer_display;

                media_pool_array.push_back(item_obj);
            }
            project_data["media_pool"] = media_pool_array;

            // Serialize sequences
            json sequences_array = json::array();
            for (const auto& seq : sequences) {
                json seq_obj;
                seq_obj["id"] = seq.id;
                seq_obj["name"] = seq.name;
                seq_obj["base_name"] = seq.base_name;
                seq_obj["frame_rate"] = seq.frame_rate;
                seq_obj["duration"] = seq.duration;

                json clips_array = json::array();
                for (const auto& clip : seq.clips) {
                    json clip_obj;
                    clip_obj["id"] = clip.id;
                    clip_obj["media_id"] = clip.media_id;
                    clip_obj["name"] = clip.name;
                    clip_obj["file_path"] = clip.file_path;
                    clip_obj["start_time"] = clip.start_time;
                    clip_obj["duration"] = clip.duration;
                    clip_obj["source_in"] = clip.source_in;
                    clip_obj["source_out"] = clip.source_out;
                    clip_obj["track_type"] = clip.track_type;
                    clips_array.push_back(clip_obj);
                }
                seq_obj["clips"] = clips_array;
                sequences_array.push_back(seq_obj);
            }
            project_data["sequences"] = sequences_array;
            project_data["current_sequence_id"] = current_sequence_id;

            // Write to file
            std::ofstream file(save_path);
            if (!file.is_open()) {
                Debug::Log("SaveProject: Failed to open file for writing: " + save_path);
                return;
            }

            file << project_data.dump(2);  // Pretty print with 2-space indent
            file.close();

            current_project_path = save_path;
            Debug::Log("SaveProject: Project saved successfully to " + save_path);

        } catch (const std::exception& e) {
            Debug::Log("SaveProject: Error - " + std::string(e.what()));
        }
    }

    void ProjectManager::LoadProject(const std::string& file_path) {
        using json = nlohmann::json;

        // Pause playback before loading project (non-blocking)
        if (video_player && video_player->IsPlaying()) {
            video_player->Pause();
            Debug::Log("LoadProject: Paused playback before loading project");
        }

        // Show open dialog if no path provided
        std::string load_path = file_path;
        if (load_path.empty()) {
            nfdu8char_t* out_path = nullptr;
            nfdfilteritem_t filter[1] = { { "Union Player Project", "umproj" } };
            nfdresult_t result = NFD_OpenDialogU8(&out_path, filter, 1, nullptr);

            if (result != NFD_OKAY) {
                if (result == NFD_ERROR) {
                    Debug::Log("LoadProject: File dialog error");
                }
                return;
            }

            load_path = out_path;
            NFD_FreePathU8(out_path);
        }

        try {
            // Read file
            std::ifstream file(load_path);
            if (!file.is_open()) {
                Debug::Log("LoadProject: Failed to open file: " + load_path);
                return;
            }

            json project_data = json::parse(file);
            file.close();

            // Validate version
            std::string version = project_data.value("version", "");
            if (version != "1.0") {
                Debug::Log("LoadProject: Unsupported project version: " + version);
                return;
            }

            // Clear existing project state
            bins.clear();
            media_pool.clear();
            sequences.clear();
            current_sequence_id.clear();
            selected_media_items.clear();
            selected_playlist_indices.clear();

            // Load media_pool first (needed for bins and sequences)
            if (project_data.contains("media_pool")) {
                for (const auto& item_json : project_data["media_pool"]) {
                    MediaItem item;
                    item.id = item_json.value("id", "");
                    item.name = item_json.value("name", "");
                    item.path = item_json.value("path", "");
                    item.type = static_cast<MediaType>(item_json.value("type", 0));
                    item.duration = item_json.value("duration", 0.0);
                    item.sequence_id = item_json.value("sequence_id", "");
                    item.clip_count = item_json.value("clip_count", 0);
                    item.is_active = item_json.value("is_active", false);

                    // Image sequence fields
                    item.sequence_pattern = item_json.value("sequence_pattern", "");
                    item.ffmpeg_pattern = item_json.value("ffmpeg_pattern", "");
                    item.frame_count = item_json.value("frame_count", 0);
                    item.start_frame = item_json.value("start_frame", 1);
                    item.end_frame = item_json.value("end_frame", 1);
                    item.frame_rate = item_json.value("frame_rate", 24.0);

                    // EXR fields
                    item.exr_layer = item_json.value("exr_layer", "");
                    item.exr_layer_display = item_json.value("exr_layer_display", "");

                    media_pool.push_back(item);
                }
            }

            // Load bins (references media_pool by ID)
            if (project_data.contains("bins")) {
                for (const auto& bin_json : project_data["bins"]) {
                    ProjectBin bin;
                    bin.name = bin_json.value("name", "");
                    bin.is_open = bin_json.value("is_open", true);

                    // Populate bin items from media_pool
                    if (bin_json.contains("items")) {
                        for (const auto& item_id : bin_json["items"]) {
                            std::string id = item_id.get<std::string>();
                            for (const auto& media_item : media_pool) {
                                if (media_item.id == id) {
                                    bin.items.push_back(media_item);
                                    break;
                                }
                            }
                        }
                    }

                    bins.push_back(bin);
                }
            }

            // Load sequences
            if (project_data.contains("sequences")) {
                for (const auto& seq_json : project_data["sequences"]) {
                    Sequence seq;
                    seq.id = seq_json.value("id", "");
                    seq.name = seq_json.value("name", "");
                    seq.base_name = seq_json.value("base_name", "");
                    seq.frame_rate = seq_json.value("frame_rate", 24.0);
                    seq.duration = seq_json.value("duration", 0.0);

                    // Load clips
                    if (seq_json.contains("clips")) {
                        for (const auto& clip_json : seq_json["clips"]) {
                            TimelineClip clip;
                            clip.id = clip_json.value("id", "");
                            clip.media_id = clip_json.value("media_id", "");
                            clip.name = clip_json.value("name", "");
                            clip.file_path = clip_json.value("file_path", "");
                            clip.start_time = clip_json.value("start_time", 0.0);
                            clip.duration = clip_json.value("duration", 0.0);
                            clip.source_in = clip_json.value("source_in", 0.0);
                            clip.source_out = clip_json.value("source_out", 0.0);
                            clip.track_type = clip_json.value("track_type", "");
                            seq.clips.push_back(clip);
                        }
                    }

                    sequences.push_back(seq);
                }
            }

            // Restore current sequence
            current_sequence_id = project_data.value("current_sequence_id", "");

            // Update project path
            current_project_path = load_path;

            Debug::Log("LoadProject: Project loaded successfully from " + load_path);
            Debug::Log("  - " + std::to_string(media_pool.size()) + " media items");
            Debug::Log("  - " + std::to_string(bins.size()) + " bins");
            Debug::Log("  - " + std::to_string(sequences.size()) + " sequences");

            // Update sequence media items in bins
            for (auto& seq : sequences) {
                UpdateSequenceInBin(seq.id);
            }

            // Update ID counter to prevent duplicate IDs when adding new items
            UpdateIDCounter();

            // Load current sequence into player if exists (but don't auto-play)
            if (!current_sequence_id.empty()) {
                Sequence* seq = GetCurrentSequence();
                if (seq && !seq->clips.empty()) {
                    LoadSequenceIntoPlayer(*seq, false);  // false = don't auto-play when loading project

                    // Ensure playback is paused after loading (in case MPV retained play state)
                    if (video_player && video_player->IsPlaying()) {
                        video_player->Pause();
                        Debug::Log("LoadProject: Ensured playback paused after loading sequence");
                    }

                    // Initialize cache and thumbnails for the first clip
                    auto sorted_clips = seq->GetAllClipsSorted();
                    if (!sorted_clips.empty()) {
                        OnVideoLoaded(sorted_clips[0].file_path);
                        Debug::Log("LoadProject: Initialized cache and thumbnails for first clip");
                    }
                }
            }

        } catch (const std::exception& e) {
            Debug::Log("LoadProject: Error - " + std::string(e.what()));
        }
    }

    void ProjectManager::OnVideoLoaded(const std::string& file_path) {
        if (file_path.empty()) return;

        // === NOTIFY MAIN ABOUT VIDEO CHANGE ===
        if (video_change_callback) {
            video_change_callback(file_path);
        }

        // === NOTIFY VIDEO CACHE MANAGER ===
        NotifyVideoChanged(file_path);

        // Extract Metadata in background to avoid playback penalty
        if (video_player && video_player->HasVideo()) {
            // Queue both MPV and Adobe metadata extraction for background processing
            QueueVideoMetadataExtraction(file_path, true);  // High priority for currently playing video
        }

        // Check if this file is already in the project
        for (auto& item : media_pool) {
            if (item.path == file_path) {
                // Update duration now that video is loaded
                if (video_player && video_player->HasVideo()) {
                    double new_duration = video_player->GetDuration();
                    if (new_duration > 0) {
                        item.duration = new_duration;

                        // Also update in bins
                        for (auto& bin : bins) {
                            for (auto& bin_item : bin.items) {
                                if (bin_item.path == file_path) {
                                    bin_item.duration = new_duration;
                                    break;
                                }
                            }
                        }
                    }
                }
                return;
            }
        }

        AddCurrentVideoToProject();
    }

    // ============================================================================
    // UI RENDERING
    // ============================================================================

    void ProjectManager::CreateProjectPanel(bool* show_project_panel) {
        if (!show_project_panel || !*show_project_panel) return;

        if (ImGui::Begin("Project", show_project_panel)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::Text(ICON_VIDEO_LIBRARY);
                ImGui::PopFont();
                ImGui::SameLine();
            }
            ImGui::Text("Project Manager");
            ImGui::PopStyleColor();
            ImGui::Separator();

            CreateProjectToolbar();
            ImGui::Separator();
            CreateProjectInfo();
            ImGui::Separator();
            CreateMediaPool();
        }
        ImGui::End();
    }

    void ProjectManager::CreateProjectToolbar() {
        bool should_disable = false;  // Enable project buttons now that save/load is implemented

        // Style buttons with more padding and stretch to fill width
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 8.0f));  // Vertical padding

        // Calculate button width (divide available width by 3 buttons)
        float available_width = ImGui::GetContentRegionAvail().x;
        float button_width = (available_width - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

        ImGui::BeginDisabled(should_disable);

        if (ImGui::Button("New Project", ImVec2(button_width, 0))) {
            // Fresh start - clear everything
            bins.clear();
            media_pool.clear();
            sequences.clear();
            current_sequence_id.clear();
            selected_media_items.clear();
            selected_playlist_indices.clear();
            current_project_path.clear();

            // Stop playback and clear current media
            if (video_player) {
                video_player->Stop();
            }
            if (current_file_path) {
                current_file_path->clear();
            }

            // Recreate default bins
            CreateNewBin("Videos");
            CreateNewBin("Audio");
            CreateNewBin("Images");
            CreateNewBin("Sequences");

            // Create default sequence
            CreateNewSequence("Main Playlist");

            Debug::Log("New Project: Fresh start - all project data cleared and media stopped");
        }
        ImGui::SameLine();

        if (ImGui::Button("Open Project", ImVec2(button_width, 0))) {
            LoadProject();
        }
        ImGui::SameLine();

        if (ImGui::Button("Save Project", ImVec2(button_width, 0))) {
            SaveProject();
        }

        ImGui::EndDisabled();
        ImGui::PopStyleVar();  // Pop FramePadding

        // "Load Media" button removed - use drag & drop for adding files
    }

    void ProjectManager::CreateProjectInfo() {
        if (current_project_path.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No project loaded");
            return;
        }

        std::string project_name = GetProjectName(current_project_path);
        ImGui::Text("Project: %s", project_name.c_str());
        ImGui::SameLine();
        if (font_mono) ImGui::PushFont(font_mono);
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%zu items)", media_pool.size());
        if (font_mono) ImGui::PopFont();
    }

    void ProjectManager::CreateMediaPool() {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 28, 28, 255));

        if (ImGui::BeginChild("MediaPool", ImVec2(0, 0), true)) {
            for (auto& bin : bins) {
                CreateBinUI(bin);
                if (bin.name == "Sequences" && bin.is_open) {
                    CreateSequencesBinToolbar();
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void ProjectManager::CreateBinUI(ProjectBin& bin) {
        std::string tree_id = "bin_" + bin.name + "##" + std::to_string(reinterpret_cast<uintptr_t>(&bin));

        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (bin.is_open) {
            node_flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.4f, 0.5f, 0.4f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.4f, 0.5f, 0.6f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.5f, 0.6f, 0.7f, 0.8f));

        bool node_open = ImGui::TreeNodeEx(tree_id.c_str(), node_flags);
        ImGui::PopStyleColor(3);

        if (node_open) {
            for (const auto& item : bin.items) {
                CreateMediaItemUI(item);
            }
            ImGui::TreePop();
        }

        bin.is_open = node_open;
    }

    void ProjectManager::CreateSequencesBinToolbar() {
        ImGui::Indent();
        if (ImGui::Button("+ New Playlist##sequences_toolbar")) {
            show_new_sequence_dialog = true;
        }
        ImGui::Unindent();
    }

    void ProjectManager::CreateMediaItemUI(const MediaItem& item) {
        ImGui::PushID(item.id.c_str());

        bool is_selected = IsItemSelected(item.id);
        std::string display_name = item.name;
        ImVec4 text_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

        if (item.type == MediaType::SEQUENCE) {
            if (item.clip_count == 0) {
                display_name += " (empty)";
                text_color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            }
            else {
                display_name += " (" + std::to_string(item.clip_count) + " clips)";
                text_color = Bright(GetWindowsAccentColor());  // Use bright accent color for playlists
            }

            if (item.is_active) {
                display_name += " [ACTIVE]";
                text_color = Bright(GetWindowsAccentColor());
            }
        }
        else {
            if (current_file_path && !current_file_path->empty() && item.path == *current_file_path) {
                display_name += " [ACTIVE]";
                text_color = Bright(GetWindowsAccentColor());
            }
        }

        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        }

        if (font_mono) ImGui::PushFont(font_mono);
        ImGui::PushStyleColor(ImGuiCol_Text, text_color);
        bool clicked = ImGui::Selectable(display_name.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopStyleColor();
        if (font_mono) ImGui::PopFont();

        if (is_selected) {
            ImGui::PopStyleColor(3);
        }

        // Handle click events
        if (clicked) {
            HandleMediaItemClick(item);
        }

        // Handle right-click context menu
        HandleMediaItemRightClick(item);

        // Handle drag & drop
        HandleMediaItemDragDrop(item, is_selected);

        // Show duration info for media files
        if (item.type != MediaType::SEQUENCE && item.duration > 0) {
            ImGui::SameLine();
            std::string type_str;
            switch (item.type) {
            case MediaType::VIDEO: type_str = "video"; break;
            case MediaType::AUDIO: type_str = "audio"; break;
            case MediaType::IMAGE: type_str = "image"; break;
            case MediaType::IMAGE_SEQUENCE:
                type_str = "sequence [" + std::to_string(item.frame_count) + " frames @ " + std::to_string(static_cast<int>(item.frame_rate)) + "fps]";
                break;
            case MediaType::EXR_SEQUENCE:
                type_str = "EXR sequence [" + std::to_string(item.frame_count) + " frames @ " + std::to_string(static_cast<int>(item.frame_rate)) + "fps]";
                if (!item.exr_layer_display.empty()) {
                    type_str += " - " + item.exr_layer_display;
                }
                break;
            default: type_str = "unknown"; break;
            }

            if (font_mono) ImGui::PushFont(font_mono);
            if (item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) {
                ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "[%s] %.2fs", type_str.c_str(), item.duration);
            } else {
                ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "[%s] %.2fs", type_str.c_str(), item.duration);
            }
            if (font_mono) ImGui::PopFont();
        }

        ImGui::PopID();
    }

    void ProjectManager::HandleProjectDialogs() {
        // New Project Dialog
        if (show_new_project_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("New Project");
            show_new_project_dialog = false;
        }

        if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char project_name[256] = "Untitled Project";
            static char project_path[512] = "";

            ImGui::Text("Create New Project");
            ImGui::Separator();
            ImGui::Text("Project Name:");
            ImGui::InputText("##ProjectNameInput", project_name, sizeof(project_name));
            ImGui::Text("Location:");
            ImGui::InputText("##ProjectPathInput", project_path, sizeof(project_path));
            ImGui::Separator();

            if (ImGui::Button("Create##NewProjectDialog")) {
                CreateNewProject(project_name, project_path);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##NewProjectDialog")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // New Sequence Dialog
        if (show_new_sequence_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("New Playlist");
            show_new_sequence_dialog = false;
        }

        if (ImGui::BeginPopupModal("New Playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char sequence_name[256] = "";
            ImGui::Text("Create New Playlist");
            ImGui::Separator();
            ImGui::Text("Playlist Name:");
            ImGui::InputText("##SequenceNameInput", sequence_name, sizeof(sequence_name));
            ImGui::Separator();

            if (ImGui::Button("Create##NewSequenceDialog")) {
                CreateNewSequence(sequence_name);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##NewSequenceDialog")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Rename Dialog
        if (show_rename_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("Rename Item");
            show_rename_dialog = false;
        }

        if (ImGui::BeginPopupModal("Rename Item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter new name:");
            bool enter_pressed = ImGui::InputText("##RenameInput", rename_buffer, sizeof(rename_buffer), ImGuiInputTextFlags_EnterReturnsTrue);

            if (ImGui::Button("OK") || enter_pressed) {
                ProcessRenameItem();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Create Playlist Dialog
        if (show_create_playlist_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("Create Playlist from Selection");
            show_create_playlist_dialog = false;
        }

        if (ImGui::BeginPopupModal("Create Playlist from Selection", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Create new playlist from %d selected items", (int)pending_playlist_items.size());
            ImGui::Separator();
            ImGui::Text("Playlist Name:");
            bool enter_pressed = ImGui::InputText("##PlaylistNameInput", new_playlist_name_buffer, sizeof(new_playlist_name_buffer), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Separator();

            if (ImGui::Button("Create") || enter_pressed) {
                ProcessCreatePlaylistFromSelection();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                pending_playlist_items.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Frame Rate Selection Dialog for Image Sequences
        if (show_frame_rate_dialog && !frame_rate_dialog_opened) {
            Debug::Log("Opening frame rate popup");
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("Select Frame Rate");
            frame_rate_dialog_opened = true;
        }

        if (ImGui::BeginPopupModal("Select Frame Rate", &show_frame_rate_dialog, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Image sequence detected:");

            // Safety check for valid path
            if (font_mono) ImGui::PushFont(font_mono);
            if (!pending_sequence_path.empty()) {
                try {
                    ImGui::Text("%s", std::filesystem::path(pending_sequence_path).filename().string().c_str());
                } catch (const std::exception&) {
                    ImGui::Text("Invalid sequence path");
                }
            } else {
                ImGui::Text("No sequence selected");
            }
            if (font_mono) ImGui::PopFont();
            ImGui::Separator();

            // EXR Layer Selection (if applicable)
            // Thread-safe access to layer data
            std::vector<std::string> layer_names_copy;
            std::vector<std::string> layer_display_names_copy;
            int current_layer_index = 0;
            {
                std::lock_guard<std::mutex> lock(exr_layers_mutex);
                layer_names_copy = exr_layer_names;
                layer_display_names_copy = exr_layer_display_names;
                current_layer_index = selected_exr_layer_index;
            }

            if (is_exr_sequence && !layer_display_names_copy.empty()) {
                ImGui::Text("Select EXR Layer:");

                // Bounds check
                if (current_layer_index >= layer_display_names_copy.size()) {
                    current_layer_index = 0;
                }

                // Show layer selection combo box
                if (ImGui::BeginCombo("##exr_layer", layer_display_names_copy[current_layer_index].c_str())) {
                    for (int i = 0; i < layer_display_names_copy.size(); i++) {
                        bool is_selected = (current_layer_index == i);
                        if (ImGui::Selectable(layer_display_names_copy[i].c_str(), is_selected)) {
                            std::lock_guard<std::mutex> lock(exr_layers_mutex);
                            selected_exr_layer_index = i;
                            current_layer_index = i;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (font_mono) ImGui::PushFont(font_mono);
                if (current_layer_index < layer_names_copy.size()) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Layer: %s", layer_names_copy[current_layer_index].c_str());
                }
                if (font_mono) ImGui::PopFont();

                // Show hidden Cryptomatte layers feedback
                if (hidden_cryptomatte_count > 0) {
                    if (font_mono) ImGui::PushFont(font_mono);
                    ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "%d Cryptomatte layer%s hidden",
                        hidden_cryptomatte_count,
                        hidden_cryptomatte_count == 1 ? "" : "s");
                    if (font_mono) ImGui::PopFont();
                }

                ImGui::Separator();
            }

            // Transcode Options (for both EXR and TIFF/PNG sequences)
            if (is_exr_sequence || is_tiff_png_sequence) {
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "Performance Optimization:");

                std::string transcode_label = is_exr_sequence ?
                    "Transcode EXR (optimize for playback)" :
                    "Transcode to EXR (optimize for playback)";

                if (ImGui::Checkbox(transcode_label.c_str(), &exr_transcode_enabled)) {
                    // Reset to defaults when enabling
                    if (exr_transcode_enabled) {
                        exr_transcode_max_width = 1920;  // 1920 default
                        exr_transcode_compression = 7;   // B44A (lossy, 32:1 ratio, fast)
                    }
                }

                if (exr_transcode_enabled) {
                    ImGui::Indent();

                    std::string transcode_help = is_exr_sequence ?
                        "Create optimized single-layer copy for smooth playback" :
                        "Convert to EXR with B44A compression for smooth playback";

                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", transcode_help.c_str());

                    // Max Width dropdown
                    const char* width_options[] = { "Native", "1920", "2560", "3840", "Custom" };
                    int width_index = (exr_transcode_max_width == 0) ? 0 :
                                     (exr_transcode_max_width == 1920) ? 1 :
                                     (exr_transcode_max_width == 2560) ? 2 :
                                     (exr_transcode_max_width == 3840) ? 3 : 4;

                    if (ImGui::Combo("Max Width", &width_index, width_options, IM_ARRAYSIZE(width_options))) {
                        switch (width_index) {
                            case 0: exr_transcode_max_width = 0; break;     // Native
                            case 1: exr_transcode_max_width = 1920; break;
                            case 2: exr_transcode_max_width = 2560; break;
                            case 3: exr_transcode_max_width = 3840; break;
                            case 4: exr_transcode_max_width = 1920; break;  // Custom - default to 1920
                        }
                    }

                    // Custom width input
                    if (width_index == 4) {
                        ImGui::InputInt("Custom Width", &exr_transcode_max_width, 64, 256);
                        if (exr_transcode_max_width < 64) exr_transcode_max_width = 64;
                        if (exr_transcode_max_width > 16384) exr_transcode_max_width = 16384;
                    }

                    // Compression dropdown
                    const char* compression_options[] = { "B44A (Lossy, 32:1, Fast)", "B44 (Lossy, 44:1)", "DWAA (Lossy)", "DWAB (Lossy)", "PIZ (Lossless)", "ZIP (Lossless)" };
                    int compression_index = (exr_transcode_compression == 7) ? 0 :  // B44A (default)
                                           (exr_transcode_compression == 6) ? 1 :  // B44
                                           (exr_transcode_compression == 8) ? 2 :  // DWAA
                                           (exr_transcode_compression == 9) ? 3 :  // DWAB
                                           (exr_transcode_compression == 4) ? 4 :  // PIZ
                                           (exr_transcode_compression == 3) ? 5 : 0;  // ZIP

                    if (ImGui::Combo("Compression", &compression_index, compression_options, IM_ARRAYSIZE(compression_options))) {
                        switch (compression_index) {
                            case 0: exr_transcode_compression = 7; break;  // B44A
                            case 1: exr_transcode_compression = 6; break;  // B44
                            case 2: exr_transcode_compression = 8; break;  // DWAA
                            case 3: exr_transcode_compression = 9; break;  // DWAB
                            case 4: exr_transcode_compression = 4; break;  // PIZ
                            case 5: exr_transcode_compression = 3; break;  // ZIP
                        }
                    }

                    ImGui::Unindent();
                }

                ImGui::Separator();
            }

            ImGui::Text("Please select a frame rate:");

            // Common frame rate buttons in a simple grid layout
            if (ImGui::Button("23.976")) selected_frame_rate = 23.976;
            ImGui::SameLine();
            if (ImGui::Button("24")) selected_frame_rate = 24.0;
            ImGui::SameLine();
            if (ImGui::Button("25")) selected_frame_rate = 25.0;
            ImGui::SameLine();
            if (ImGui::Button("29.97")) selected_frame_rate = 29.97;

            if (ImGui::Button("30")) selected_frame_rate = 30.0;
            ImGui::SameLine();
            if (ImGui::Button("50")) selected_frame_rate = 50.0;
            ImGui::SameLine();
            if (ImGui::Button("59.94")) selected_frame_rate = 59.94;
            ImGui::SameLine();
            if (ImGui::Button("60")) selected_frame_rate = 60.0;

            ImGui::Separator();
            ImGui::Text("Custom frame rate:");
            ImGui::InputDouble("##fps", &selected_frame_rate, 0.1, 1.0, "%.3f");

            ImGui::Separator();
            if (ImGui::Button("OK")) {
                Debug::Log("OK button pressed");
                if (!pending_sequence_path.empty() && selected_frame_rate > 0.0) {
                    Debug::Log("Processing sequence...");

                    // Pass selected EXR layer if applicable
                    std::string selected_layer = "";
                    if (is_exr_sequence) {
                        std::lock_guard<std::mutex> lock(exr_layers_mutex);
                        if (selected_exr_layer_index < exr_layer_names.size()) {
                            selected_layer = exr_layer_names[selected_exr_layer_index];
                        }
                    }

                    // Check if transcode is requested (for both EXR and TIFF/PNG)
                    if ((is_exr_sequence || is_tiff_png_sequence) && exr_transcode_enabled) {
                        ProcessImageSequenceWithTranscode(pending_sequence_path, selected_frame_rate,
                                                         selected_layer, exr_transcode_max_width,
                                                         exr_transcode_compression);
                    } else {
                        ProcessImageSequence(pending_sequence_path, selected_frame_rate, selected_layer);
                    }

                    Debug::Log("ProcessImageSequence completed");
                }
                Debug::Log("Closing frame rate dialog");
                show_frame_rate_dialog = false;
                frame_rate_dialog_opened = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                show_frame_rate_dialog = false;
                frame_rate_dialog_opened = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        } else if (frame_rate_dialog_opened) {
            // Dialog was closed by X button or Escape
            Debug::Log("Frame rate dialog closed by user");
            show_frame_rate_dialog = false;
            frame_rate_dialog_opened = false;
        }
    }

    void ProjectManager::CreatePropertiesSection() {
        if (!video_player || current_file_path->empty()) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No video loaded");
            return;
        }

        // Check if this is a single image file to prevent metadata spam
        bool is_single_image = false;
        if (!current_file_path->empty() && current_file_path->find("mf://") != 0) {
            // Not an MF:// sequence, check if it's a single image
            MediaType media_type = GetMediaType(*current_file_path);
            if (media_type == MediaType::IMAGE) {
                // This is a single image - don't spam metadata extraction
                is_single_image = true;
            }
        }

        if (is_single_image) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Single image loaded");
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Image properties not available for single files");
            return;
        }

        // Check if this is an image sequence or EXR sequence
        bool is_image_sequence = current_file_path->substr(0, 5) == "mf://";
        bool is_exr_sequence = current_file_path->substr(0, 6) == "exr://";

        if (is_image_sequence || is_exr_sequence) {
            // Show basic sequence info from video player
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), is_exr_sequence ? "EXR sequence loaded" : "Image sequence loaded");

            // Display basic properties from video player
            if (video_player && video_player->HasVideo()) {
                ImGui::Spacing();

                if (ImGui::CollapsingHeader("Sequence Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Determine image type from file path
                    std::string image_type = "Unknown";
                    if (is_exr_sequence) {
                        image_type = "EXR";
                    } else if (!current_file_path->empty()) {
                        // Extract from mf:// URL or from first file
                        std::string path_to_check = *current_file_path;
                        if (path_to_check.substr(0, 5) == "mf://") {
                            path_to_check = path_to_check.substr(5); // Remove mf:// prefix
                        }
                        size_t dot_pos = path_to_check.find_last_of('.');
                        if (dot_pos != std::string::npos) {
                            std::string ext = path_to_check.substr(dot_pos + 1);
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
                            image_type = ext;
                        }
                    }

                    ImGui::Columns(2, "SequenceProps", false);
                    ImGui::SetColumnWidth(0, 120);

                    // Use monospace font for values
                    extern ImFont* font_mono;

                    // Image type
                    ImGui::Text("Image Type:");
                    ImGui::NextColumn();
                    if (font_mono) ImGui::PushFont(font_mono);
                    ImGui::Text("%s", image_type.c_str());
                    if (font_mono) ImGui::PopFont();
                    ImGui::NextColumn();

                    // Dimensions
                    ImGui::Text("Resolution:");
                    ImGui::NextColumn();
                    if (font_mono) ImGui::PushFont(font_mono);
                    ImGui::Text("%d x %d", video_player->GetVideoWidth(), video_player->GetVideoHeight());
                    if (font_mono) ImGui::PopFont();
                    ImGui::NextColumn();

                    // Duration
                    ImGui::Text("Duration:");
                    ImGui::NextColumn();
                    double duration = video_player->GetDuration();
                    if (font_mono) ImGui::PushFont(font_mono);
                    if (duration > 0) {
                        ImGui::Text("%.2f seconds", duration);
                    } else {
                        ImGui::Text("Unknown");
                    }
                    if (font_mono) ImGui::PopFont();
                    ImGui::NextColumn();

                    // Frame rate
                    ImGui::Text("Frame Rate:");
                    ImGui::NextColumn();
                    double fps = video_player->GetFrameRate();
                    if (font_mono) ImGui::PushFont(font_mono);
                    if (fps > 0) {
                        ImGui::Text("%.3f fps", fps);
                    } else {
                        ImGui::Text("Unknown");
                    }
                    if (font_mono) ImGui::PopFont();
                    ImGui::NextColumn();

                    // Frame range (replace Total Frames)
                    ImGui::Text("Frame Range:");
                    ImGui::NextColumn();
                    int total_frames = video_player->GetTotalFrames();
                    int start_frame = video_player->GetImageSequenceStartFrame();
                    if (font_mono) ImGui::PushFont(font_mono);
                    if (total_frames > 0) {
                        int end_frame = start_frame + total_frames - 1;
                        ImGui::Text("%d-%d", start_frame, end_frame);
                    } else {
                        ImGui::Text("Unknown");
                    }
                    if (font_mono) ImGui::PopFont();
                    ImGui::NextColumn();

                    ImGui::Columns(1);
                }
            } else {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Sequence properties will be available when loaded");
            }
            return;
        }

        // For video/audio files, quietly queue metadata extraction in background
        const CombinedMetadata* cached_meta = GetCachedMetadata(*current_file_path);
        if (!cached_meta) {
            QueueVideoMetadataExtraction(*current_file_path, true);
            // Don't show loading messages - just return and let it load in background
            return;
        }

        // Show available metadata (no loading states or progress bars)
        if (cached_meta->video_meta) {
            DisplayVideoMetadata(cached_meta->video_meta.get());
        }

        if (cached_meta->adobe_meta) {
            ImGui::Spacing();
            DisplayAdobeMetadata(cached_meta->adobe_meta.get());

            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Timecode Information", ImGuiTreeNodeFlags_DefaultOpen)) {
                DisplayTimecodeTable(cached_meta->adobe_meta.get());
            }
        }
    }

    // ============================================================================
    // MEDIA ITEM INTERACTION HANDLERS
    // ============================================================================

    void ProjectManager::HandleMediaItemClick(const MediaItem& item) {
        bool ctrl_held = ImGui::GetIO().KeyCtrl;
        bool shift_held = ImGui::GetIO().KeyShift;

        // Single click - handle selection
        SelectMediaItem(item.id, ctrl_held, shift_held);

        // Double-click - load the item
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (item.type == MediaType::SEQUENCE) {
                LoadSequenceFromBin(item.id);
            }
            else {
                LoadSingleMediaItem(item);
            }
        }
    }

    void ProjectManager::HandleMediaItemRightClick(const MediaItem& item) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            std::string popup_id = "context_" + item.id;
            ImGui::OpenPopup(popup_id.c_str());

            if (!IsItemSelected(item.id)) {
                ClearSelection();
                SelectMediaItem(item.id, false, false);
            }
        }

        // Context menu popup
        std::string popup_id = "context_" + item.id;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
        if (ImGui::BeginPopup(popup_id.c_str())) {
            ShowMediaItemContextMenu(item);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
    }

    void ProjectManager::HandleMediaItemDragDrop(const MediaItem& item, bool is_selected) {
        if (ImGui::BeginDragDropSource()) {
            if (is_selected && selected_media_items.size() > 1) {
                std::string payload_data;
                for (const auto& selected_id : selected_media_items) {
                    if (!payload_data.empty()) payload_data += ";";
                    payload_data += selected_id;
                }
                ImGui::SetDragDropPayload("MEDIA_ITEMS_MULTI", payload_data.c_str(), payload_data.size() + 1);
                ImGui::Text("Dragging %zu items", selected_media_items.size());
            }
            else {
                ImGui::SetDragDropPayload("MEDIA_ITEM", item.id.c_str(), item.id.size() + 1);
                ImGui::Text("Dragging: %s", item.name.c_str());
            }
            ImGui::EndDragDropSource();
        }
    }

    void ProjectManager::ShowMediaItemContextMenu(const MediaItem& item) {
        int selection_count = static_cast<int>(selected_media_items.size());

        if (selection_count <= 1) {
            ImGui::Text("Options for: %s", item.name.c_str());
        }
        else {
            ImGui::Text("%d items selected", selection_count);
        }
        ImGui::Separator();

        if (ImGui::MenuItem("Delete", "Del")) {
            DeleteSelectedItems();
            ImGui::CloseCurrentPopup();
        }

        if (selection_count == 1 && item.type == MediaType::SEQUENCE && ImGui::MenuItem("Rename", "F2")) {
            StartRenaming(item.id);
            ImGui::CloseCurrentPopup();
        }

        if (selection_count == 1 && item.type != MediaType::SEQUENCE && ImGui::MenuItem("Show in Explorer")) {
            ShowInExplorer(item.path);
            ImGui::CloseCurrentPopup();
        }

        if (selection_count > 1) {
            ImGui::Separator();
            if (ImGui::MenuItem("Create Playlist from Selection")) {
                CreatePlaylistFromSelection();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Clear Selection")) {
                ClearSelection();
                ImGui::CloseCurrentPopup();
            }
        }
    }

    // ============================================================================
    // MEDIA MANAGEMENT
    // ============================================================================

    // LoadMediaFiles() removed - use drag & drop instead for adding multiple files

    void ProjectManager::AddMediaFileToProject(const std::string& file_path) {
        MediaItem item;
        item.id = GenerateUniqueID();
        item.name = GetFileName(file_path);
        item.path = file_path;
        item.type = GetMediaType(file_path);

        if (item.type == MediaType::VIDEO || item.type == MediaType::AUDIO) {
            double probed_duration = video_player->ProbeFileDuration(file_path);
            if (probed_duration > 0) {
                item.duration = probed_duration;
            }
            else {
                item.duration = (item.type == MediaType::VIDEO) ? 30.0 : 180.0;
            }
        }
        else {
            item.duration = 1.0;
        }

        media_pool.push_back(item);

        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > bin_index) {
            bins[bin_index].items.push_back(item);
        }
    }

    void ProjectManager::AddCurrentVideoToProject() {
        if (!current_file_path || current_file_path->empty()) return;

        // Check if already in project
        for (const auto& item : media_pool) {
            if (item.path == *current_file_path) {
                return;
            }
        }

        MediaItem item;
        item.id = GenerateUniqueID();
        item.name = GetFileName(*current_file_path);
        item.path = *current_file_path;
        item.type = GetMediaType(*current_file_path);

        Debug::Log("AddCurrentVideoToProject: Adding media to project");
        Debug::Log("  - ID: " + item.id);
        Debug::Log("  - Name: " + item.name);
        Debug::Log("  - Path: " + item.path);
        Debug::Log("  - Type: " + std::to_string(static_cast<int>(item.type)));

        double detected_duration = 0.0;
        if (video_player && video_player->HasVideo()) {
            detected_duration = video_player->GetDuration();
        }

        if (detected_duration > 0) {
            item.duration = detected_duration;
        }
        else {
            item.duration = GetDefaultDurationForType(item.type);
        }

        media_pool.push_back(item);

        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > bin_index) {
            bins[bin_index].items.push_back(item);
        }
    }

    void ProjectManager::LoadSingleMediaItem(const MediaItem& item) {
        Debug::Log("=== LoadSingleMediaItem CALLED ===");
        Debug::Log("MediaItem details:");
        Debug::Log("  - ID: " + item.id);
        Debug::Log("  - Name: " + item.name);
        Debug::Log("  - Path: " + item.path);
        Debug::Log("  - Type: " + std::to_string(static_cast<int>(item.type)));
        Debug::Log("  - Duration: " + std::to_string(item.duration));
        Debug::Log("  - FFmpeg pattern: " + (item.ffmpeg_pattern.empty() ? "(empty)" : item.ffmpeg_pattern));
        Debug::Log("  - Sequence pattern: " + (item.sequence_pattern.empty() ? "(empty)" : item.sequence_pattern));
        Debug::Log("  - EXR layer: " + (item.exr_layer.empty() ? "(empty)" : item.exr_layer));

        ClearSelection();
        current_sequence_id.clear();
        UpdateSequenceActiveStates("");

        // Set frame rate for image sequences before loading
        if ((item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) && item.frame_rate > 0.0) {
            // Set MPV frame rate for playback
            video_player->SetMFFrameRate(item.frame_rate);

            // For regular image sequences, also store start frame for display
            if (item.type == MediaType::IMAGE_SEQUENCE) {
                video_player->SetImageSequenceFrameRate(item.frame_rate, item.start_frame);
            }
        }

        // === Image Sequence Handling ===
        if (item.type == MediaType::IMAGE_SEQUENCE) {
            Debug::Log("LoadSingleMediaItem: Loading image sequence: " + item.path);
            Debug::Log("LoadSingleMediaItem: MediaItem details:");
            Debug::Log("  - ffmpeg_pattern: " + (item.ffmpeg_pattern.empty() ? "(EMPTY)" : item.ffmpeg_pattern));
            Debug::Log("  - sequence_pattern: " + (item.sequence_pattern.empty() ? "(EMPTY)" : item.sequence_pattern));
            Debug::Log("  - frame_rate: " + std::to_string(item.frame_rate));
            Debug::Log("  - start_frame: " + std::to_string(item.start_frame));
            Debug::Log("  - end_frame: " + std::to_string(item.end_frame));
            Debug::Log("  - frame_count: " + std::to_string(item.frame_count));

            // Rebuild the file list using stored pattern
            // item.path is an mf:// URL, so we need to use ffmpeg_pattern to reconstruct the file list
            std::vector<std::string> sequence_files;

            if (!item.ffmpeg_pattern.empty()) {
                // Use ffmpeg_pattern to rebuild file list (e.g., "/path/shot_%04d.exr")
                Debug::Log("LoadSingleMediaItem: Using ffmpeg_pattern: " + item.ffmpeg_pattern);

                // Extract directory and pattern from ffmpeg_pattern
                std::filesystem::path pattern_path(item.ffmpeg_pattern);
                std::string directory = pattern_path.parent_path().string();

                // Build file list using stored frame range
                for (int frame = item.start_frame; frame <= item.end_frame; ++frame) {
                    char frame_str[512];  // Increased buffer size to handle long filenames
                    snprintf(frame_str, sizeof(frame_str), item.sequence_pattern.c_str(), frame);
                    std::string file_path = directory + "/" + frame_str;
                    sequence_files.push_back(file_path);
                }

                Debug::Log("LoadSingleMediaItem: Reconstructed " + std::to_string(sequence_files.size()) + " files from pattern");
                if (!sequence_files.empty()) {
                    Debug::Log("LoadSingleMediaItem: First file: " + sequence_files[0]);
                    Debug::Log("LoadSingleMediaItem: Last file: " + sequence_files[sequence_files.size() - 1]);
                }
            } else {
                // Fallback: Try to parse mf:// URL (should not happen, but just in case)
                Debug::Log("LoadSingleMediaItem: Warning - No ffmpeg_pattern, attempting detection from path");
                if (item.path.substr(0, 5) == "mf://") {
                    // Extract first file path from mf:// URL
                    std::string first_file = item.path.substr(5);
                    size_t fps_pos = first_file.find(":fps=");
                    if (fps_pos != std::string::npos) {
                        first_file = first_file.substr(0, fps_pos);
                    }
                    Debug::Log("LoadSingleMediaItem: Attempting to detect sequence from: " + first_file);
                    sequence_files = DetectImageSequence(first_file);
                    Debug::Log("LoadSingleMediaItem: Detected " + std::to_string(sequence_files.size()) + " files");
                }
            }

            Debug::Log("LoadSingleMediaItem: Checking conditions for loading:");
            Debug::Log("  - sequence_files.empty() = " + std::string(sequence_files.empty() ? "TRUE" : "FALSE"));
            Debug::Log("  - item.frame_rate = " + std::to_string(item.frame_rate));

            if (!sequence_files.empty() && item.frame_rate > 0.0) {
                // Determine pipeline mode based on format
                PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Default
                size_t dot_pos = sequence_files[0].find_last_of('.');
                if (dot_pos != std::string::npos) {
                    std::string ext = sequence_files[0].substr(dot_pos);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".tif" || ext == ".tiff") {
                        pipeline_mode = PipelineMode::HDR_RES;  // TIFF sequences likely 16-bit
                    }
                }

                Debug::Log("LoadSingleMediaItem: Loading " + std::to_string(sequence_files.size()) +
                           " files with pipeline mode: " + std::string(PipelineModeToString(pipeline_mode)));

                // Load through DirectEXRCache with universal loader
                Debug::Log("LoadSingleMediaItem: Calling LoadImageSequenceWithCache()...");
                bool success = video_player->LoadImageSequenceWithCache(sequence_files, item.frame_rate, pipeline_mode);
                Debug::Log("LoadSingleMediaItem: LoadImageSequenceWithCache() returned: " + std::string(success ? "TRUE (success)" : "FALSE (failed)"));

                if (success) {
                    *current_file_path = item.path;
                    Debug::Log("LoadSingleMediaItem: Successfully loaded image sequence - EARLY RETURN");

                    // Notify callbacks
                    if (video_change_callback) {
                        video_change_callback(item.path);
                    }

                    // Select this item and extract metadata
                    SelectMediaItem(item.id, false, false);
                    NotifyVideoChanged(item.path);
                    QueueVideoMetadataExtraction(item.path, true);
                    return;  // Early return - sequence loaded successfully
                } else {
                    Debug::Log("LoadSingleMediaItem: ERROR - Failed to load image sequence, will fall through to LoadFile()");
                }
            } else {
                Debug::Log("LoadSingleMediaItem: ERROR - Could not rebuild file list or frame_rate invalid");
                Debug::Log("LoadSingleMediaItem: Will fall through to LoadFile() fallback");
            }
        }

        Debug::Log("LoadSingleMediaItem: Reached fallback section (item.type = " + std::to_string(static_cast<int>(item.type)) + ")");

        // Special handling for EXR sequences
        if (item.type == MediaType::EXR_SEQUENCE && !item.exr_layer.empty()) {
            Debug::Log("LoadSingleMediaItem: Loading EXR sequence: " + item.path);

            // Parse the exr:// URL to get the original sequence path
            std::string exr_path = item.path;
            if (exr_path.substr(0, 6) == "exr://") {
                size_t query_pos = exr_path.find("?layer=");
                if (query_pos != std::string::npos) {
                    std::string sequence_path = exr_path.substr(6, query_pos - 6); // Remove "exr://" prefix
                    std::string layer_name = item.exr_layer;

                    Debug::Log("LoadSingleMediaItem: Detected sequence path: " + sequence_path);
                    Debug::Log("LoadSingleMediaItem: Detected layer: " + layer_name);

                    // Rebuild the file list from the sequence path
                    std::vector<std::string> sequence_files = DetectImageSequence(sequence_path);
                    if (!sequence_files.empty()) {
                        if (video_player->LoadEXRSequenceWithDummy(sequence_files, layer_name, item.frame_rate)) {
                            *current_file_path = item.path;
                            Debug::Log("LoadSingleMediaItem: Successfully loaded EXR sequence");

                            // Notify callbacks
                            if (video_change_callback) {
                                video_change_callback(item.path);
                            }

                            // Select this item and extract metadata
                            SelectMediaItem(item.id, false, false);
                            NotifyVideoChanged(item.path);
                            QueueVideoMetadataExtraction(item.path, true);
                            return;  // Early return - EXR sequence loaded successfully
                        } else {
                            Debug::Log("LoadSingleMediaItem: ERROR - Failed to load EXR sequence");
                        }
                    }
                }
            }
        }

        // === FALLBACK: Regular video/audio loading ===
        // This handles regular MP4/MOV/etc files, OR fallback if image sequence loading failed
        Debug::Log("LoadSingleMediaItem: Loading as regular file: " + item.path);
        video_player->LoadFile(item.path);
        *current_file_path = item.path;

        // === NOTIFY MAIN ABOUT VIDEO CHANGE ===
        if (video_change_callback) {
            video_change_callback(item.path);
        }

        // === NOTIFY VIDEO CACHE MANAGER ===
        // Skip frame cache for audio files (no video frames to cache)
        if (item.type != MediaType::AUDIO) {
            NotifyVideoChanged(item.path);
        } else {
            Debug::Log("LoadSingleMediaItem: Skipping cache notification for audio file");
        }

        // Select this item in the project panel
        SelectMediaItem(item.id, false, false);

        // Extract metadata in background to avoid playback penalty
        if (video_player && video_player->HasVideo()) {
            QueueVideoMetadataExtraction(item.path, true);  // High priority for loaded media
        }
    }

    MediaItem* ProjectManager::GetMediaItem(const std::string& media_id) {
        auto it = std::find_if(media_pool.begin(), media_pool.end(),
            [&media_id](const MediaItem& item) { return item.id == media_id; });
        return (it != media_pool.end()) ? &(*it) : nullptr;
    }

    // ============================================================================
    // SELECTION MANAGEMENT
    // ============================================================================

    void ProjectManager::SelectMediaItem(const std::string& item_id, bool ctrl_held, bool shift_held) {
        if (!ctrl_held && !shift_held) {
            selected_media_items.clear();
            selected_media_items.insert(item_id);
            last_selected_item = item_id;
        }
        else if (ctrl_held) {
            if (selected_media_items.count(item_id)) {
                selected_media_items.erase(item_id);
            }
            else {
                selected_media_items.insert(item_id);
                last_selected_item = item_id;
            }
        }
        else if (shift_held && !last_selected_item.empty()) {
            SelectItemRange(last_selected_item, item_id);
        }
    }

    void ProjectManager::SelectItemRange(const std::string& start_id, const std::string& end_id) {
        std::vector<MediaItem*> visible_items;
        for (auto& bin : bins) {
            if (bin.is_open) {
                for (auto& item : bin.items) {
                    visible_items.push_back(&item);
                }
            }
        }

        int start_idx = -1, end_idx = -1;
        for (int i = 0; i < visible_items.size(); i++) {
            if (visible_items[i]->id == start_id) start_idx = i;
            if (visible_items[i]->id == end_id) end_idx = i;
        }

        if (start_idx >= 0 && end_idx >= 0) {
            int min_idx = (std::min)(start_idx, end_idx);
            int max_idx = (std::max)(start_idx, end_idx);
            for (int i = min_idx; i <= max_idx; i++) {
                selected_media_items.insert(visible_items[i]->id);
            }
        }
    }

    void ProjectManager::ClearSelection() {
        selected_media_items.clear();
        last_selected_item.clear();
    }

    bool ProjectManager::IsItemSelected(const std::string& item_id) const {
        return selected_media_items.count(item_id) > 0;
    }

    std::vector<MediaItem> ProjectManager::GetSelectedItems() const {
        std::vector<MediaItem> result;
        for (const auto& id : selected_media_items) {
            auto it = std::find_if(media_pool.begin(), media_pool.end(),
                [&id](const MediaItem& item) { return item.id == id; });
            if (it != media_pool.end()) {
                result.push_back(*it);
            }
        }
        return result;
    }

    // ============================================================================
    // ITEM OPERATIONS (DELETE, RENAME, ETC.)
    // ============================================================================

    void ProjectManager::DeleteSelectedItems() {
        std::vector<std::string> items_to_delete(selected_media_items.begin(), selected_media_items.end());
        std::vector<std::string> sequence_ids_to_delete;
        std::vector<std::string> video_paths_to_uncache;

        // Collect sequence IDs and video file paths before deletion
        for (const std::string& item_id : items_to_delete) {
            auto media_item = std::find_if(media_pool.begin(), media_pool.end(),
                [&item_id](const MediaItem& item) { return item.id == item_id; });
            if (media_item != media_pool.end()) {
                if (media_item->type == MediaType::SEQUENCE) {
                    sequence_ids_to_delete.push_back(media_item->sequence_id);
                } else if (media_item->type == MediaType::VIDEO) {
                    // Collect video file paths to remove from cache
                    video_paths_to_uncache.push_back(media_item->path);
                }
            }
        }

        // Delete items from media_pool and bins
        for (const std::string& item_id : items_to_delete) {
            media_pool.erase(
                std::remove_if(media_pool.begin(), media_pool.end(),
                    [&item_id](const MediaItem& item) { return item.id == item_id; }),
                media_pool.end()
            );

            for (auto& bin : bins) {
                bin.items.erase(
                    std::remove_if(bin.items.begin(), bin.items.end(),
                        [&item_id](const MediaItem& item) { return item.id == item_id; }),
                    bin.items.end()
                );
            }
        }

        // Delete sequences
        for (const std::string& sequence_id : sequence_ids_to_delete) {
            sequences.erase(
                std::remove_if(sequences.begin(), sequences.end(),
                    [&sequence_id](const Sequence& seq) { return seq.id == sequence_id; }),
                sequences.end()
            );

            if (current_sequence_id == sequence_id) {
                current_sequence_id.clear();
            }
        }

        // Clean up video caches for deleted video files
        for (const std::string& video_path : video_paths_to_uncache) {
            RemoveVideoFromCache(video_path);
        }

        ClearSelection();
    }

    void ProjectManager::StartRenaming(const std::string& item_id) {
        renaming_item_id = item_id;
        auto item_it = std::find_if(media_pool.begin(), media_pool.end(),
            [&item_id](const MediaItem& item) { return item.id == item_id; });

        if (item_it != media_pool.end()) {
            strncpy_s(rename_buffer, item_it->name.c_str(), sizeof(rename_buffer) - 1);
            show_rename_dialog = true;
        }
    }

    void ProjectManager::ProcessRenameItem() {
        auto item_it = std::find_if(media_pool.begin(), media_pool.end(),
            [this](const MediaItem& item) { return item.id == renaming_item_id; });

        if (item_it != media_pool.end()) {
            item_it->name = rename_buffer;
            // Also update in bins
            for (auto& bin : bins) {
                auto bin_item = std::find_if(bin.items.begin(), bin.items.end(),
                    [this](const MediaItem& item) { return item.id == renaming_item_id; });
                if (bin_item != bin.items.end()) {
                    bin_item->name = rename_buffer;
                }
            }
        }
    }

    void ProjectManager::StartRenamingSelected() {
        if (selected_media_items.size() == 1) {
            StartRenaming(*selected_media_items.begin());
        }
    }

    void ProjectManager::ShowItemProperties(const std::string& item_id) {
        auto item_it = std::find_if(media_pool.begin(), media_pool.end(),
            [&item_id](const MediaItem& item) { return item.id == item_id; });

        if (item_it != media_pool.end()) {
            Debug::Log("Item details:");
            Debug::Log("  Name: " + item_it->name);
            Debug::Log("  Type: " + std::to_string(static_cast<int>(item_it->type)));
            Debug::Log("  Path: " + item_it->path);
            Debug::Log("  Duration: " + std::to_string(item_it->duration));

            // To revisit
        }
    }

    void ProjectManager::CreatePlaylistFromSelection() {
        if (selected_media_items.empty()) return;

        // Filter out sequences and image sequences (EXR/TIFF/PNG/JPEG)
        // Only videos and single images can be added to playlists
        std::vector<MediaItem> media_items_only;
        for (const auto& item_id : selected_media_items) {
            auto media_item = GetMediaItem(item_id);
            if (media_item &&
                media_item->type != MediaType::SEQUENCE &&
                media_item->type != MediaType::EXR_SEQUENCE &&
                media_item->type != MediaType::IMAGE_SEQUENCE) {
                media_items_only.push_back(*media_item);
            }
        }

        if (media_items_only.empty()) return;

        pending_playlist_items = media_items_only;
        show_create_playlist_dialog = true;

        std::string default_name = "Playlist from " + std::to_string(media_items_only.size()) + " items";
        strncpy_s(new_playlist_name_buffer, default_name.c_str(), sizeof(new_playlist_name_buffer) - 1);
    }

    void ProjectManager::ProcessCreatePlaylistFromSelection() {
        std::string playlist_name = new_playlist_name_buffer;
        if (playlist_name.empty()) {
            playlist_name = "New Playlist";
        }

        CreateNewSequence(playlist_name);

        // Find and switch to the newly created playlist
        for (const auto& seq : sequences) {
            if (seq.name == playlist_name) {
                SwitchToSequence(seq.id);
                for (const auto& media_item : pending_playlist_items) {
                    AddToPlaylist(media_item.id);
                }
                break;
            }
        }

        ClearSelection();
        pending_playlist_items.clear();
    }

    void ProjectManager::ShowInExplorer(const std::string& file_path) {
        if (file_path.empty()) return;

        std::string resolved_path = file_path;

        // Handle mf:// URLs (image sequences)
        if (file_path.substr(0, 5) == "mf://") {
            // Extract directory from mf:// URL
            // Format: mf://directory/basename*extension or mf://path/sequence_%04d.exr:fps=24
            std::string path_part = file_path.substr(5); // Remove "mf://"

            // Remove fps parameter if present
            size_t fps_pos = path_part.find(":fps=");
            if (fps_pos != std::string::npos) {
                path_part = path_part.substr(0, fps_pos);
            }

            // Extract directory (everything up to the last /)
            size_t last_slash = path_part.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                resolved_path = path_part.substr(0, last_slash);
                Debug::Log("ShowInExplorer: Resolved mf:// URL to directory: " + resolved_path);
            } else {
                resolved_path = path_part; // Fallback: use whole path
            }
        }
        // Handle exr:// URLs (EXR sequences with layers)
        else if (file_path.substr(0, 6) == "exr://") {
            // Format: exr://path/to/sequence.exr?layer=beauty
            std::string path_part = file_path.substr(6); // Remove "exr://"

            // Remove layer parameter if present
            size_t query_pos = path_part.find('?');
            if (query_pos != std::string::npos) {
                path_part = path_part.substr(0, query_pos);
            }

            // Extract directory for sequence, or use file directly
            size_t last_slash = path_part.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                resolved_path = path_part.substr(0, last_slash);
                Debug::Log("ShowInExplorer: Resolved exr:// URL to directory: " + resolved_path);
            } else {
                resolved_path = path_part;
            }
        }

#ifdef _WIN32
        // Launch explorer in a separate thread to avoid blocking the UI
        std::thread([resolved_path]() {
            std::string windows_path = resolved_path;
            std::replace(windows_path.begin(), windows_path.end(), '/', '\\');

            // Use /select for files, plain path for directories
            std::filesystem::path fs_path(windows_path);
            std::string command;

            if (std::filesystem::is_directory(fs_path)) {
                command = "explorer \"" + windows_path + "\"";
            } else {
                command = "explorer /select,\"" + windows_path + "\"";
            }

            Debug::Log("ShowInExplorer: Executing command: " + command);
            system(command.c_str());
        }).detach();
#endif
    }

    // ============================================================================
    // SEQUENCE MANAGEMENT
    // ============================================================================

    void ProjectManager::CreateNewSequence(const std::string& name) {
        Sequence new_sequence;
        new_sequence.id = GenerateUniqueID();
        new_sequence.name = name.empty() ? ("Sequence " + std::to_string(sequences.size() + 1)) : name;
        new_sequence.frame_rate = 24.0;
        new_sequence.duration = 0.0;

        sequences.push_back(new_sequence);

        if (current_sequence_id.empty()) {
            current_sequence_id = new_sequence.id;
        }

        AddSequenceToProject(new_sequence.id);
    }

    void ProjectManager::AddSequenceToProject(const std::string& sequence_id) {
        auto seq_it = std::find_if(sequences.begin(), sequences.end(),
            [&sequence_id](const Sequence& seq) { return seq.id == sequence_id; });

        if (seq_it != sequences.end()) {
            MediaItem sequence_item = CreateSequenceMediaItem(*seq_it);
            media_pool.push_back(sequence_item);

            if (bins.size() > SEQUENCES_BIN_INDEX) {
                bins[SEQUENCES_BIN_INDEX].items.push_back(sequence_item);
            }
        }
    }

    MediaItem ProjectManager::CreateSequenceMediaItem(const Sequence& sequence) {
        MediaItem item;
        item.id = "seq_item_" + sequence.id;
        item.name = sequence.name;
        item.type = MediaType::SEQUENCE;
        item.sequence_id = sequence.id;
        item.duration = sequence.duration;
        item.clip_count = sequence.clips.size();
        item.is_active = (sequence.id == current_sequence_id);
        return item;
    }

    void ProjectManager::LoadSequenceFromBin(const std::string& media_item_id) {
        auto item_it = std::find_if(media_pool.begin(), media_pool.end(),
            [&media_item_id](const MediaItem& item) { return item.id == media_item_id; });

        if (item_it == media_pool.end() || item_it->type != MediaType::SEQUENCE) {
            return;
        }

        if (show_inspector_panel) {
            *show_inspector_panel = true;
        }

        SwitchToSequence(item_it->sequence_id);

        Sequence* sequence = GetCurrentSequence();
        if (sequence) {
            if (sequence->clips.empty()) {
                video_player->Stop();
                if (current_file_path) {
                    current_file_path->clear();
                }
                UpdateSequenceInBin(sequence->id);
            }
            else {
                LoadSequenceIntoPlayer(*sequence);
            }
        }
    }

    void ProjectManager::LoadSequenceIntoPlayer(const Sequence& sequence, bool auto_play) {
        if (sequence.clips.empty()) return;

        std::string playlist_content;
        auto sorted_clips = sequence.GetAllClipsSorted();

        for (const auto& clip : sorted_clips) {
            playlist_content += clip.file_path + "\n";
        }

        video_player->LoadPlaylist(playlist_content);

        if (!sorted_clips.empty() && current_file_path) {
            *current_file_path = sorted_clips[0].file_path;
        }

        // Only auto-play if requested (user-initiated, not project loading)
        if (auto_play) {
            video_player->Play();
        }

        // Extract metadata for first clip in background
        if (!sorted_clips.empty()) {
            QueueVideoMetadataExtraction(sorted_clips[0].file_path, true);  // High priority for first clip
        }
    }

    bool ProjectManager::IsSequenceMode() const {
        return !current_sequence_id.empty();
    }

    Sequence* ProjectManager::GetCurrentSequence() const {
        if (current_sequence_id.empty()) return nullptr;

        for (const auto& sequence : sequences) {
            if (sequence.id == current_sequence_id) {
                return const_cast<Sequence*>(&sequence);
            }
        }
        return nullptr;
    }

    void ProjectManager::SwitchToSequence(const std::string& sequence_id) {
        current_sequence_id = sequence_id;
        UpdateSequenceActiveStates(sequence_id);
    }

    void ProjectManager::ClearSequenceMode() {
        current_sequence_id.clear();
        UpdateSequenceActiveStates("");
    }

    void ProjectManager::UpdateSequenceActiveStates(const std::string& active_sequence_id) {
        // Update media_pool items
        for (auto& item : media_pool) {
            if (item.type == MediaType::SEQUENCE) {
                item.is_active = (item.sequence_id == active_sequence_id);
            }
        }

        // Update bin items
        for (auto& bin : bins) {
            for (auto& item : bin.items) {
                if (item.type == MediaType::SEQUENCE) {
                    item.is_active = (item.sequence_id == active_sequence_id);
                }
            }
        }
    }

    void ProjectManager::UpdateSequenceInBin(const std::string& sequence_id) {
        auto seq_it = std::find_if(sequences.begin(), sequences.end(),
            [&sequence_id](const Sequence& seq) { return seq.id == sequence_id; });

        if (seq_it == sequences.end()) return;

        const Sequence& sequence = *seq_it;

        // Update media_pool items
        for (auto& item : media_pool) {
            if (item.type == MediaType::SEQUENCE && item.sequence_id == sequence_id) {
                item.clip_count = sequence.clips.size();
                item.duration = sequence.duration;
                item.is_active = (sequence_id == current_sequence_id);
            }
        }

        // Update bin items
        for (auto& bin : bins) {
            for (auto& item : bin.items) {
                if (item.type == MediaType::SEQUENCE && item.sequence_id == sequence_id) {
                    item.clip_count = sequence.clips.size();
                    item.duration = sequence.duration;
                    item.is_active = (sequence_id == current_sequence_id);
                }
            }
        }
    }

    // ============================================================================
    // PLAYLIST OPERATIONS
    // ============================================================================

    void ProjectManager::AddToPlaylist(const std::string& media_id) {
        Sequence* seq = GetOrCreateCurrentSequence();
        if (!seq) return;

        MediaItem* media_item = GetMediaItem(media_id);
        if (!media_item) return;

        // Prevent image sequences from being added to playlists
        // IMAGE_SEQUENCE (TIFF/PNG/JPEG) and EXR_SEQUENCE both use DirectEXRCache workflow incompatible with playlists
        if (media_item->type == MediaType::EXR_SEQUENCE || media_item->type == MediaType::IMAGE_SEQUENCE) {
            Debug::Log("WARNING: Cannot add image sequence to playlist - Image sequences use a pure cache workflow incompatible with playlists");
            return;
        }

        TimelineClip clip;
        clip.id = GenerateUniqueID();
        clip.media_id = media_id;
        clip.name = media_item->name;
        clip.file_path = media_item->path;
        clip.duration = media_item->duration;
        clip.start_time = seq->duration;
        clip.track_type = "video";

        seq->clips.push_back(clip);
        seq->UpdateDuration();

        // === SMART CACHING FOR PLAYLIST DRAG-DROP ===
        bool should_cache_immediately = false;

        // Case 1: First item in empty playlist (very likely to be accessed)
        if (seq->clips.size() == 1) {
            should_cache_immediately = true;
            Debug::Log("AddToPlaylist: First item in playlist, enabling immediate cache");
        }
        // Case 2: Item added at current playlist position (user might scrub immediately)
        else {
            int current_playlist_pos = GetCurrentPlaylistIndex();
            int new_item_index = static_cast<int>(seq->clips.size()) - 1; // Just added at end
            if (current_playlist_pos == new_item_index) {
                should_cache_immediately = true;
                Debug::Log("AddToPlaylist: Item added at current position, enabling immediate cache");
            }
        }

        if (should_cache_immediately) {
            // Notify cache system about the new item
            // Note: Image sequences (mf://) are automatically skipped by NotifyVideoChanged (they use DirectEXRCache only)
            if (video_change_callback) {
                video_change_callback(media_item->path);
            }
            NotifyVideoChanged(media_item->path);
        }

        RebuildPlaylistInMPV();
        UpdateSequenceInBin(seq->id);
    }

    void ProjectManager::AddMultipleToPlaylist(const std::string& payload_string) {
        std::vector<std::string> media_ids = ParsePayloadString(payload_string);

        Sequence* seq = GetOrCreateCurrentSequence();
        if (!seq) return;

        bool was_empty = seq->clips.empty();
        MediaItem* first_media_item = nullptr;
        int image_seq_count = 0;  // Track how many image sequences were skipped

        for (const auto& media_id : media_ids) {
            MediaItem* media_item = GetMediaItem(media_id);
            if (!media_item) continue;

            // Prevent image sequences from being added to playlists
            // IMAGE_SEQUENCE (TIFF/PNG/JPEG) and EXR_SEQUENCE both use DirectEXRCache workflow incompatible with playlists
            if (media_item->type == MediaType::EXR_SEQUENCE || media_item->type == MediaType::IMAGE_SEQUENCE) {
                image_seq_count++;
                continue;  // Skip this item
            }

            // Remember first item for potential caching
            if (!first_media_item) {
                first_media_item = media_item;
            }

            TimelineClip clip;
            clip.id = GenerateUniqueID();
            clip.media_id = media_id;
            clip.name = media_item->name;
            clip.file_path = media_item->path;
            clip.duration = media_item->duration;
            clip.start_time = seq->duration;
            clip.track_type = "video";

            seq->clips.push_back(clip);
            seq->UpdateDuration();
        }

        // === SMART CACHING FOR MULTIPLE DRAG-DROP ===
        // Only cache the first item if playlist was empty (avoid cache spam)
        if (was_empty && first_media_item) {
            Debug::Log("AddMultipleToPlaylist: First item in empty playlist, enabling cache for: " + first_media_item->name);

            // Notify cache system
            // Note: Image sequences (mf://) are automatically skipped by NotifyVideoChanged (they use DirectEXRCache only)
            if (video_change_callback) {
                video_change_callback(first_media_item->path);
            }
            NotifyVideoChanged(first_media_item->path);
        }

        // Log warning if any image sequences were skipped
        if (image_seq_count > 0) {
            Debug::Log("WARNING: Skipped " + std::to_string(image_seq_count) + " image sequence(s) - Image sequences use a pure cache workflow incompatible with playlists");
        }

        RebuildPlaylistInMPV();
        UpdateSequenceInBin(seq->id);
    }

    void ProjectManager::ClearCurrentPlaylist() {
        Sequence* seq = GetCurrentSequence();
        if (!seq) return;

        seq->clips.clear();
        seq->duration = 0.0;

        if (video_player && video_player->GetMPVHandle()) {
            const char* cmd[] = { "playlist-clear", nullptr };
            mpv_command(video_player->GetMPVHandle(), cmd);
            video_player->Stop();
        }

        UpdateSequenceInBin(seq->id);
        cached_playlist_position = -1;
    }

    void ProjectManager::RemoveFromPlaylist(int index) {
        Sequence* seq = GetCurrentSequence();
        if (!seq || index < 0 || index >= (int)seq->clips.size()) return;

        auto sorted_clips = seq->GetAllClipsSorted();
        seq->clips.erase(
            std::remove_if(seq->clips.begin(), seq->clips.end(),
                [&sorted_clips, index](const TimelineClip& clip) {
                    return clip.id == sorted_clips[index].id;
                }),
            seq->clips.end());

        seq->UpdateDuration();
        RebuildPlaylistInMPV();
        UpdateSequenceInBin(seq->id);
        ReloadCurrentPlaylist();
    }

    void ProjectManager::RebuildPlaylistInMPV() {
        Sequence* seq = GetCurrentSequence();
        if (!seq || !video_player) return;

        if (seq->clips.empty()) {
            if (video_player->GetMPVHandle()) {
                const char* cmd[] = { "playlist-clear", nullptr };
                mpv_command(video_player->GetMPVHandle(), cmd);
            }
            if (current_file_path) {
                current_file_path->clear();
            }
            return;
        }

        std::string playlist_content;
        auto sorted_clips = seq->GetAllClipsSorted();
        for (const auto& clip : sorted_clips) {
            playlist_content += clip.file_path + "\n";
        }

        video_player->LoadPlaylist(playlist_content);
        cached_playlist_position = 0;

        if (!sorted_clips.empty() && current_file_path) {
            *current_file_path = sorted_clips[0].file_path;
            QueueVideoMetadataExtraction(sorted_clips[0].file_path, true);  // High priority for first clip
        }
    }

    void ProjectManager::ReloadCurrentPlaylist() {
        Sequence* seq = GetCurrentSequence();
        if (!seq) return;

        if (seq->clips.empty()) {
            if (video_player && video_player->GetMPVHandle()) {
                const char* cmd[] = { "playlist-clear", nullptr };
                mpv_command(video_player->GetMPVHandle(), cmd);
                video_player->Stop();
            }
            return;
        }

        int old_position = GetCurrentPlaylistIndex();

        if (video_player && video_player->GetMPVHandle()) {
            const char* clear_cmd[] = { "playlist-clear", nullptr };
            mpv_command(video_player->GetMPVHandle(), clear_cmd);
        }

        RebuildPlaylistInMPV();

        int new_playlist_length = GetPlaylistLength();
        int target_position = (std::min)(old_position, new_playlist_length - 1);
        target_position = (std::max)(target_position, 0);

        if (target_position >= 0 && target_position < new_playlist_length) {
            JumpToPlaylistIndex(target_position);
        }
    }

    Sequence* ProjectManager::GetOrCreateCurrentSequence() {
        Sequence* seq = GetCurrentSequence();
        if (!seq) {
            CreateNewSequence("Main Playlist");
            seq = GetCurrentSequence();
        }
        return seq;
    }

    // ============================================================================
    // PLAYLIST PLAYBACK CONTROL
    // ============================================================================

    int ProjectManager::GetCurrentPlaylistIndex() const {
        if (!video_player || !video_player->GetMPVHandle()) return -1;

        int64_t playlist_pos = -1;
        if (mpv_get_property(video_player->GetMPVHandle(), "playlist-pos", MPV_FORMAT_INT64, &playlist_pos) == 0) {
            cached_playlist_position = (int)playlist_pos;
            return (int)playlist_pos;
        }
        return cached_playlist_position;
    }

    int ProjectManager::GetPlaylistLength() const {
        if (!video_player || !video_player->GetMPVHandle()) return 0;

        int64_t playlist_count = 0;
        if (mpv_get_property(video_player->GetMPVHandle(), "playlist-count", MPV_FORMAT_INT64, &playlist_count) == 0) {
            return (int)playlist_count;
        }
        return 0;
    }

    std::string ProjectManager::GetCurrentClipName() const {
        if (!IsInSequenceMode()) {
            return current_file_path ? *current_file_path : "";
        }

        auto seq = GetCurrentSequence();
        if (!seq || seq->clips.empty()) return "";

        int current_index = GetCurrentPlaylistIndex();
        if (current_index >= 0 && current_index < (int)seq->clips.size()) {
            auto sorted_clips = seq->GetAllClipsSorted();
            return sorted_clips[current_index].name;
        }
        return "";
    }

    void ProjectManager::GoToNextInPlaylist() {
        if (!video_player || !video_player->GetMPVHandle() || !IsInSequenceMode()) return;

        const char* cmd[] = { "playlist-next", nullptr };
        if (mpv_command(video_player->GetMPVHandle(), cmd) >= 0) {
            SyncPlaylistPosition();
        }
    }

    void ProjectManager::GoToPreviousInPlaylist() {
        if (!video_player || !video_player->GetMPVHandle() || !IsInSequenceMode()) return;

        const char* cmd[] = { "playlist-prev", nullptr };
        if (mpv_command(video_player->GetMPVHandle(), cmd) >= 0) {
            SyncPlaylistPosition();
        }
    }

    void ProjectManager::JumpToPlaylistIndex(int index) {
        if (!video_player || !video_player->GetMPVHandle() || !IsInSequenceMode()) return;

        auto seq = GetCurrentSequence();
        if (!seq || index < 0 || index >= (int)seq->clips.size()) return;

        int64_t target_pos = index;
        if (mpv_set_property(video_player->GetMPVHandle(), "playlist-pos", MPV_FORMAT_INT64, &target_pos) >= 0) {
            cached_playlist_position = index;
            SyncPlaylistPosition();
        }
    }

    void ProjectManager::SyncPlaylistPosition() {
        if (!video_player || !video_player->GetMPVHandle() || !current_file_path) return;

        int64_t current_pos = -1;
        if (mpv_get_property(video_player->GetMPVHandle(), "playlist-pos", MPV_FORMAT_INT64, &current_pos) == 0) {
            cached_playlist_position = (int)current_pos;

            if (IsInSequenceMode()) {
                auto seq = GetCurrentSequence();
                if (seq && !seq->clips.empty()) {
                    auto sorted_clips = seq->GetAllClipsSorted();
                    if (current_pos >= 0 && current_pos < (int)sorted_clips.size()) {
                        std::string new_file_path = sorted_clips[current_pos].file_path;
                        if (*current_file_path != new_file_path) {
                            *current_file_path = new_file_path;
                            QueueVideoMetadataExtraction(new_file_path, true);  // High priority for current clip

                            // === NOTIFY VIDEO PLAYER OF PLAYLIST ITEM CHANGE ===
                            // This handles thumbnail cache updates and audio filter switching
                            if (video_player) {
                                video_player->OnPlaylistItemChanged(new_file_path);
                            }

                            // === NOTIFY CACHE SYSTEM FOR PLAYLIST SWITCHES ===
                            // Note: Image sequences (mf://) are automatically skipped by NotifyVideoChanged (they use DirectEXRCache only)
                            if (video_change_callback) {
                                video_change_callback(new_file_path);
                            }
                            NotifyVideoChanged(new_file_path);
                        }
                    }
                }
            }
        }
    }

    // ============================================================================
    // PLAYLIST SELECTION MANAGEMENT
    // ============================================================================

    void ProjectManager::SelectPlaylistItem(int index, bool ctrl_held, bool shift_held) {
        if (!ctrl_held && !shift_held) {
            selected_playlist_indices.clear();
            selected_playlist_indices.insert(index);
            last_selected_playlist_index = index;
        }
        else if (ctrl_held) {
            if (selected_playlist_indices.count(index)) {
                selected_playlist_indices.erase(index);
            }
            else {
                selected_playlist_indices.insert(index);
                last_selected_playlist_index = index;
            }
        }
        else if (shift_held && last_selected_playlist_index >= 0) {
            int min_idx = (std::min)(last_selected_playlist_index, index);
            int max_idx = (std::max)(last_selected_playlist_index, index);
            for (int i = min_idx; i <= max_idx; i++) {
                selected_playlist_indices.insert(i);
            }
        }
    }

    void ProjectManager::ClearPlaylistSelection() {
        selected_playlist_indices.clear();
        last_selected_playlist_index = -1;
    }

    bool ProjectManager::IsPlaylistItemSelected(int index) const {
        return selected_playlist_indices.count(index) > 0;
    }

    int ProjectManager::GetSelectedPlaylistItemsCount() const {
        return static_cast<int>(selected_playlist_indices.size());
    }

    void ProjectManager::DeleteSelectedPlaylistItems() {
        if (selected_playlist_indices.empty()) return;

        std::vector<int> indices_to_delete(selected_playlist_indices.begin(), selected_playlist_indices.end());
        std::sort(indices_to_delete.rbegin(), indices_to_delete.rend());

        for (int index : indices_to_delete) {
            RemoveFromPlaylist(index);
        }

        ClearPlaylistSelection();
        ReloadCurrentPlaylist();
    }

    void ProjectManager::MoveSelectedPlaylistItemsUp() {
        if (selected_playlist_indices.empty()) return;

        std::vector<int> indices(selected_playlist_indices.begin(), selected_playlist_indices.end());
        std::sort(indices.begin(), indices.end());

        for (int index : indices) {
            if (index > 0) {
                MovePlaylistItem(index, index - 1);
            }
        }

        // Update selection
        std::set<int> new_selection;
        for (int index : selected_playlist_indices) {
            new_selection.insert(index > 0 ? index - 1 : index);
        }
        selected_playlist_indices = new_selection;
        ReloadCurrentPlaylist();
    }

    void ProjectManager::MoveSelectedPlaylistItemsDown() {
        if (selected_playlist_indices.empty()) return;

        Sequence* seq = GetCurrentSequence();
        if (!seq) return;

        int max_index = (int)seq->clips.size() - 1;
        std::vector<int> indices(selected_playlist_indices.begin(), selected_playlist_indices.end());
        std::sort(indices.rbegin(), indices.rend());

        for (int index : indices) {
            if (index < max_index) {
                MovePlaylistItem(index, index + 1);
            }
        }

        // Update selection
        std::set<int> new_selection;
        for (int index : selected_playlist_indices) {
            new_selection.insert(index < max_index ? index + 1 : index);
        }
        selected_playlist_indices = new_selection;
        ReloadCurrentPlaylist();
    }

    void ProjectManager::MovePlaylistItem(int from_index, int to_index) {
        Sequence* seq = GetCurrentSequence();
        if (!seq) return;

        auto sorted_clips = seq->GetAllClipsSorted();
        if (from_index < 0 || from_index >= (int)sorted_clips.size() ||
            to_index < 0 || to_index >= (int)sorted_clips.size()) {
            return;
        }

        TimelineClip moving_clip = sorted_clips[from_index];
        sorted_clips.erase(sorted_clips.begin() + from_index);
        sorted_clips.insert(sorted_clips.begin() + to_index, moving_clip);

        // Update start times
        double current_time = 0.0;
        for (auto& clip : sorted_clips) {
            clip.start_time = current_time;
            current_time += clip.duration;
        }

        seq->clips = sorted_clips;
        seq->UpdateDuration();
        ReloadCurrentPlaylist();
    }

    void ProjectManager::RemoveDuplicatesFromPlaylist() {
        Sequence* seq = GetCurrentSequence();
        if (!seq) return;

        std::set<std::string> seen_paths;
        std::vector<TimelineClip> unique_clips;

        for (const auto& clip : seq->clips) {
            if (seen_paths.find(clip.file_path) == seen_paths.end()) {
                seen_paths.insert(clip.file_path);
                unique_clips.push_back(clip);
            }
        }

        if (unique_clips.size() != seq->clips.size()) {
            seq->clips = unique_clips;
            seq->UpdateDuration();
            RebuildPlaylistInMPV();
            UpdateSequenceInBin(seq->id);
        }
    }

    // ============================================================================
    // DRAG & DROP OPERATIONS
    // ============================================================================

    void ProjectManager::LoadSingleFileFromDrop(const std::string& file_path) {
        Debug::Log("LoadSingleFileFromDrop called with: " + file_path);

        if (!IsValidMediaFile(file_path)) {
            Debug::Log("File is not valid media: " + file_path);
            return;
        }

        Debug::Log("File is valid media, checking type...");

        // Wait for file to be readable before checking if it's a sequence (cloud sync)
        Debug::Log("LoadSingleFileFromDrop: Checking if file is readable (cloud sync check)...");
        if (!WaitForFileReadable(file_path, 30)) {
            Debug::Log("ERROR: File not readable after waiting - may not be synced from cloud");
            return;
        }
        Debug::Log("LoadSingleFileFromDrop: File is readable, proceeding with type detection");

        // Check if this is an image sequence
        MediaType media_type = GetMediaType(file_path);
        Debug::Log("Media type determined: " + std::to_string(static_cast<int>(media_type)));

        if (media_type == MediaType::IMAGE_SEQUENCE || media_type == MediaType::EXR_SEQUENCE) {
            Debug::Log("Detected as image/EXR sequence, showing frame rate dialog");

            // Show frame rate dialog for image sequences
            ShowFrameRateDialog(file_path);
            return; // ProcessImageSequence will handle the actual loading
        }

        Debug::Log("Processing as regular media file");

        AddMediaFileToProject(file_path);

        current_sequence_id.clear();
        UpdateSequenceActiveStates("");

        video_player->LoadFile(file_path);
        *current_file_path = file_path;
        /*video_player->Play();*/

        // === NOTIFY MAIN ABOUT VIDEO CHANGE === (same as LoadSingleMediaItem)
        if (video_change_callback) {
            video_change_callback(file_path);
        }

        // === NOTIFY VIDEO CACHE MANAGER ===
        NotifyVideoChanged(file_path);

        // Extract metadata in background to avoid playback blocking
        QueueVideoMetadataExtraction(file_path, true);  // High priority for loaded video
    }

    void ProjectManager::LoadMultipleFilesFromDrop(const std::vector<std::string>& file_paths) {
        std::set<std::string> processed_sequences; // Track sequences we've already processed

        for (const auto& file_path : file_paths) {
            if (!IsValidMediaFile(file_path)) continue;

            // Wait for file to be readable before checking if it's a sequence (cloud sync)
            Debug::Log("LoadMultipleFilesFromDrop: Checking if file is readable (cloud sync check): " + file_path);
            if (!WaitForFileReadable(file_path, 30)) {
                Debug::Log("ERROR: File not readable after waiting, skipping: " + file_path);
                continue;
            }
            Debug::Log("LoadMultipleFilesFromDrop: File is readable, proceeding with type detection");

            MediaType media_type = GetMediaType(file_path);
            if (media_type == MediaType::IMAGE_SEQUENCE || media_type == MediaType::EXR_SEQUENCE) {
                // For image sequences, we need to group them and process once per sequence
                std::vector<std::string> sequence_files = DetectImageSequence(file_path);
                if (!sequence_files.empty()) {
                    // Use the first file as the sequence identifier
                    std::string sequence_id = sequence_files[0];

                    if (processed_sequences.find(sequence_id) == processed_sequences.end()) {
                        processed_sequences.insert(sequence_id);
                        // Show frame rate dialog for the first file of each sequence
                        ShowFrameRateDialog(file_path);
                    }
                }
            } else {
                // Regular media files
                AddMediaFileToProject(file_path);
            }
        }
    }

    bool ProjectManager::IsValidMediaFile(const std::string& file_path) {
        size_t dot_pos = file_path.find_last_of('.');
        if (dot_pos == std::string::npos) return false;

        std::string extension = file_path.substr(dot_pos + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        // Supported formats only: Video, Audio, JPEG, PNG, TIFF, EXR
        // Removed unsupported: BMP, TGA, DPX, JPEG2000 (j2k, jp2)
        std::vector<std::string> supported = {
            // Video formats
            "mp4", "avi", "mkv", "mov", "wmv", "flv", "webm", "m4v", "3gp", "ogv", "ts", "mts", "m2ts", "mxf",
            // Audio formats
            "wav", "mp3", "aac", "flac", "ogg", "wma", "m4a",
            // Image formats (supported only)
            "jpg", "jpeg", "png", "tiff", "tif", "exr", "hdr"
        };

        return std::find(supported.begin(), supported.end(), extension) != supported.end();
    }

    // ============================================================================
    // METADATA MANAGEMENT
    // ============================================================================

    void ProjectManager::StartAdobeWorkerThread() {
        worker_running = true;
        adobe_worker_thread = std::thread(&ProjectManager::AdobeWorkerLoop, this);
    }

    void ProjectManager::StopAdobeWorkerThread() {
        worker_running = false;
        if (adobe_worker_thread.joinable()) {
            adobe_worker_thread.join();
        }
    }

    void ProjectManager::AdobeWorkerLoop() {
        while (worker_running) {
            std::string file_path;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!adobe_metadata_queue.empty()) {
                    file_path = adobe_metadata_queue.front();
                    adobe_metadata_queue.pop();
                }
            }

            if (!file_path.empty()) {
                ProcessAdobeMetadata(file_path);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void ProjectManager::ProcessAdobeMetadata(const std::string& file_path) {
        auto adobe_meta = AdobeMetadataExtractor::ExtractAdobePaths(file_path);
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            auto it = metadata_cache.find(file_path);
            if (it != metadata_cache.end()) {
                it->second.adobe_meta = std::move(adobe_meta);
                it->second.state = MetadataState::COMPLETE;

                // Log extraction timing
                auto duration = std::chrono::steady_clock::now() - it->second.start_time;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                // Debug removed
            }
        }
    }

    void ProjectManager::QueueAdobeMetadata(const std::string& file_path) {
        // Skip Adobe metadata extraction for image sequences and EXRs
        if (ShouldSkipAdobeMetadataExtraction(file_path)) {
            return;
        }

        std::lock_guard<std::mutex> lock(queue_mutex);
        adobe_metadata_queue.push(file_path);
    }

    const ProjectManager::CombinedMetadata* ProjectManager::GetCachedMetadata(const std::string& file_path) const {
        auto it = metadata_cache.find(file_path);
        return (it != metadata_cache.end()) ? &it->second : nullptr;
    }

    void ProjectManager::ExtractMetadataForClip(const std::string& file_path) {
        // Deprecated method - use QueueVideoMetadataExtraction instead
        QueueVideoMetadataExtraction(file_path, true);
    }

    bool ProjectManager::ShouldSkipAdobeMetadataExtraction(const std::string& file_path) {
        if (file_path.empty()) return true;

        // Skip Adobe metadata extraction for MF:// (image sequence) URLs
        if (file_path.substr(0, 5) == "mf://") {
            return true;
        }

        // Skip Adobe metadata extraction for custom EXR:// URLs
        if (file_path.substr(0, 6) == "exr://") {
            return true;
        }

        // Check file extension for image sequences and EXRs
        try {
            std::filesystem::path path(file_path);
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            // Skip Adobe metadata for image sequence formats and EXRs
            // These files don't contain Adobe project paths or timecode info
            if (extension == ".exr" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".png" || extension == ".bmp" || extension == ".tiff" ||
                extension == ".tif" || extension == ".tga" || extension == ".hdr" ||
                extension == ".dpx") {

                return true; // Skip Adobe metadata for all image formats
            }
        } catch (...) {
            // If path parsing fails, don't skip (safer to extract if unsure)
            return false;
        }

        return false; // Extract Adobe metadata for video/audio files
    }

    void ProjectManager::QueueVideoMetadataExtraction(const std::string& file_path, bool high_priority) {
        if (file_path.empty()) return;

        // Special handling for image sequences (MF:// URLs)
        if (file_path.substr(0, 5) == "mf://") {
            // For image sequences, extract metadata from the first frame only
            std::string mf_url = file_path;

            // Parse the MF URL to get the first frame path
            // Format: mf://path/to/sequence_%04d.exr:fps=24
            size_t fps_pos = mf_url.find(":fps=");
            if (fps_pos != std::string::npos) {
                mf_url = mf_url.substr(0, fps_pos); // Remove fps parameter
            }

            if (mf_url.substr(0, 5) == "mf://") {
                std::string pattern_path = mf_url.substr(5); // Remove "mf://"

                // Convert printf-style pattern to actual first frame
                // e.g., "path/sequence_%04d.exr" -> find actual first frame
                size_t printf_pos = pattern_path.find('%');
                if (printf_pos != std::string::npos) {
                    // Find the end of the printf pattern
                    size_t d_pos = pattern_path.find('d', printf_pos);
                    if (d_pos != std::string::npos) {
                        std::string directory = pattern_path.substr(0, pattern_path.find_last_of('/'));
                        std::string base_part = pattern_path.substr(0, printf_pos);
                        base_part = base_part.substr(base_part.find_last_of('/') + 1);
                        std::string extension = pattern_path.substr(d_pos + 1);

                        // Parse the base name to understand the pattern
                        std::regex pattern_regex(R"(^(.+?)([_\.\-]?)$)");
                        std::smatch pattern_match;

                        std::string base_name, separator;
                        if (std::regex_match(base_part, pattern_match, pattern_regex)) {
                            base_name = pattern_match[1].str();
                            separator = pattern_match[2].str();
                        } else {
                            base_name = base_part;
                        }

                        // Find the first matching file in the directory
                        try {
                            std::vector<std::string> matching_files;
                            std::regex file_pattern(base_name + separator + R"(\d{1,12})" + extension);

                            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                                if (entry.is_regular_file()) {
                                    std::string filename = entry.path().filename().string();
                                    if (std::regex_match(filename, file_pattern)) {
                                        matching_files.push_back(entry.path().string());
                                    }
                                }
                            }

                            if (!matching_files.empty()) {
                                // Sort and use the first file
                                std::sort(matching_files.begin(), matching_files.end());
                                QueueVideoMetadataExtraction(matching_files[0], high_priority);
                                return;
                            }
                        } catch (const std::filesystem::filesystem_error&) {
                            // If we can't read the directory, fall back to normal processing
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex);

            // Skip if metadata already exists or is being processed
            auto it = metadata_cache.find(file_path);
            if (it != metadata_cache.end()) {
                if (it->second.state != MetadataState::NOT_STARTED) {
                    return;  // Already processing or complete
                }
            }

            // Initialize metadata entry with loading state
            metadata_cache[file_path].state = MetadataState::LOADING_VIDEO;
            metadata_cache[file_path].start_time = std::chrono::steady_clock::now();
        }

        {
            std::lock_guard<std::mutex> lock(video_queue_mutex);
            video_metadata_queue.push({file_path, high_priority});
        }

        // Debug removed
    }

    void ProjectManager::StartVideoMetadataWorkerThread() {
        video_worker_running = true;
        video_metadata_worker_thread = std::thread(&ProjectManager::VideoMetadataWorkerLoop, this);
        // Debug removed
    }

    void ProjectManager::StopVideoMetadataWorkerThread() {
        video_worker_running = false;
        if (video_metadata_worker_thread.joinable()) {
            video_metadata_worker_thread.join();
        }
        // Debug removed
    }

    void ProjectManager::VideoMetadataWorkerLoop() {
        while (video_worker_running) {
            std::string file_path;
            bool high_priority = false;

            {
                std::lock_guard<std::mutex> lock(video_queue_mutex);
                if (!video_metadata_queue.empty()) {
                    auto item = video_metadata_queue.front();
                    file_path = item.first;
                    high_priority = item.second;
                    video_metadata_queue.pop();
                }
            }

            if (!file_path.empty()) {
                ProcessVideoMetadata(file_path);
            }
            else {
                // No work to do, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void ProjectManager::ProcessVideoMetadata(const std::string& file_path) {
        // Debug removed

        // Extract MPV metadata (non-blocking approach)
        if (video_player && video_player->HasVideo()) {
            // Small delay to ensure video is stable, but much shorter than before
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            auto video_meta = std::make_unique<VideoMetadata>(video_player->ExtractMetadataFast());

            // Store video metadata and update state
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                auto& cached_meta = metadata_cache[file_path];
                cached_meta.video_meta = std::move(video_meta);
                cached_meta.state = MetadataState::VIDEO_READY;
            }

            // Debug removed

            // Update cache system with new metadata
            if (video_cache_manager && video_cache_manager->IsCachingEnabled()) {
                const CombinedMetadata* cached_meta = GetCachedMetadata(file_path);
                if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                    video_cache_manager->UpdateVideoMetadata(file_path, *cached_meta->video_meta);

                    // Check if this is H.264/H.265 and disable cache if needed
                    std::string codec = cached_meta->video_meta->video_codec;
                    std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);
                    current_video_codec = cached_meta->video_meta->video_codec;

                    // Skip H.264 detection for image sequences - they use dummy H.264 videos for timing
                    // but actual frames come from DirectEXRCache (no B-frame issues)
                    bool is_image_sequence = (file_path.substr(0, 5) == "mf://") ||
                                             (file_path.substr(0, 6) == "exr://");

                    if (!is_image_sequence && (codec.find("h264") != std::string::npos ||
                        codec.find("hevc") != std::string::npos ||
                        codec.find("h265") != std::string::npos ||
                        codec.find("avc") != std::string::npos)) {

                        Debug::Log("=== H.264/H.265 DETECTED ===");
                        Debug::Log("Codec: " + current_video_codec);
                        Debug::Log("Disabling cache for B-frame safety");

                        SetCacheEnabled(false);
                        ClearAllCaches();  // Clear ALL caches, not just current
                        cache_auto_disabled_for_codec = true;

                        Debug::Log("Cache disabled and ALL caches cleared for: " + file_path);
                    } else if (is_image_sequence) {
                        Debug::Log("=== H.264/H.265 IN DUMMY VIDEO (image sequence) ===");
                        Debug::Log("Skipping cache disable - image sequences use DirectEXRCache for actual frames");
                    }
                }
            }

            // Queue Adobe metadata for later processing (no additional delay)
            QueueAdobeMetadata(file_path);
        }
        else {
            // Update state to indicate failure
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                auto& cached_meta = metadata_cache[file_path];
                cached_meta.state = MetadataState::NOT_STARTED;  // Reset to allow retry
            }
            // Debug removed
        }
    }

    void ProjectManager::DisplayVideoMetadata(const VideoMetadata* video_meta) {
        if (!video_meta || !video_meta->is_loaded) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No metadata available");
            return;
        }

        bool is_audio_only = IsAudioOnlyFile(video_meta);

        // Always show file information
        if (ImGui::CollapsingHeader("File Information", ImGuiTreeNodeFlags_DefaultOpen)) {
            DisplayFileInfoTable(video_meta);
        }

        // For audio-only files, prioritize audio properties
        if (is_audio_only) {
            if (HasAudioInfo(video_meta) && ImGui::CollapsingHeader("Audio Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
                DisplayAudioPropertiesTable(video_meta);
            }
        } else {
            // For video files, show video properties first
            if (ImGui::CollapsingHeader("Video Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
                DisplayVideoPropertiesTable(video_meta);
            }

            if (HasColorInfo(video_meta) && ImGui::CollapsingHeader("Color Properties")) {
                DisplayColorPropertiesTable(video_meta);
            }

            if (HasAudioInfo(video_meta) && ImGui::CollapsingHeader("Audio Properties")) {
                DisplayAudioPropertiesTable(video_meta);
            }
        }
    }

    void ProjectManager::DisplayAdobeMetadata(const AdobeMetadata* adobe_meta) {
        if (!adobe_meta || !adobe_meta->is_loaded) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Loading Adobe project info...");
            return;
        }

        if (!adobe_meta->HasAnyAdobeProject()) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No Adobe project links found");
            return;
        }

        if (ImGui::CollapsingHeader("Adobe Projects", ImGuiTreeNodeFlags_DefaultOpen)) {
            DisplayAdobeProjectsTable(adobe_meta);
        }
    }

    void ProjectManager::DisplayTimecodeTable(const AdobeMetadata* adobe_meta) {
        if (!adobe_meta) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No timecode metadata available");
            return;
        }

        if (!adobe_meta->HasAnyTimecode()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No timecode found in metadata");
            return;
        }

        if (ImGui::BeginTable("TimecodeTable", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Timecode", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f);

            // QuickTime Start Timecode
            if (!adobe_meta->qt_start_timecode.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("QT Start:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", adobe_meta->qt_start_timecode.c_str());
                if (font_mono) ImGui::PopFont();
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("Copy##QTStart")) {
                    CopyToClipboard(adobe_meta->qt_start_timecode);
                }
            }

            // QuickTime General Timecode
            if (!adobe_meta->qt_timecode.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("QT TimeCode:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", adobe_meta->qt_timecode.c_str());
                if (font_mono) ImGui::PopFont();
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("Copy##QTCode")) {
                    CopyToClipboard(adobe_meta->qt_timecode);
                }
            }

            // Creation dates for reference
            if (!adobe_meta->qt_creation_date.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Created:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", adobe_meta->qt_creation_date.c_str());
                if (font_mono) ImGui::PopFont();
            }

            if (!adobe_meta->qt_media_create_date.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Media Created:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", adobe_meta->qt_media_create_date.c_str());
                if (font_mono) ImGui::PopFont();
            }

            ImGui::EndTable();
        }
    }

    // ============================================================================
    // METADATA DISPLAY HELPERS
    // ============================================================================

    void ProjectManager::DisplayFileInfoTable(const VideoMetadata* video_meta) {
        if (ImGui::BeginTable("FileInfoTable", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 100.0f);

            // File Name
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Name:");
            ImGui::TableSetColumnIndex(1);
            if (font_mono) ImGui::PushFont(font_mono);
            ImGui::TextWrapped("%s", video_meta->file_name.c_str());
            if (font_mono) ImGui::PopFont();

            // File Path
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Path:");
            ImGui::TableSetColumnIndex(1);
            if (font_mono) ImGui::PushFont(font_mono);
            ImGui::TextWrapped("%s", video_meta->file_path.c_str());
            if (font_mono) ImGui::PopFont();
            ImGui::TableSetColumnIndex(2);

            if (ImGui::SmallButton("Open##Path")) {
                OpenFileInExplorer(video_meta->file_path);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open in Windows Explorer");
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Copy##Path")) {
                CopyToClipboard(video_meta->file_path);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Copy path to clipboard");
            }

            // File Size
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Size:");
            ImGui::TableSetColumnIndex(1);
            if (font_mono) ImGui::PushFont(font_mono);
            if (video_meta->file_size > 0) {
                double size_mb = video_meta->file_size / (1024.0 * 1024.0);
                if (size_mb >= 1024.0) {
                    ImGui::Text("%.2f GB", size_mb / 1024.0);
                }
                else {
                    ImGui::Text("%.2f MB", size_mb);
                }
            }
            else {
                ImGui::Text("Unknown");
            }
            if (font_mono) ImGui::PopFont();

            ImGui::EndTable();
        }
    }

    void ProjectManager::DisplayVideoPropertiesTable(const VideoMetadata* video_meta) {
        if (ImGui::BeginTable("VideoPropsTable", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            if (video_meta->width > 0 && video_meta->height > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Resolution:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%dx%d", video_meta->width, video_meta->height);
                if (font_mono) ImGui::PopFont();
            }

            if (video_meta->frame_rate > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Frame Rate:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%.3f fps", video_meta->frame_rate);
                if (font_mono) ImGui::PopFont();
            }

            if (video_meta->total_frames > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Total Frames:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%d", video_meta->total_frames);
                if (font_mono) ImGui::PopFont();
            }

            if (!video_meta->video_codec.empty() && video_meta->video_codec != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Video Codec:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%s", video_meta->video_codec.c_str());
                if (font_mono) ImGui::PopFont();
            }

            if (!video_meta->pixel_format.empty() && video_meta->pixel_format != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Pixel Format:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%s", video_meta->pixel_format.c_str());
                if (font_mono) ImGui::PopFont();
            }

            ImGui::EndTable();
        }
    }

    void ProjectManager::DisplayColorPropertiesTable(const VideoMetadata* video_meta) {
        if (ImGui::BeginTable("ColorPropsTable", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            if (!video_meta->colorspace.empty() && video_meta->colorspace != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Colorspace:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%s", video_meta->colorspace.c_str());
                if (font_mono) ImGui::PopFont();
            }

            if (!video_meta->color_primaries.empty() && video_meta->color_primaries != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Primaries:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%s", video_meta->color_primaries.c_str());
                if (font_mono) ImGui::PopFont();
            }

            if (!video_meta->color_transfer.empty() && video_meta->color_transfer != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Transfer:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%s", video_meta->color_transfer.c_str());
                if (font_mono) ImGui::PopFont();
            }

            // NEW: Display color range
            if (!video_meta->range_type.empty() && video_meta->range_type != "unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Range:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%s", video_meta->range_type.c_str());
                if (font_mono) ImGui::PopFont();
            }

            // NEW: Display color matrix status for 4444 formats
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Processing:");
            ImGui::TableSetColumnIndex(1);

            // Debug: Log when this is called
            bool is_4444 = video_meta->Is4444Format();
            Debug::Log("Inspector: Is4444Format() returned " + std::string(is_4444 ? "true" : "false") +
                      " for pixel_format: '" + video_meta->pixel_format + "'");
            if (font_mono) ImGui::PushFont(font_mono);
            if (is_4444) {
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "4444 Color Matrix Applied");
            } else {
                ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "Standard Processing");
            }
            if (font_mono) ImGui::PopFont();
            ImGui::EndTable();
        }
    }

    void ProjectManager::DisplayAudioPropertiesTable(const VideoMetadata* video_meta) {
        if (ImGui::BeginTable("AudioPropsTable", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            if (!video_meta->audio_codec.empty() && video_meta->audio_codec != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Audio Codec:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%s", video_meta->audio_codec.c_str());
                if (font_mono) ImGui::PopFont();
            }

            if (video_meta->audio_sample_rate > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Sample Rate:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%d Hz", video_meta->audio_sample_rate);
                if (font_mono) ImGui::PopFont();
            }

            if (video_meta->audio_channels > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Channels:");
                ImGui::TableSetColumnIndex(1);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("%d", video_meta->audio_channels);
                if (font_mono) ImGui::PopFont();
            }

            ImGui::EndTable();
        }
    }

    void ProjectManager::DisplayAdobeProjectsTable(const AdobeMetadata* adobe_meta) {
        if (ImGui::BeginTable("AdobeProjectsTable", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Application", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Project File", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 100.0f);

            if (!adobe_meta->ae_project_path.empty()) {
                DisplayAdobeProjectRow("After Effects:", adobe_meta->ae_project_path, "AE");
            }

            if (!adobe_meta->premiere_win_path.empty()) {
                DisplayAdobeProjectRow("Premiere (Win):", adobe_meta->premiere_win_path, "PR");
            }

            if (!adobe_meta->premiere_mac_path.empty()) {
                DisplayAdobeProjectRow("Premiere (Mac):", adobe_meta->premiere_mac_path, "PRM");
            }

            ImGui::EndTable();
        }
    }

    void ProjectManager::DisplayAdobeProjectRow(const std::string& app_name, const std::string& project_path, const std::string& button_suffix) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", app_name.c_str());

        ImGui::TableSetColumnIndex(1);
        std::string filename = std::filesystem::path(project_path).filename().string();
        if (font_mono) ImGui::PushFont(font_mono);
        ImVec4 color = Bright(GetWindowsAccentColor());
        ImGui::TextColored(color, "%s", filename.c_str());
        if (font_mono) ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", project_path.c_str());
        }

        ImGui::TableSetColumnIndex(2);
        if (button_suffix != "PRM") { // Mac paths might not work with Windows Explorer
            std::string open_button = "Open##" + button_suffix;
            if (ImGui::SmallButton(open_button.c_str())) {
                OpenFileInExplorer(project_path);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open in Windows Explorer");
            }
            ImGui::SameLine();
        }

        std::string copy_button = "Copy##" + button_suffix;
        if (ImGui::SmallButton(copy_button.c_str())) {
            CopyToClipboard(project_path);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Copy path to clipboard");
        }
    }

    // ============================================================================
    // UTILITY FUNCTIONS
    // ============================================================================

    double ProjectManager::GetTimelineDuration() const {
        return video_player ? video_player->GetDuration() : 0.0;
    }

    double ProjectManager::GetTimelinePosition() const {
        return video_player ? video_player->GetPosition() : 0.0;
    }

    ProjectManager::SequencePosition ProjectManager::CalculateSequencePosition(double global_position) const {
        SequencePosition result;

        if (!IsSequenceMode()) {
            result.clip_index = 0;
            result.clip_position = global_position;
            return result;
        }

        Sequence* current_seq = const_cast<ProjectManager*>(this)->GetCurrentSequence();
        if (!current_seq) return result;

        auto sorted_clips = current_seq->GetAllClipsSorted();
        double cumulative_time = 0.0;

        for (size_t i = 0; i < sorted_clips.size(); i++) {
            double clip_end = cumulative_time + sorted_clips[i].duration;
            if (global_position < clip_end) {
                result.clip_index = (int)i;
                result.clip_position = global_position - cumulative_time;
                result.clip_path = sorted_clips[i].file_path;
                break;
            }
            cumulative_time = clip_end;
        }

        return result;
    }

    void ProjectManager::CreateNewBin(const std::string& name) {
        ProjectBin new_bin;
        new_bin.name = name.empty() ? ("Bin " + std::to_string(bins.size() + 1)) : name;
        new_bin.is_open = true;
        bins.push_back(new_bin);
    }

    std::string ProjectManager::GenerateUniqueID() {
        static int counter = 0;
        return "item_" + std::to_string(++counter);
    }

    void ProjectManager::UpdateIDCounter() {
        // Scan all loaded IDs to find the maximum counter value
        // IDs have the format "item_N" where N is an integer
        int max_counter = 0;

        // Scan media_pool items
        for (const auto& item : media_pool) {
            if (item.id.substr(0, 5) == "item_") {
                try {
                    int id_num = std::stoi(item.id.substr(5));
                    if (id_num > max_counter) {
                        max_counter = id_num;
                    }
                } catch (...) {
                    // Skip malformed IDs
                }
            }
        }

        // Scan sequence IDs
        for (const auto& seq : sequences) {
            if (seq.id.substr(0, 5) == "item_") {
                try {
                    int id_num = std::stoi(seq.id.substr(5));
                    if (id_num > max_counter) {
                        max_counter = id_num;
                    }
                } catch (...) {
                    // Skip malformed IDs
                }
            }

            // Also scan clip IDs within sequences
            for (const auto& clip : seq.clips) {
                if (clip.id.substr(0, 5) == "item_") {
                    try {
                        int id_num = std::stoi(clip.id.substr(5));
                        if (id_num > max_counter) {
                            max_counter = id_num;
                        }
                    } catch (...) {
                        // Skip malformed IDs
                    }
                }
            }
        }

        // Update the static counter in GenerateUniqueID
        // We need to access the static variable, so we'll call GenerateUniqueID max_counter times
        // Actually, we can't easily access the static variable from here.
        // Better approach: Make the counter accessible or generate IDs until we reach max_counter

        // Generate dummy IDs to advance the counter to max_counter
        if (max_counter > 0) {
            for (int i = 0; i < max_counter; i++) {
                std::string dummy = GenerateUniqueID();
            }
            Debug::Log("UpdateIDCounter: Advanced ID counter to " + std::to_string(max_counter) +
                       " (next ID will be item_" + std::to_string(max_counter + 1) + ")");
        }
    }

    std::string ProjectManager::GetProjectName(const std::string& path) {
        std::filesystem::path file_path(path);
        return file_path.stem().string();
    }

    std::string ProjectManager::GetFileName(const std::string& path) {
        std::filesystem::path file_path(path);
        return file_path.filename().string();
    }

    bool ProjectManager::WaitForFileReadable(const std::string& file_path, int timeout_seconds) {
        Debug::Log("WaitForFileReadable: Waiting for file to become readable: " + file_path);

        const auto start_time = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(timeout_seconds);
        const int check_interval_ms = 500;  // Check every 500ms
        int attempt = 0;

        while (std::chrono::steady_clock::now() - start_time < timeout) {
            attempt++;

            // Try to open file for reading
            std::ifstream file(file_path, std::ios::binary);
            if (file.is_open()) {
                // Successfully opened - check if we can read at least 1 byte
                char byte;
                if (file.read(&byte, 1)) {
                    file.close();
                    if (attempt > 1) {
                        Debug::Log("WaitForFileReadable: File became readable after " + std::to_string(attempt) + " attempts");
                    }
                    return true;
                }
                file.close();
            }

            if (attempt == 1) {
                Debug::Log("WaitForFileReadable: File not yet readable (may be syncing from cloud)");
            }

            // Wait before next attempt
            std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms));
        }

        Debug::Log("WaitForFileReadable: File did not become readable after " + std::to_string(timeout_seconds) + " seconds");
        return false;
    }

    MediaType ProjectManager::GetMediaType(const std::string& path) const {
        std::filesystem::path file_path(path);
        std::string ext = file_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        // Video formats
        if (ext == ".mp4" || ext == ".mov" || ext == ".avi" || ext == ".mxf" ||
            ext == ".mkv" || ext == ".wmv" || ext == ".flv" || ext == ".webm" ||
            ext == ".m4v" || ext == ".3gp" || ext == ".ogv" || ext == ".ts" ||
            ext == ".mts" || ext == ".m2ts") {
            return MediaType::VIDEO;
        }

        // Audio formats
        if (ext == ".wav" || ext == ".mp3" || ext == ".aac" || ext == ".flac" ||
            ext == ".ogg" || ext == ".wma" || ext == ".m4a") {
            return MediaType::AUDIO;
        }

        // Image formats - check if it's part of a sequence
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
            ext == ".tiff" || ext == ".tif" || ext == ".tga" || ext == ".hdr" || ext == ".dpx") {

            // Check if this looks like part of an image sequence
            if (IsPartOfImageSequence(path)) {
                return MediaType::IMAGE_SEQUENCE;
            }
            return MediaType::IMAGE;
        }

        // EXR files - special handling for multi-layer support
        if (ext == ".exr") {
            // Check if this looks like part of an image sequence
            if (IsPartOfImageSequence(path)) {
                return MediaType::EXR_SEQUENCE;
            }
            return MediaType::IMAGE;
        }

        return MediaType::VIDEO; // Default fallback
    }

    int ProjectManager::GetBinIndexForMediaType(MediaType type) {
        switch (type) {
        case MediaType::VIDEO: return 0;
        case MediaType::AUDIO: return 1;
        case MediaType::IMAGE: return 2;
        case MediaType::IMAGE_SEQUENCE: return 2; // Put image sequences in Images bin as originally intended
        case MediaType::EXR_SEQUENCE: return 2; // Put EXR sequences in Images bin as originally intended
        case MediaType::SEQUENCE: return 3;
        default: return 0;
        }
    }

    double ProjectManager::GetDefaultDurationForType(MediaType type) {
        switch (type) {
        case MediaType::VIDEO: return 30.0;
        case MediaType::AUDIO: return 180.0;
        case MediaType::IMAGE: return 1.0;
        case MediaType::IMAGE_SEQUENCE: return 10.0; // Default 10 seconds, will be calculated based on frame count and rate
        case MediaType::EXR_SEQUENCE: return 10.0; // Default 10 seconds, will be calculated based on frame count and rate
        default: return 30.0;
        }
    }

    std::vector<std::string> ProjectManager::ParsePayloadString(const std::string& payload_string) {
        std::vector<std::string> media_ids;
        size_t pos = 0;
        std::string payload_copy = payload_string;

        while (pos < payload_copy.length()) {
            size_t next_pos = payload_copy.find(';', pos);
            if (next_pos == std::string::npos) {
                std::string media_id = payload_copy.substr(pos);
                if (!media_id.empty()) {
                    media_ids.push_back(media_id);
                }
                break;
            }
            else {
                std::string media_id = payload_copy.substr(pos, next_pos - pos);
                if (!media_id.empty()) {
                    media_ids.push_back(media_id);
                }
                pos = next_pos + 1;
            }
        }
        return media_ids;
    }

    bool ProjectManager::HasColorInfo(const VideoMetadata* video_meta) {
        return (!video_meta->colorspace.empty() && video_meta->colorspace != "Unknown") ||
            (!video_meta->color_primaries.empty() && video_meta->color_primaries != "Unknown") ||
            (!video_meta->color_transfer.empty() && video_meta->color_transfer != "Unknown");
    }

    bool ProjectManager::HasAudioInfo(const VideoMetadata* video_meta) {
        return (!video_meta->audio_codec.empty() && video_meta->audio_codec != "Unknown") ||
            video_meta->audio_sample_rate > 0 || video_meta->audio_channels > 0;
    }

    bool ProjectManager::IsAudioOnlyFile(const VideoMetadata* video_meta) {
        // Audio-only file: has audio info but no video dimensions
        return HasAudioInfo(video_meta) && (video_meta->width == 0 || video_meta->height == 0);
    }

    void ProjectManager::OpenFileInExplorer(const std::string& file_path) {
        // Delegate to the main ShowInExplorer implementation (non-blocking + handles special URLs)
        ShowInExplorer(file_path);
    }

    void ProjectManager::CopyToClipboard(const std::string& text) {
        ImGui::SetClipboardText(text.c_str());
    }

    // ============================================================================
    // T
    // ============================================================================

    std::string ProjectManager::GenerateEDLFromSequence(const ump::Sequence& sequence) {
        // This method duplicates the logic in RebuildPlaylistInMPV and LoadSequenceIntoPlayer
        // Consider removing and using the simplified playlist approach instead
        if (sequence.clips.empty()) return "";

        std::string edl;
        auto sorted_clips = sequence.GetAllClipsSorted();
        for (const auto& clip : sorted_clips) {
            edl += clip.file_path + "\n";
        }
        return edl;
    }

    // ============================================================================
    // VIDEO CACHE MANAGEMENT (ProjectManager interface)
    // ============================================================================

    FrameCache* ProjectManager::GetCurrentVideoCache() const {
        if (!video_cache_manager || !current_file_path || current_file_path->empty()) {
            return nullptr;
        }

        // Skip cache for audio files (no video frames to cache)
        MediaType media_type = GetMediaType(*current_file_path);
        if (media_type == MediaType::AUDIO) {
            return nullptr;
        }

        return video_cache_manager->GetCacheForVideo(*current_file_path);
    }

    bool ProjectManager::GetCachedFrame(double timestamp, GLuint& texture_id, int& width, int& height) {
        if (!cache_enabled || !video_cache_manager || !current_file_path || current_file_path->empty()) {
            return false;
        }
        return video_cache_manager->GetCachedFrame(*current_file_path, timestamp, texture_id, width, height);
    }

    void ProjectManager::NotifyVideoChanged(const std::string& video_path) {
        // Skip FFMPEG-based FrameCache for image sequences - they use DirectEXRCache only
        if (video_path.substr(0, 6) == "exr://") {
            Debug::Log("ProjectManager: Skipping FFMPEG cache for EXR sequence (uses DirectEXRCache)");
            return;
        }

        // Skip FFMPEG-based FrameCache for TIFF/PNG/JPEG image sequences - they use DirectEXRCache
        if (video_path.substr(0, 5) == "mf://") {
            Debug::Log("ProjectManager: Skipping FFMPEG cache for TIFF/PNG/JPEG image sequence (uses DirectEXRCache with universal loaders)");
            return;
        }

        // Skip frame cache for audio-only files (no video frames to cache)
        MediaType media_type = GetMediaType(video_path);
        if (media_type == MediaType::AUDIO) {
            Debug::Log("ProjectManager: Skipping FrameCache for audio file (no video frames)");
            return;
        }

        // Re-enable cache if it was auto-disabled for previous video's codec
        // BUT first check if we already have metadata for the new video to avoid re-enabling for H.264/H.265
        if (cache_auto_disabled_for_codec) {
            Debug::Log("NotifyVideoChanged: Cache was previously auto-disabled, checking new video: " + video_path);
            const CombinedMetadata* cached_meta = GetCachedMetadata(video_path);

            if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                // We have metadata - check codec before re-enabling
                std::string codec = cached_meta->video_meta->video_codec;
                Debug::Log("NotifyVideoChanged: Found cached metadata, codec = '" + codec + "'");
                std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);

                // Skip H.264 check for image sequences - they always use DirectEXRCache
                bool is_image_sequence = (video_path.substr(0, 5) == "mf://") ||
                                         (video_path.substr(0, 6) == "exr://");

                bool is_h264_h265 = !is_image_sequence && (codec.find("h264") != std::string::npos ||
                                     codec.find("hevc") != std::string::npos ||
                                     codec.find("h265") != std::string::npos ||
                                     codec.find("avc") != std::string::npos);

                if (is_h264_h265) {
                    Debug::Log("=== CACHED H.264/H.265 DETECTED ===");
                    Debug::Log("Codec: " + codec);
                    Debug::Log("Cache remains disabled (not re-enabling for H.264/H.265)");
                    current_video_codec = cached_meta->video_meta->video_codec;
                    // Keep cache_auto_disabled_for_codec = true
                } else {
                    Debug::Log("=== NEW MEDIA LOADED ===");
                    Debug::Log("Previous video codec: " + current_video_codec);
                    Debug::Log("New codec: " + codec);
                    Debug::Log("Re-enabling cache for non-H.264/H.265 media");
                    SetCacheEnabled(true);
                    cache_auto_disabled_for_codec = false;
                    current_video_codec = "";
                }
            } else {
                // No cached metadata - re-enable and check when metadata arrives
                Debug::Log("=== NEW MEDIA LOADED ===");
                Debug::Log("Previous video codec: " + current_video_codec);
                Debug::Log("Re-enabling cache for new media (will check codec when metadata arrives)");
                SetCacheEnabled(true);
                cache_auto_disabled_for_codec = false;
                current_video_codec = "";
            }
        }

        if (cache_enabled && video_cache_manager) {
            // Image sequences (mf://) are now handled by early return above
            // Only regular videos reach this point
            video_cache_manager->NotifyVideoChanged(video_path, video_player);

            // CACHE INITIALIZATION FIX: If we already have cached metadata for this video,
            // check codec and potentially disable cache before starting
            const CombinedMetadata* cached_meta = GetCachedMetadata(video_path);
            if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                Debug::Log("ProjectManager: Found existing metadata for " + video_path + ", checking codec");

                // Check if this is H.264/H.265 and disable cache if needed
                std::string codec = cached_meta->video_meta->video_codec;
                std::string codec_lower = codec;
                std::transform(codec_lower.begin(), codec_lower.end(), codec_lower.begin(), ::tolower);
                current_video_codec = codec;

                // Skip H.264 detection for image sequences - they use dummy H.264 videos for timing
                bool is_image_sequence = (video_path.substr(0, 5) == "mf://") ||
                                         (video_path.substr(0, 6) == "exr://");

                if (!is_image_sequence && (codec_lower.find("h264") != std::string::npos ||
                    codec_lower.find("hevc") != std::string::npos ||
                    codec_lower.find("h265") != std::string::npos ||
                    codec_lower.find("avc") != std::string::npos)) {

                    Debug::Log("=== H.264/H.265 DETECTED (from cached metadata) ===");
                    Debug::Log("Codec: " + current_video_codec);
                    Debug::Log("Disabling cache for B-frame safety");

                    SetCacheEnabled(false);
                    ClearAllCaches();  // Clear ALL caches, not just current
                    cache_auto_disabled_for_codec = true;

                    Debug::Log("Cache disabled and ALL caches cleared for: " + video_path);
                } else {
                    // Safe codec - apply metadata to cache system
                    Debug::Log("ProjectManager: Safe codec detected, applying metadata to cache system");
                    video_cache_manager->UpdateVideoMetadata(video_path, *cached_meta->video_meta);
                }
            }
        }
    }

    void ProjectManager::SetCacheConfig(const FrameCache::CacheConfig& config) {
        if (video_cache_manager) {
            video_cache_manager->SetCacheConfig(config);
        }
    }


    void ProjectManager::RemoveVideoFromCache(const std::string& video_path) {
        if (video_cache_manager) {
            video_cache_manager->RemoveCacheForVideo(video_path);
        }
    }

    void ProjectManager::ClearAllCaches() {
        if (video_cache_manager) {
            // Get all cached video paths and remove each one
            std::vector<std::string> cached_paths = video_cache_manager->GetAllCachedVideoPaths();

            // Remove all caches
            for (const auto& path : cached_paths) {
                video_cache_manager->RemoveCacheForVideo(path);
            }
        }

        // Clear EXR cache if active (BUT NOT for image sequences!)
        // Image sequences use DirectEXRCache as their PRIMARY frame source
        // Clearing it would break viewport rendering completely
        if (video_player && current_file_path) {
            bool is_image_sequence = (current_file_path->substr(0, 5) == "mf://") ||
                                     (current_file_path->substr(0, 6) == "exr://");

            if (!is_image_sequence) {
                video_player->ClearEXRCache();
                Debug::Log("ProjectManager: Cleared EXR cache");
            } else {
                Debug::Log("ProjectManager: Skipping EXR cache clear (image sequence uses it for frames)");
            }
        }
    }

    // ClearCurrentVideoCache() removed - use ClearAllCaches() instead
    // State management only allows one video cached, so "current" vs "all" is redundant

    void ProjectManager::RestartCache() {
        if (!video_cache_manager || !current_file_path || current_file_path->empty()) {
            Debug::Log("ProjectManager: Cannot restart cache - no video loaded or cache manager unavailable");
            return;
        }

        Debug::Log("ProjectManager: Performing cache restart for: " + *current_file_path);

        // Enable caching
        cache_enabled = true;
        video_cache_manager->SetCachingEnabled(true);

        // Check if cache exists AND is initialized
        // GetCacheForVideo() creates empty cache if it doesn't exist, so we need to check initialization
        FrameCache* cache = video_cache_manager->GetCacheForVideo(*current_file_path);
        bool cache_needs_init = (!cache || !cache->IsInitialized());

        if (cache_needs_init) {
            Debug::Log("ProjectManager: Cache not initialized (was destroyed by Clear Cache or new), initializing...");

            // Initialize cache by calling NotifyVideoChanged (same as initial load)
            // This will also set current_video_path and call SetVideoFile()
            video_cache_manager->NotifyVideoChanged(*current_file_path, video_player);

            // Apply metadata if we have it cached
            const CombinedMetadata* cached_meta = GetCachedMetadata(*current_file_path);
            if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                Debug::Log("ProjectManager: Applying cached metadata to initialized cache");
                video_cache_manager->UpdateVideoMetadata(*current_file_path, *cached_meta->video_meta);
            }
        } else {
            // Cache exists and is initialized - just clear and restart
            Debug::Log("ProjectManager: Cache initialized, clearing and restarting extraction");
            video_cache_manager->SetCurrentVideo(*current_file_path);
            video_cache_manager->RestartCurrentVideoCache();
        }

        // Restart EXR cache if active
        if (video_player && video_player->HasEXRCache()) {
            Debug::Log("ProjectManager: Restarting EXR cache");
            const auto& exr_files = video_player->GetEXRSequenceFiles();
            std::string exr_layer = video_player->GetEXRLayerName();
            double exr_fps = video_player->GetEXRFrameRate();

            if (!exr_files.empty()) {
                video_player->ClearEXRCache();
                video_player->InitializeEXRCache(exr_files, exr_layer, exr_fps);
                Debug::Log("ProjectManager: EXR cache restarted with " + std::to_string(exr_files.size()) + " frames");
            }
        }

        Debug::Log("ProjectManager: Cache restart completed");
    }

    void ProjectManager::SetCacheEnabled(bool enabled) {
        cache_enabled = enabled;

        // EXR PATTERN: Just set the flag, threads keep running
        if (video_cache_manager) {
            video_cache_manager->SetCachingEnabled(enabled);
        }

        // Control EXR cache
        if (video_player) {
            video_player->SetEXRCacheEnabled(enabled);
            Debug::Log("ProjectManager: Cache " + std::string(enabled ? "enabled" : "disabled") + " (EXR pattern - threads still running)");
        }
    }

    bool ProjectManager::IsCacheEnabled() const {
        return cache_enabled;
    }

    FrameCache::CacheStats ProjectManager::GetCacheStats() const {
        if (video_cache_manager) {
            return video_cache_manager->GetTotalStats();
        }
        return FrameCache::CacheStats{};
    }

    std::vector<FrameCache::CacheSegment> ProjectManager::GetCacheSegments() const {
        if (!current_file_path || current_file_path->empty()) {
            return std::vector<FrameCache::CacheSegment>();
        }

        // Check if current video player is in EXR mode
        if (video_player && video_player->IsInEXRMode()) {
            // Use EXR cache segments instead of regular video cache (convert types)
            auto exr_segments = video_player->GetEXRCacheSegments();
            std::vector<FrameCache::CacheSegment> result;
            result.reserve(exr_segments.size());
            for (const auto& seg : exr_segments) {
                FrameCache::CacheSegment fc_seg;
                fc_seg.start_time = seg.start_time;
                fc_seg.end_time = seg.end_time;
                fc_seg.density = seg.density;
                fc_seg.type = FrameCache::CacheSegment::SCRUB_CACHE;
                result.push_back(fc_seg);
            }
            return result;
        }

        // Original regular video cache logic
        if (video_cache_manager) {
            return video_cache_manager->GetCacheSegments(*current_file_path);
        }

        return std::vector<FrameCache::CacheSegment>();
    }

    void ProjectManager::NotifyPlaybackState(bool is_playing) {
        if (video_cache_manager) {
            video_cache_manager->NotifyPlaybackState(is_playing);
        }
    }

    // Note: TryOpportunisticCaching() removed - using window-based extraction only


    // ============================================================================
    // VIDEO CACHE IMPLEMENTATION
    // ============================================================================

    VideoCache::VideoCache() {
        // Initialize default config for seconds-based cache management
        default_config.max_cache_seconds = 20; // 20 second default cache window
        default_config.use_centered_caching = true; // Center around seekbar by default
        default_config.cache_width = 1920;
        default_config.cache_height = -1; // Auto-calculate aspect ratio
    }

    VideoCache::~VideoCache() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        video_caches.clear(); // This will destroy all FrameCache instances
    }

    FrameCache* VideoCache::GetCacheForVideo(const std::string& video_path) {
        if (video_path.empty()) return nullptr;

        // Skip FFMPEG-based FrameCache for EXR sequences - they use DirectEXRCache only
        if (video_path.substr(0, 6) == "exr://") {
            return nullptr;
        }

        // Skip cache for audio files (no video frames to cache)
        size_t dot_pos = video_path.find_last_of('.');
        if (dot_pos != std::string::npos) {
            std::string ext = video_path.substr(dot_pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".mp3" || ext == ".aac" ||
                ext == ".flac" || ext == ".ogg" || ext == ".wma" || ext == ".m4a") {
                return nullptr;
            }
        }

        {
            std::lock_guard<std::mutex> lock(cache_mutex);

            auto it = video_caches.find(video_path);
            if (it != video_caches.end()) {
                UpdateLRUOrder(video_path);
                it->second->last_accessed = std::chrono::steady_clock::now();
                return it->second->cache.get();
            }
        }

        // Create new cache for this video OUTSIDE the lock to avoid deadlocks
        auto new_cache = std::make_unique<FrameCache>(default_config);

        // Cache eviction now handled by SetCurrentVideo() when switching videos
        FrameCache* cache_ptr = new_cache.get();

        {
            std::lock_guard<std::mutex> lock(cache_mutex);

            // Double-check that another thread didn't create the cache while we were unlocked
            auto it = video_caches.find(video_path);
            if (it != video_caches.end()) {
                // Another thread created it, return that one and discard ours
                return it->second->cache.get();
            }

            // Store our newly created cache
            auto cache_entry = std::make_unique<VideoCacheEntry>(std::move(new_cache), video_path);
            cache_ptr = cache_entry->cache.get(); // Update pointer after move

            video_caches[video_path] = std::move(cache_entry);
            lru_order.push_front(video_path);

            // Cache eviction now handled by SetCurrentVideo() when switching videos
        }

        return cache_ptr;
    }

    void VideoCache::SetCurrentVideo(const std::string& video_path) {
        std::lock_guard<std::mutex> lock(cache_mutex);

        // Clear cache of previous video to free RAM when switching
        if (!current_video_path.empty() && current_video_path != video_path) {
            Debug::Log("VideoCache: Clearing cache for previous video: " + current_video_path);
            auto it = video_caches.find(current_video_path);
            if (it != video_caches.end()) {
                // Remove from LRU order
                lru_order.remove(current_video_path);
                // Remove from cache map (this will destroy the FrameCache automatically)
                video_caches.erase(it);
            }
        }

        current_video_path = video_path;

        // Update access time for current video
        if (!video_path.empty()) {
            UpdateLRUOrder(video_path);
        }
    }

    bool VideoCache::GetCachedFrame(const std::string& video_path, double timestamp, GLuint& texture_id, int& width, int& height) {
        FrameCache* cache = GetCacheForVideo(video_path);
        if (!cache) return false;
        
        return cache->GetCachedFrame(timestamp, texture_id, width, height);
    }

    void VideoCache::NotifyVideoChanged(const std::string& video_path, VideoPlayer* video_player) {
        SetCurrentVideo(video_path);

        FrameCache* cache = GetCacheForVideo(video_path);
        if (cache && caching_enabled) {
            // Initialize cache with video file - metadata will be passed later via UpdateVideoMetadata
            cache->SetVideoFile(video_path, nullptr);

            // Background caching will now be started by the FrameCache after metadata is available
            // This ensures proper sequencing and eliminates race conditions

            if (video_player) {
                // CRITICAL: Immediately notify cache of current playback state to avoid conflicts
                bool is_playing = video_player->IsPlaying();
                cache->NotifyPlaybackState(is_playing);

                cache->UpdateScrubPosition(video_player->GetPosition(), video_player);
            }

            // Previous video cache is now cleared automatically in SetCurrentVideo()
            // Individual video caches use seconds-based eviction around playhead
        }
    }

    // Note: TryOpportunisticCaching() removed - using window-based extraction only

    void VideoCache::NotifyPlaybackState(bool is_playing) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        // Notify all caches, but focus on current video
        for (auto& pair : video_caches) {
            pair.second->cache->NotifyPlaybackState(is_playing);
        }
    }


    void VideoCache::SetCacheConfig(const FrameCache::CacheConfig& config) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        default_config = config;

        // Apply to all existing caches
        for (auto& pair : video_caches) {
            pair.second->cache->SetCacheConfig(config);
        }
    }

    PipelineMode VideoCache::GetPipelineMode() const {
        std::lock_guard<std::mutex> lock(cache_mutex);

        // Get pipeline mode from current video cache
        if (!current_video_path.empty()) {
            auto it = video_caches.find(current_video_path);
            if (it != video_caches.end() && it->second->cache) {
                return it->second->cache->GetPipelineMode();
            }
        }

        // Fallback to default config
        return default_config.pipeline_mode;
    }

    FrameCache::CacheStats VideoCache::GetTotalStats() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        FrameCache::CacheStats total_stats;
        
        for (const auto& pair : video_caches) {
            auto stats = pair.second->cache->GetStats();
            total_stats.total_frames_cached += stats.total_frames_cached;
            // Removed: Memory usage aggregation (memory-based eviction removed)
            total_stats.cache_hits += stats.cache_hits;
            total_stats.cache_misses += stats.cache_misses;
        }
        
        if (total_stats.cache_hits + total_stats.cache_misses > 0) {
            total_stats.hit_ratio = static_cast<float>(total_stats.cache_hits) / 
                                   (total_stats.cache_hits + total_stats.cache_misses);
        }
        
        return total_stats;
    }

    FrameCache::CacheStats VideoCache::GetStatsForVideo(const std::string& video_path) const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = video_caches.find(video_path);
        if (it != video_caches.end()) {
            return it->second->cache->GetStats();
        }
        return FrameCache::CacheStats{};
    }

    std::vector<FrameCache::CacheSegment> VideoCache::GetCacheSegments(const std::string& video_path) const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = video_caches.find(video_path);
        if (it != video_caches.end()) {
            return it->second->cache->GetCacheSegments();
        }
        return std::vector<FrameCache::CacheSegment>();
    }


    void VideoCache::RemoveCacheForVideo(const std::string& video_path) {
        if (video_path.empty()) return;
        
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        auto it = video_caches.find(video_path);
        if (it != video_caches.end()) {
            // Remove from LRU order
            lru_order.remove(video_path);
            
            // Remove from cache map (this will destroy the FrameCache automatically)
            video_caches.erase(it);
            
            // Clear current video if it was the removed one
            if (current_video_path == video_path) {
                current_video_path.clear();
            }
        }
    }


    std::vector<std::string> VideoCache::GetAllCachedVideoPaths() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        std::vector<std::string> paths;
        for (const auto& entry : video_caches) {
            paths.push_back(entry.first);
        }
        return paths;
    }


    size_t VideoCache::GetCacheCount() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        return video_caches.size();
    }

    // Private methods
    void VideoCache::UpdateLRUOrder(const std::string& video_path) {
        // Remove from current position and add to front
        lru_order.remove(video_path);
        lru_order.push_front(video_path);
    }

    void VideoCache::EvictOldestCache() {
        if (lru_order.empty()) return;
        
        // Don't evict the current video
        std::string oldest_path;
        for (auto it = lru_order.rbegin(); it != lru_order.rend(); ++it) {
            if (*it != current_video_path) {
                oldest_path = *it;
                break;
            }
        }
        
        if (!oldest_path.empty()) {
            video_caches.erase(oldest_path);
            lru_order.remove(oldest_path);
        }
    }


    void VideoCache::SetCachingEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        caching_enabled = enabled;

        // Enable/disable caching for all video caches
        for (auto& pair : video_caches) {
            pair.second->cache->SetCachingEnabled(enabled);
        }
    }

    // ClearCurrentVideoCache() removed - use ClearAllCaches() instead

    void VideoCache::RestartCurrentVideoCache() {
        std::lock_guard<std::mutex> lock(cache_mutex);

        if (current_video_path.empty()) {
            Debug::Log("VideoCache: RestartCurrentVideoCache - current_video_path is EMPTY, cannot restart");
            return;
        }

        Debug::Log("VideoCache: Restarting cache for: " + current_video_path);

        auto it = video_caches.find(current_video_path);
        if (it != video_caches.end()) {
            // EXR PATTERN: Just clear frames, thread keeps running
            Debug::Log("VideoCache: Found cache, calling ClearCachedFrames()...");
            it->second->cache->ClearCachedFrames();
            Debug::Log("VideoCache: ClearCachedFrames() returned");
        } else {
            Debug::Log("VideoCache: Cache NOT found in video_caches map for: " + current_video_path);
        }
    }

    bool VideoCache::IsCachingEnabled() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        return caching_enabled;
    }

    void VideoCache::UpdateVideoMetadata(const std::string& video_path, const VideoMetadata& metadata) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = video_caches.find(video_path);
        if (it != video_caches.end() && it->second->cache) {
            it->second->cache->UpdateVideoMetadata(video_path, metadata);
            // Background extraction will be started by FrameCache::UpdateVideoMetadata() after metadata is applied
        }
    }


    // ============================================================================
    // IMAGE SEQUENCE DETECTION
    // ============================================================================

    bool ProjectManager::IsPartOfImageSequence(const std::string& file_path) const {
        try {
            std::filesystem::path path(file_path);
            std::string filename = path.stem().string(); // filename without extension
            std::string directory = path.parent_path().string();
            std::string extension = path.extension().string();

            // Parse from the end backward - look for separator + digits at the end
            // Patterns: file.000012, file_0014, file-0000000000014
            std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
            std::smatch match;

            if (!std::regex_match(filename, match, pattern)) {
                // Try pattern without separator (rare case: file000012)
                std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)"); // Require 3+ digits to avoid false positives
                if (!std::regex_match(filename, match, no_sep_pattern)) {
                    return false; // Doesn't look like a sequence
                }
            }

            std::string base_name = match[1].str();
            std::string separator = (match.size() > 3) ? match[2].str() : ""; // Separator if exists
            std::string number_str = (match.size() > 3) ? match[3].str() : match[2].str();

            // Create pattern to match similar files with same separator and digit length
            std::string search_pattern;
            if (!separator.empty()) {
                // With separator: base + separator + digits
                search_pattern = base_name + R"([_\.\-]\d{)" + std::to_string(number_str.length()) + R"(})";
            } else {
                // Without separator: base + digits (be more specific to avoid false matches)
                search_pattern = base_name + R"(\d{)" + std::to_string(number_str.length()) + R"(})";
            }
            std::regex sequence_pattern(search_pattern);

            int count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (!entry.is_regular_file()) continue;

                std::filesystem::path other_path = entry.path();
                if (other_path.extension() != extension) continue;

                std::string other_filename = other_path.stem().string();
                if (std::regex_match(other_filename, sequence_pattern)) {
                    count++;
                    if (count >= 2) { // Found at least 2 files with the pattern
                        return true;
                    }
                }
            }

            return false; // Not enough files found
        } catch (...) {
            return false; // Any error means not a sequence
        }
    }

    std::vector<std::string> ProjectManager::DetectImageSequence(const std::string& file_path) {
        try {
            std::filesystem::path path(file_path);
            std::string filename = path.stem().string();
            std::string directory = path.parent_path().string();
            std::string extension = path.extension().string();

            // Use same improved pattern as IsPartOfImageSequence
            std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
            std::smatch match;

            if (!std::regex_match(filename, match, pattern)) {
                // Try pattern without separator (rare case: file000012)
                std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)"); // Require 3+ digits to avoid false positives
                if (!std::regex_match(filename, match, no_sep_pattern)) {
                    return {}; // No valid pattern
                }
            }

            std::string base_name = match[1].str();
            std::string separator = (match.size() > 3) ? match[2].str() : ""; // Separator if exists
            std::string number_str = (match.size() > 3) ? match[3].str() : match[2].str();

            // Create pattern to match similar files with same separator and digit length
            std::string search_pattern;
            if (!separator.empty()) {
                // With separator: base + separator + digits
                search_pattern = base_name + R"([_\.\-]\d{)" + std::to_string(number_str.length()) + R"(})";
            } else {
                // Without separator: base + digits (be more specific to avoid false matches)
                search_pattern = base_name + R"(\d{)" + std::to_string(number_str.length()) + R"(})";
            }
            std::regex sequence_pattern(search_pattern);

            // Collect all matching files
            std::vector<std::string> sequence_files;

            // Use error_code to avoid exceptions from directory_iterator (cloud placeholders may cause issues)
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
                if (ec) {
                    // Error during iteration - may be cloud sync issue
                    Debug::Log("DetectImageSequence: Directory iteration error - " + ec.message());
                    continue;  // Try to continue with what we have
                }

                // Check if entry exists and is accessible (cloud placeholders may fail this)
                std::error_code entry_ec;
                if (!entry.exists(entry_ec) || entry_ec) {
                    continue;  // Skip files that don't exist or can't be accessed
                }

                if (!entry.is_regular_file(entry_ec) || entry_ec) {
                    continue;  // Skip non-regular files or inaccessible files
                }

                std::filesystem::path other_path = entry.path();
                if (other_path.extension() != extension) continue;

                std::string other_filename = other_path.stem().string();
                if (std::regex_match(other_filename, sequence_pattern)) {
                    sequence_files.push_back(other_path.string());
                }
            }

            // CRITICAL: Sort files alphabetically so frame indices match file order
            // directory_iterator does NOT guarantee order!
            std::sort(sequence_files.begin(), sequence_files.end());

            return sequence_files;
        } catch (const std::exception& e) {
            Debug::Log("DetectImageSequence: Exception - " + std::string(e.what()));
            return {}; // Any error returns empty vector
        } catch (...) {
            Debug::Log("DetectImageSequence: Unknown exception");
            return {}; // Any error returns empty vector
        }
    }

    void ProjectManager::ShowFrameRateDialog(const std::string& sequence_path) {
        Debug::Log("ShowFrameRateDialog called with: " + sequence_path);
        pending_sequence_path = sequence_path;

        // Check if this is an EXR sequence and detect layers
        is_exr_sequence = false;
        is_tiff_png_sequence = false;
        {
            std::lock_guard<std::mutex> lock(exr_layers_mutex);
            exr_layer_names.clear();
            exr_layer_display_names.clear();
            selected_exr_layer_index = 0;
        }
        hidden_cryptomatte_count = 0;

        std::filesystem::path path(sequence_path);
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        if (extension == ".exr") {
            Debug::Log("EXR sequence detected, analyzing layers asynchronously...");
            is_exr_sequence = true;

            // Show dialog immediately with "Detecting layers..." placeholder
            {
                std::lock_guard<std::mutex> lock(exr_layers_mutex);
                exr_layer_names.clear();
                exr_layer_display_names.clear();
                exr_layer_names.push_back("Detecting layers...");
                exr_layer_display_names.push_back("Detecting layers...");
                selected_exr_layer_index = 0;
            }

            // Launch async layer detection
            std::string path_copy = sequence_path;
            std::thread([this, path_copy]() {
                EXRLayerDetector detector;
                std::vector<EXRLayer> layers;
                int crypto_count = 0;

                // Get first file from sequence
                std::vector<std::string> exr_files = DetectImageSequence(path_copy);
                if (exr_files.empty()) {
                    std::lock_guard<std::mutex> lock(exr_layers_mutex);
                    exr_layer_names.clear();
                    exr_layer_display_names.clear();
                    exr_layer_names.push_back("RGBA");
                    exr_layer_display_names.push_back("RGBA (default)");
                    selected_exr_layer_index = 0;
                    Debug::Log("EXR Layer Detection: No sequence files found");
                    return;
                }

                if (detector.DetectLayers(exr_files[0], layers, crypto_count)) {
                    // Update UI data with mutex protection
                    std::lock_guard<std::mutex> lock(exr_layers_mutex);
                    exr_layer_names.clear();
                    exr_layer_display_names.clear();
                    hidden_cryptomatte_count = crypto_count;

                    if (!layers.empty()) {
                        for (const EXRLayer& layer : layers) {
                            exr_layer_names.push_back(layer.name);
                            exr_layer_display_names.push_back(layer.name);

                            if (layer.is_default) {
                                selected_exr_layer_index = exr_layer_names.size() - 1;
                            }
                        }
                        Debug::Log("Found " + std::to_string(layers.size()) + " EXR layers");
                    } else {
                        exr_layer_names.push_back("RGBA");
                        exr_layer_display_names.push_back("RGBA");
                        selected_exr_layer_index = 0;
                    }
                } else {
                    // Fallback to default
                    std::lock_guard<std::mutex> lock(exr_layers_mutex);
                    exr_layer_names.clear();
                    exr_layer_display_names.clear();
                    exr_layer_names.push_back("RGBA");
                    exr_layer_display_names.push_back("RGBA (default)");
                    selected_exr_layer_index = 0;
                    Debug::Log("EXR Layer Detection: Failed, using default RGBA");
                }
            }).detach();
        } else if (extension == ".tif" || extension == ".tiff" || extension == ".png") {
            // TIFF/PNG sequence detected
            Debug::Log("TIFF/PNG sequence detected, checking for transcode eligibility...");
            is_tiff_png_sequence = true;

            // Detect if this is a 16-bit or large resolution sequence that would benefit from transcode
            // For now, we'll show the option for all TIFF/PNG sequences
            // The user can choose whether to transcode based on their needs
        }

        show_frame_rate_dialog = true;
        frame_rate_dialog_opened = false; // Reset flag so popup can open
        Debug::Log("Frame rate dialog state set successfully");
    }

    void ProjectManager::ProcessImageSequence(const std::string& sequence_path, double frame_rate, const std::string& exr_layer) {
        Debug::Log("ProcessImageSequence: Step 1 - Detecting sequence files");

        // Get the full sequence file list
        std::vector<std::string> sequence_files = DetectImageSequence(sequence_path);
        if (sequence_files.empty()) {
            Debug::Log("ProcessImageSequence: No sequence files found");
            return;
        }

        Debug::Log("ProcessImageSequence: Step 2 - Found " + std::to_string(sequence_files.size()) + " files");

        // Extract sequence information from first and last files
        std::filesystem::path first_file(sequence_files[0]);
        std::filesystem::path last_file(sequence_files.back());

        Debug::Log("ProcessImageSequence: Step 3 - Extracted file paths");

        // Parse the first file to understand the naming pattern
        std::string first_filename = first_file.stem().string();
        Debug::Log("ProcessImageSequence: Step 4 - Parsing filename: " + first_filename);

        // Use same improved pattern as IsPartOfImageSequence and DetectImageSequence
        std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
        std::smatch match;

        if (!std::regex_match(first_filename, match, pattern)) {
            // Try pattern without separator (rare case: file000012)
            std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)"); // Require 3+ digits to avoid false positives
            if (!std::regex_match(first_filename, match, no_sep_pattern)) {
                Debug::Log("ProcessImageSequence: Failed to match pattern");
                return;
            }
        }

        std::string base_name = match[1].str();
        std::string separator = (match.size() > 3) ? match[2].str() : ""; // Separator if exists
        std::string first_number = (match.size() > 3) ? match[3].str() : match[2].str();
        Debug::Log("ProcessImageSequence: Step 5 - Parsed base_name=" + base_name + " separator='" + separator + "' number=" + first_number);

        // Parse last file to get the end frame using same logic
        std::string last_filename = last_file.stem().string();
        std::smatch last_match;
        std::string last_number = first_number; // Default to first if parsing fails

        if (std::regex_match(last_filename, last_match, pattern)) {
            last_number = (last_match.size() > 3) ? last_match[3].str() : last_match[2].str();
        } else {
            std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)");
            if (std::regex_match(last_filename, last_match, no_sep_pattern)) {
                last_number = last_match[2].str();
            }
        }

        int start_frame = std::stoi(first_number);
        int end_frame = std::stoi(last_number);
        Debug::Log("ProcessImageSequence: Step 6 - Frame range: " + std::to_string(start_frame) + " to " + std::to_string(end_frame));

        // Create a more specific MPV pattern
        // MPV supports patterns like: mf://path/sequence_%04d.exr:fps=24
        std::string directory = first_file.parent_path().string();
        std::string extension = first_file.extension().string();
        Debug::Log("ProcessImageSequence: Step 7 - Directory: " + directory);

        // Determine the padding for the sequence
        int padding = static_cast<int>(first_number.length());

        // Create pattern: base_name + separator + %0Xd + extension (where X is padding)
        std::string mf_pattern = base_name + separator + "%0" + std::to_string(padding) + "d" + extension;
        Debug::Log("ProcessImageSequence: Step 8 - MF pattern: " + mf_pattern);

        // Replace backslashes with forward slashes for MPV (cross-platform compatibility)
        std::replace(directory.begin(), directory.end(), '\\', '/');

        // Create MPV MF:// URL using the working C# pattern
        // For complex names like "20250920_1809--ACES2065-1_TIFF", we need to remove the separator + digits
        std::string file_basename = base_name;
        if (!separator.empty()) {
            // Remove the separator from the end if it exists
            if (!file_basename.empty() && file_basename.back() == separator[0]) {
                file_basename.pop_back();
            }
        }

        // C# working format: mf://directory/basename*extension (no fps in URL)
        std::string mf_url = "mf://" + directory + "/" + file_basename + "*" + extension;
        Debug::Log("ProcessImageSequence: Using C# working pattern: " + mf_url);
        Debug::Log("ProcessImageSequence: File basename after regex: '" + file_basename + "'");

        // Alternative: Try first file path to test basic loading
        std::string first_file_path = sequence_files[0];
        Debug::Log("ProcessImageSequence: Step 9b - First file for testing: " + first_file_path);

        // For non-standard start frames, we can add start parameter if MPV supports it
        // For now, we'll let MPV handle the sequence as-is

        // Determine if this is an EXR sequence
        std::filesystem::path seq_path(sequence_path);
        std::string ext = seq_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        bool is_exr = (ext == ".exr");
        bool is_transcoded_exr = is_exr && exr_layer.empty();  // Transcoded single-layer EXRs have empty layer

        // Create MediaItem for the sequence
        MediaItem item;
        item.id = GenerateUniqueID();

        if (is_exr && !exr_layer.empty()) {
            // EXR sequence with layer selection (original multi-layer)
            item.name = base_name + " [" + std::to_string(sequence_files.size()) + " frames " +
                       std::to_string(start_frame) + "-" + std::to_string(end_frame) + "] - " + exr_layer;
            item.type = MediaType::EXR_SEQUENCE;
            item.exr_layer = exr_layer;

            // Find display name for layer
            {
                std::lock_guard<std::mutex> lock(exr_layers_mutex);
                for (int i = 0; i < exr_layer_names.size(); i++) {
                    if (exr_layer_names[i] == exr_layer) {
                        item.exr_layer_display = exr_layer_display_names[i];
                        break;
                    }
                }
            }

            // For EXR sequences, we'll use the same path format as current_file_path for consistency
            item.path = "exr://" + sequence_path + "?layer=" + exr_layer;
        } else if (is_transcoded_exr) {
            // Transcoded single-layer EXR (treat as EXR, not regular image sequence)
            item.name = base_name + " [" + std::to_string(sequence_files.size()) + " frames " +
                       std::to_string(start_frame) + "-" + std::to_string(end_frame) + "] - Transcoded";
            item.type = MediaType::EXR_SEQUENCE;
            item.exr_layer = "RGBA";  // Transcoded files are always single-layer RGBA
            item.exr_layer_display = "RGBA (Transcoded)";
            item.path = "exr://" + sequence_path + "?layer=RGBA";
        } else {
            // Regular image sequence (TIFF, PNG, JPEG, etc.)
            item.name = base_name + " [" + std::to_string(sequence_files.size()) + " frames " +
                       std::to_string(start_frame) + "-" + std::to_string(end_frame) + "]";
            item.path = mf_url;
            item.type = MediaType::IMAGE_SEQUENCE;
        }

        item.frame_count = static_cast<int>(sequence_files.size());
        item.start_frame = start_frame;
        item.end_frame = end_frame;
        item.frame_rate = frame_rate;
        item.duration = static_cast<double>(sequence_files.size()) / frame_rate;
        item.sequence_pattern = mf_pattern;

        // Auto-detect pipeline mode from first image file (for all image sequences)
        PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Default

        // === BRANCH A: FFMPEG CACHE PATH ===
        // For regular image sequences (not EXR), prepare FFMPEG cache configuration
        if (item.type == MediaType::IMAGE_SEQUENCE) {
            Debug::Log("ProcessImageSequence: Preparing FFMPEG cache configuration");

            // Auto-detect pipeline mode from first file
            if (!sequence_files.empty()) {
                ump::ImageInfo img_info;
                if (ump::GetImageInfo(sequence_files[0], img_info)) {
                    pipeline_mode = img_info.recommended_pipeline;
                    Debug::Log("ProcessImageSequence: Auto-detected pipeline mode: " +
                              std::string(PipelineModeToString(pipeline_mode)) +
                              " (" + std::to_string(img_info.bit_depth) + "-bit, " +
                              (img_info.is_float ? "float" : "int") + ")");
                } else {
                    // Fallback to safe default (8-bit Normal mode)
                    pipeline_mode = PipelineMode::NORMAL;
                    Debug::Log("ProcessImageSequence: Using safe fallback mode: NORMAL (8-bit)");
                }
            }

            // Parse sequence for FFMPEG using our robust pattern converter
            ump::ImageSequenceConfig ffmpeg_config =
                ump::ImageSequencePatternConverter::ParseSequence(sequence_files, frame_rate, pipeline_mode);

            if (ffmpeg_config.is_valid) {
                Debug::Log("ProcessImageSequence: FFMPEG pattern: " + ffmpeg_config.ffmpeg_pattern);
                Debug::Log("ProcessImageSequence: Pipeline mode: " + std::string(PipelineModeToString(pipeline_mode)));

                // Store the full FFmpeg pattern in MediaItem for later cache reconstruction
                item.ffmpeg_pattern = ffmpeg_config.ffmpeg_pattern;
            } else {
                Debug::Log("ProcessImageSequence: Warning - FFMPEG pattern parsing failed");
            }
        }

        // Add to project
        media_pool.push_back(item);
        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > bin_index) {
            bins[bin_index].items.push_back(item);
        }

        // Load the sequence immediately
        current_sequence_id.clear();
        UpdateSequenceActiveStates("");

        // Clear previous selection and select this new item
        ClearSelection();
        SelectMediaItem(item.id, false, false);

        if (video_player) {
            Debug::Log("ProcessImageSequence: Step 10 - Loading in video player");

            // === BRANCH B: MPV PLAYBACK PATH ===

            // Check if this is an EXR sequence requiring custom processing
            // Note: Both multi-layer (with layer name) and transcoded (empty layer) use EXR pipeline
            if (item.type == MediaType::EXR_SEQUENCE) {
                Debug::Log("EXR sequence detected - using EXR pipeline with DirectEXRCache");
                Debug::Log("EXR layer: " + (item.exr_layer.empty() ? "(single-layer/transcoded)" : item.exr_layer));

                // Use EXR hybrid dummy + overlay approach (works for both multi-layer and transcoded)
                if (video_player->LoadEXRSequenceWithDummy(sequence_files, item.exr_layer, frame_rate)) {
                    *current_file_path = "exr://" + sequence_path + "?layer=" + item.exr_layer;

                    if (video_change_callback) {
                        video_change_callback(*current_file_path);
                    }

                    Debug::Log("EXR sequence loaded successfully");
                } else {
                    Debug::Log("ERROR: Failed to load EXR sequence");
                }
            } else {
                Debug::Log("Regular image sequence - using DirectEXRCache with universal loader");
                Debug::Log("Pipeline mode: " + std::string(PipelineModeToString(pipeline_mode)));

                // Load image sequence through DirectEXRCache with universal loader
                if (video_player->LoadImageSequenceWithCache(sequence_files, frame_rate, pipeline_mode)) {
                    // Store start frame for display purposes
                    video_player->SetImageSequenceFrameRate(frame_rate, start_frame);

                    *current_file_path = mf_url;  // Keep mf:// URL for identification

                    if (video_change_callback) {
                        video_change_callback(mf_url);
                    }

                    Debug::Log("Image sequence loaded successfully via DirectEXRCache");
                } else {
                    Debug::Log("ERROR: Failed to load image sequence with DirectEXRCache");
                }
            }

            // === SKIP VIDEO CACHE MANAGER FOR IMAGE SEQUENCES ===
            // Image sequences (both EXR and TIFF/PNG/JPEG) now use DirectEXRCache, not FrameCache
            // Only video files should go through FrameCache
            // EXR sequences are handled above via LoadEXRSequenceWithDummy
            // TIFF/PNG/JPEG sequences are handled above via LoadImageSequenceWithCache
        }

        Debug::Log("Processed image sequence: " + item.name + " (" + std::to_string(sequence_files.size()) +
                   " frames at " + std::to_string(frame_rate) + " fps)");
        Debug::Log("MPV URL: " + mf_url);
    }

    void ProjectManager::ProcessImageSequenceWithTranscode(const std::string& sequence_path, double frame_rate,
                                                           const std::string& exr_layer, int max_width, int compression) {
        Debug::Log("ProcessImageSequenceWithTranscode: Starting transcode workflow");

        // Get the full sequence file list
        std::vector<std::string> sequence_files = DetectImageSequence(sequence_path);
        if (sequence_files.empty()) {
            Debug::Log("ProcessImageSequenceWithTranscode: No sequence files found");
            return;
        }

        Debug::Log("ProcessImageSequenceWithTranscode: Found " + std::to_string(sequence_files.size()) + " source files");

        // Create transcoder
        static ump::EXRTranscoder transcoder;

        // Apply disk cache settings to transcoder
        transcoder.SetCacheConfig(g_custom_cache_path, g_cache_retention_days, g_transcode_cache_max_gb, g_clear_cache_on_exit);

        // Build transcode config
        ump::EXRTranscodeConfig config;
        config.max_width = max_width;
        config.compression = static_cast<Imf::Compression>(compression);
        config.threadCount = static_cast<size_t>(g_exr_transcode_threads);

        // Check if transcode already exists
        if (transcoder.HasTranscodedSequence(sequence_files, exr_layer, max_width, config.compression)) {
            Debug::Log("ProcessImageSequenceWithTranscode: Transcode already exists, loading directly");

            // Get transcoded files
            std::vector<std::string> transcoded_files = transcoder.GetTranscodedFiles(
                sequence_files, exr_layer, max_width, config.compression);

            if (!transcoded_files.empty()) {
                // Load transcoded sequence as single-layer EXR
                ProcessImageSequence(transcoded_files[0], frame_rate, "");  // Empty layer = single-layer
                return;
            }
        }

        // Start async transcode
        Debug::Log("ProcessImageSequenceWithTranscode: Starting async transcode...");

        // Show progress dialog
        extern bool show_transcode_progress;
        extern std::atomic<int> transcode_current_frame;
        extern std::atomic<int> transcode_total_frames;
        extern std::string transcode_status_message;
        extern std::mutex transcode_status_mutex;

        show_transcode_progress = true;
        transcode_current_frame = 0;
        transcode_total_frames = static_cast<int>(sequence_files.size());

        transcoder.TranscodeSequenceAsync(
            sequence_files,
            exr_layer,
            config,
            // Progress callback
            [](int current, int total, const std::string& message) {
                Debug::Log("Transcode progress: " + std::to_string(current) + "/" + std::to_string(total) + " - " + message);

                // Update UI progress (thread-safe)
                extern std::atomic<int> transcode_current_frame;
                extern std::atomic<int> transcode_total_frames;
                extern std::string transcode_status_message;
                extern std::mutex transcode_status_mutex;

                transcode_current_frame = current;
                transcode_total_frames = total;

                {
                    std::lock_guard<std::mutex> lock(transcode_status_mutex);
                    transcode_status_message = message;
                }
            },
            // Completion callback
            [this, sequence_files, exr_layer, max_width, compression, frame_rate](bool success, const std::string& error_message) {
                // Hide progress dialog
                extern bool show_transcode_progress;
                show_transcode_progress = false;

                if (success) {
                    Debug::Log("ProcessImageSequenceWithTranscode: Transcode complete!");

                    // Get transcoded files
                    static ump::EXRTranscoder local_transcoder;
                    Imf::Compression comp = static_cast<Imf::Compression>(compression);
                    std::vector<std::string> transcoded_files = local_transcoder.GetTranscodedFiles(
                        sequence_files, exr_layer, max_width, comp);

                    if (!transcoded_files.empty()) {
                        Debug::Log("ProcessImageSequenceWithTranscode: Loading transcoded sequence (" +
                                  std::to_string(transcoded_files.size()) + " frames)");

                        // Load transcoded sequence using EXR pipeline (empty layer = transcoded single-layer)
                        ProcessImageSequence(transcoded_files[0], frame_rate, "");  // Will auto-detect as transcoded EXR
                    } else {
                        Debug::Log("ProcessImageSequenceWithTranscode: ERROR - No transcoded files found after completion");
                        // TODO: Show error dialog
                    }
                } else {
                    Debug::Log("ProcessImageSequenceWithTranscode: ERROR - Transcode failed: " + error_message);
                    // TODO: Show error dialog with message: error_message
                }
            }
        );

        Debug::Log("ProcessImageSequenceWithTranscode: Async transcode initiated");
    }

    void ProjectManager::CancelTranscode() {
        // Access the static transcoder instance from ProcessImageSequenceWithTranscode
        static ump::EXRTranscoder transcoder;
        transcoder.CancelTranscode();
        Debug::Log("ProjectManager: Transcode cancellation requested");

        // Hide progress dialog
        extern bool show_transcode_progress;
        show_transcode_progress = false;
    }

    bool ProjectManager::IsInImageSequenceMode() const {
        return (current_file_path && current_file_path->find("mf://") == 0);
    }

    bool ProjectManager::HasLoadedEXRSequences() const {
        // Check if currently playing video is an EXR sequence
        bool in_exr_mode = video_player && video_player->IsInEXRMode();
        Debug::Log("HasLoadedEXRSequences check: video_player IsInEXRMode = " + std::string(in_exr_mode ? "true" : "false"));

        if (in_exr_mode) {
            Debug::Log("HasLoadedEXRSequences: Returning true (video player is in EXR mode)");
            return true;
        }

        // Check if any media items in project are EXR sequences
        int exr_count = 0;
        for (const auto& item : media_pool) {
            if (item.type == MediaType::EXR_SEQUENCE) {
                exr_count++;
            }
        }
        Debug::Log("HasLoadedEXRSequences: Found " + std::to_string(exr_count) + " EXR sequences in media_pool");

        if (exr_count > 0) {
            Debug::Log("HasLoadedEXRSequences: Returning true (found EXR in media pool)");
            return true;
        }

        Debug::Log("HasLoadedEXRSequences: Returning false (no EXR sequences found)");
        return false;
    }

    PipelineMode ProjectManager::GetImageSequencePipelineMode() const {
        // Get pipeline mode from video cache (auto-detected from first frame)
        if (video_cache_manager) {
            return video_cache_manager->GetPipelineMode();
        }

        // Default fallback
        return PipelineMode::NORMAL;
    }

    std::string ProjectManager::GetAnnotationPathForMedia(const std::string& media_path) const {
        // For regular files (videos), return as-is
        if (media_path.find("mf://") != 0 && media_path.find("exr://") != 0) {
            return media_path;
        }

        // For mf:// URLs (image sequences), extract directory from stored MediaItem
        if (media_path.find("mf://") == 0) {
            for (const auto& item : media_pool) {
                if (item.path == media_path && !item.ffmpeg_pattern.empty()) {
                    // ffmpeg_pattern is like "/path/to/sequence_%04d.png"
                    // Extract directory from it
                    std::filesystem::path pattern_path(item.ffmpeg_pattern);
                    return pattern_path.parent_path().string();
                }
            }
        }

        // For exr:// URLs, extract path before the ? (layer parameter)
        if (media_path.find("exr://") == 0) {
            std::string path_part = media_path.substr(6); // Remove "exr://"
            size_t query_pos = path_part.find('?');
            if (query_pos != std::string::npos) {
                path_part = path_part.substr(0, query_pos);
            }
            return path_part;
        }

        // Fallback: return as-is
        return media_path;
    }

}