// ============================================================================
// User settings persistence (Load/Save/Delete)
// ============================================================================

#include "app/application.h"
#include "app/app_config.h"
#include "utils/debug_utils.h"
#include "player/pipeline_mode.h"
#include "player/video_decoder_factory.h"
#include "transcode/transcode_settings.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdlib>

// Globals defined in main.cpp
extern bool use_windows_accent_color;
extern int custom_accent_color_index;
extern ImVec4 custom_picker_color;
extern const int accent_color_palette_count;
extern bool cache_enabled;
extern float g_font_scale;
extern int g_exr_read_ahead_frames;
extern int g_read_behind_frames;
extern int g_exr_thread_count;
extern int g_exr_transcode_threads;
extern int g_timeline_read_ahead_frames;
extern int g_timeline_read_behind_frames;
extern int g_timeline_max_textures;
extern int g_timeline_io_threads;
extern int g_video_decode_threads;
extern std::string g_custom_cache_path;
extern int g_cache_retention_days;
extern int g_transcode_cache_max_gb;
extern bool g_clear_cache_on_exit;
extern TranscodeSettings transcode_settings;

// ============================================================================
// USER SETTINGS PERSISTENCE
// ============================================================================

std::string Application::GetSettingsPath() {
    const char* localappdata = std::getenv("LOCALAPPDATA");
    if (localappdata) {
        std::string base_path = std::string(localappdata) + "\\qcview";
        // Ensure directory exists
        std::filesystem::create_directories(base_path);
        return base_path + "\\settings.qcv";
    }
    return "settings.qcv";  // Fallback to current directory
}

std::string Application::GetLayoutIniPath() {
    const char* localappdata = std::getenv("LOCALAPPDATA");
    if (localappdata) {
        std::string base_path = std::string(localappdata) + "\\qcview";
        // Ensure directory exists
        std::filesystem::create_directories(base_path);
        return base_path + "\\layout.ini";
    }
    return "imgui.ini";  // Fallback to default
}

void Application::LoadSettings() {
    try {
        std::string settings_path = GetSettingsPath();

        if (!std::filesystem::exists(settings_path)) {
            Debug::Log("No settings file found, using defaults");
            first_time_setup = true; // No settings, build default layout
            return;
        }

        std::ifstream file(settings_path);
        if (!file.is_open()) {
            Debug::Log("Failed to open settings file");
            first_time_setup = true; // Can't open settings, build default layout
            return;
        }

        nlohmann::json j = nlohmann::json::parse(file);
        file.close();

        // Appearance settings
        if (j.contains("appearance")) {
            if (j["appearance"].contains("use_windows_accent_color")) {
                use_windows_accent_color = j["appearance"]["use_windows_accent_color"].get<bool>();
            }
            if (j["appearance"].contains("custom_accent_color_index")) {
                custom_accent_color_index = j["appearance"]["custom_accent_color_index"].get<int>();
                // Validate the index is within bounds (-2 = custom picker, -1 = default, 0+ = palette)
                if (custom_accent_color_index >= accent_color_palette_count) {
                    custom_accent_color_index = -1;
                }
            }
            // Load custom picker color
            if (j["appearance"].contains("custom_picker_color_r")) {
                custom_picker_color.x = j["appearance"]["custom_picker_color_r"].get<float>();
            }
            if (j["appearance"].contains("custom_picker_color_g")) {
                custom_picker_color.y = j["appearance"]["custom_picker_color_g"].get<float>();
            }
            if (j["appearance"].contains("custom_picker_color_b")) {
                custom_picker_color.z = j["appearance"]["custom_picker_color_b"].get<float>();
            }
            if (j["appearance"].contains("video_background")) {
                std::string bg_str = j["appearance"]["video_background"].get<std::string>();
                if (bg_str == "BLACK") video_background_type = VideoBackgroundType::BLACK;
                else if (bg_str == "DEFAULT") video_background_type = VideoBackgroundType::DEFAULT;
                else if (bg_str == "DARK_CHECKERBOARD") video_background_type = VideoBackgroundType::DARK_CHECKERBOARD;
                else if (bg_str == "LIGHT_CHECKERBOARD") video_background_type = VideoBackgroundType::LIGHT_CHECKERBOARD;
            }
            if (j["appearance"].contains("font_scale")) {
                g_font_scale = j["appearance"]["font_scale"].get<float>();
                // Clamp to valid range
                if (g_font_scale < 0.5f) g_font_scale = 0.5f;
                if (g_font_scale > 2.0f) g_font_scale = 2.0f;
            }
        }

        // Window position and size (will be applied after window creation)
        if (j.contains("window")) {
            if (j["window"].contains("x")) {
                saved_window_x = j["window"]["x"].get<int>();
            }
            if (j["window"].contains("y")) {
                saved_window_y = j["window"]["y"].get<int>();
            }
            if (j["window"].contains("width")) {
                saved_window_width = j["window"]["width"].get<int>();
            }
            if (j["window"].contains("height")) {
                saved_window_height = j["window"]["height"].get<int>();
            }
            has_saved_window_settings = true;
        }

        // Frame.io settings
        if (j.contains("frameio")) {
            if (j["frameio"].contains("saved_token")) {
                std::string encoded_token = j["frameio"]["saved_token"].get<std::string>();
                std::string decoded_token = DecodeBase64(encoded_token);
                if (!decoded_token.empty()) {
                    strncpy_s(frameio_import_state.token_buffer,
                              sizeof(frameio_import_state.token_buffer),
                              decoded_token.c_str(), _TRUNCATE);
                    Debug::Log("Loaded Frame.io token from settings");
                }
            }
        }

        // Video cache settings
        if (j.contains("video_cache")) {
            if (j["video_cache"].contains("cache_enabled")) {
                cache_enabled = j["video_cache"]["cache_enabled"].get<bool>();
            }
            if (j["video_cache"].contains("max_cache_seconds")) {
                cache_settings.max_cache_seconds = j["video_cache"]["max_cache_seconds"].get<int>();
            }
            if (j["video_cache"].contains("enable_nvidia_decode")) {
                cache_settings.enable_nvidia_decode = j["video_cache"]["enable_nvidia_decode"].get<bool>();
            }
            if (j["video_cache"].contains("max_batch_size")) {
                cache_settings.max_batch_size = j["video_cache"]["max_batch_size"].get<int>();
            }
            if (j["video_cache"].contains("max_concurrent_batches")) {
                cache_settings.max_concurrent_batches = j["video_cache"]["max_concurrent_batches"].get<int>();
            }
            if (j["video_cache"].contains("pipeline_mode")) {
                std::string mode_str = j["video_cache"]["pipeline_mode"].get<std::string>();
                cache_settings.current_pipeline_mode = StringToPipelineMode(mode_str);
            }
        }

        // Image sequence cache settings (applies to EXR/TIFF/PNG/JPEG sequences)
        if (j.contains("exr_cache")) {
            if (j["exr_cache"].contains("read_ahead_frames")) {
                g_exr_read_ahead_frames = j["exr_cache"]["read_ahead_frames"].get<int>();
                printf("[Settings Load] g_exr_read_ahead_frames = %d (from exr_cache)\n", g_exr_read_ahead_frames);
            }
            if (j["exr_cache"].contains("read_behind_frames")) {
                g_read_behind_frames = j["exr_cache"]["read_behind_frames"].get<int>();
                printf("[Settings Load] g_read_behind_frames = %d (from exr_cache)\n", g_read_behind_frames);
            } else if (j["exr_cache"].contains("read_behind_seconds")) {
                // Backwards compatibility: convert old seconds to frames @ 24fps
                g_read_behind_frames = static_cast<int>(j["exr_cache"]["read_behind_seconds"].get<float>() * 24.0f);
            }
        }

        // Timeline cache settings (EDL/OTIO playback)
        if (j.contains("timeline_cache")) {
            if (j["timeline_cache"].contains("read_ahead_frames")) {
                g_timeline_read_ahead_frames = j["timeline_cache"]["read_ahead_frames"].get<int>();
                printf("[Settings Load] g_timeline_read_ahead_frames = %d (from timeline_cache)\n", g_timeline_read_ahead_frames);
            }
            if (j["timeline_cache"].contains("read_behind_frames")) {
                g_timeline_read_behind_frames = j["timeline_cache"]["read_behind_frames"].get<int>();
                printf("[Settings Load] g_timeline_read_behind_frames = %d (from timeline_cache)\n", g_timeline_read_behind_frames);
            }
            if (j["timeline_cache"].contains("max_textures")) {
                g_timeline_max_textures = j["timeline_cache"]["max_textures"].get<int>();
            }
            if (j["timeline_cache"].contains("io_threads")) {
                g_timeline_io_threads = j["timeline_cache"]["io_threads"].get<int>();
            }
            if (j["timeline_cache"].contains("decode_threads")) {
                g_video_decode_threads = j["timeline_cache"]["decode_threads"].get<int>();
            }
        }

        // Performance settings (image sequence I/O + EXR transcode)
        if (j.contains("performance")) {
            if (j["performance"].contains("exr_io_threads")) {
                g_exr_thread_count = j["performance"]["exr_io_threads"].get<int>();
            }
            if (j["performance"].contains("exr_transcode_threads")) {
                g_exr_transcode_threads = j["performance"]["exr_transcode_threads"].get<int>();
            }
        }

        // Disk cache settings
        if (j.contains("disk_cache")) {
            if (j["disk_cache"].contains("custom_path")) {
                g_custom_cache_path = j["disk_cache"]["custom_path"].get<std::string>();
            }
            if (j["disk_cache"].contains("retention_days")) {
                g_cache_retention_days = j["disk_cache"]["retention_days"].get<int>();
            }
            if (j["disk_cache"].contains("transcode_cache_max_gb")) {
                g_transcode_cache_max_gb = j["disk_cache"]["transcode_cache_max_gb"].get<int>();
            }
            if (j["disk_cache"].contains("clear_on_exit")) {
                g_clear_cache_on_exit = j["disk_cache"]["clear_on_exit"].get<bool>();
            }
        }

        // Thumbnail scrubbing settings
        if (j.contains("thumbnails")) {
            if (j["thumbnails"].contains("enabled")) {
                cache_settings.enable_thumbnails = j["thumbnails"]["enabled"].get<bool>();
            }
            if (j["thumbnails"].contains("width")) {
                cache_settings.thumbnail_width = j["thumbnails"]["width"].get<int>();
            }
            if (j["thumbnails"].contains("height")) {
                cache_settings.thumbnail_height = j["thumbnails"]["height"].get<int>();
            }
            if (j["thumbnails"].contains("cache_size")) {
                cache_settings.thumbnail_cache_size = j["thumbnails"]["cache_size"].get<int>();
            }
        }

        // Playback settings
        if (j.contains("playback")) {
            if (j["playback"].contains("auto_play_on_load")) {
                cache_settings.auto_play_on_load = j["playback"]["auto_play_on_load"].get<bool>();
            }
            if (j["playback"].contains("loop_enabled")) {
                cache_settings.loop_enabled = j["playback"]["loop_enabled"].get<bool>();
            }
        }

        // Audio sync settings
        if (j.contains("audio_sync")) {
            if (j["audio_sync"].contains("display_latency_preset")) {
                cache_settings.display_latency_preset = j["audio_sync"]["display_latency_preset"].get<int>();
                // Clamp to valid range (0-5)
                if (cache_settings.display_latency_preset < 0) cache_settings.display_latency_preset = 0;
                if (cache_settings.display_latency_preset > 5) cache_settings.display_latency_preset = 5;
            }
            if (j["audio_sync"].contains("fine_tune_ms")) {
                cache_settings.audio_fine_tune_ms = j["audio_sync"]["fine_tune_ms"].get<float>();
                // Clamp to ±50ms
                if (cache_settings.audio_fine_tune_ms < -50.0f) cache_settings.audio_fine_tune_ms = -50.0f;
                if (cache_settings.audio_fine_tune_ms > 50.0f) cache_settings.audio_fine_tune_ms = 50.0f;
            }
            if (j["audio_sync"].contains("custom_display_latency_ms")) {
                cache_settings.custom_display_latency_ms = j["audio_sync"]["custom_display_latency_ms"].get<float>();
                // Clamp to 0-100ms
                if (cache_settings.custom_display_latency_ms < 0.0f) cache_settings.custom_display_latency_ms = 0.0f;
                if (cache_settings.custom_display_latency_ms > 100.0f) cache_settings.custom_display_latency_ms = 100.0f;
            }
        }

        // Color management settings
        if (j.contains("color_management")) {
            if (j["color_management"].contains("auto_121_enabled")) {
                cache_settings.auto_121_enabled = j["color_management"]["auto_121_enabled"].get<bool>();
            }
        }

        // Video settings
        if (j.contains("video")) {
            if (j["video"].contains("range_mode")) {
                cache_settings.video_range_mode = j["video"]["range_mode"].get<int>();
                // Clamp to valid range
                if (cache_settings.video_range_mode < 0) cache_settings.video_range_mode = 0;
                if (cache_settings.video_range_mode > 2) cache_settings.video_range_mode = 2;
                // Apply to global decoder setting
                qcview::g_video_range_override = static_cast<VideoRangeMode>(cache_settings.video_range_mode);
            }
        }

        // Safety overlay settings
        if (j.contains("safety_overlay")) {
            if (j["safety_overlay"].contains("type")) {
                std::string type_str = j["safety_overlay"]["type"].get<std::string>();
                if (type_str == "TITLE_SAFE_16_9") safety_settings.type = SafetyOverlayType::TITLE_SAFE_16_9;
                else if (type_str == "ACTION_SAFE_16_9") safety_settings.type = SafetyOverlayType::ACTION_SAFE_16_9;
                else if (type_str == "TITLE_SAFE_9_16") safety_settings.type = SafetyOverlayType::TITLE_SAFE_9_16;
                else if (type_str == "ACTION_SAFE_9_16") safety_settings.type = SafetyOverlayType::ACTION_SAFE_9_16;
                else if (type_str == "BROADCAST_SAFE") safety_settings.type = SafetyOverlayType::BROADCAST_SAFE;
                else if (type_str == "CENTER_CUT_SAFE") safety_settings.type = SafetyOverlayType::CENTER_CUT_SAFE;
                else if (type_str == "CUSTOM_GUIDE") safety_settings.type = SafetyOverlayType::CUSTOM_GUIDE;
                else safety_settings.type = SafetyOverlayType::NONE;
            }
            if (j["safety_overlay"].contains("opacity")) {
                safety_settings.opacity = j["safety_overlay"]["opacity"].get<float>();
            }
            if (j["safety_overlay"].contains("line_width")) {
                safety_settings.line_width = j["safety_overlay"]["line_width"].get<float>();
            }
            if (j["safety_overlay"].contains("color_r")) {
                safety_settings.color[0] = j["safety_overlay"]["color_r"].get<float>();
            }
            if (j["safety_overlay"].contains("color_g")) {
                safety_settings.color[1] = j["safety_overlay"]["color_g"].get<float>();
            }
            if (j["safety_overlay"].contains("color_b")) {
                safety_settings.color[2] = j["safety_overlay"]["color_b"].get<float>();
            }
            if (j["safety_overlay"].contains("color_a")) {
                safety_settings.color[3] = j["safety_overlay"]["color_a"].get<float>();
            }
            if (j["safety_overlay"].contains("show_labels")) {
                safety_settings.show_labels = j["safety_overlay"]["show_labels"].get<bool>();
            }
            if (j["safety_overlay"].contains("show_percentage_marks")) {
                safety_settings.show_percentage_marks = j["safety_overlay"]["show_percentage_marks"].get<bool>();
            }
        }

        // Transcode performance settings
        if (j.contains("transcode")) {
            if (j["transcode"].contains("encoder_threads")) {
                transcode_settings.encoder_thread_count = j["transcode"]["encoder_threads"].get<int>();
            }
            if (j["transcode"].contains("prefer_hardware")) {
                transcode_settings.prefer_hardware_encoding = j["transcode"]["prefer_hardware"].get<bool>();
            }
            if (j["transcode"].contains("hardware_encoder")) {
                transcode_settings.hardware_encoder = j["transcode"]["hardware_encoder"].get<std::string>();
            }
            if (j["transcode"].contains("default_workers")) {
                transcode_settings.default_worker_count = j["transcode"]["default_workers"].get<int>();
            }
            if (j["transcode"].contains("max_workers")) {
                transcode_settings.max_worker_count = j["transcode"]["max_workers"].get<int>();
            }
            if (j["transcode"].contains("prefetch_buffer")) {
                transcode_settings.prefetch_buffer_size = j["transcode"]["prefetch_buffer"].get<int>();
            }
            if (j["transcode"].contains("prefetch_ahead")) {
                transcode_settings.prefetch_ahead_count = j["transcode"]["prefetch_ahead"].get<int>();
            }
            if (j["transcode"].contains("concurrent_loads")) {
                transcode_settings.concurrent_loads = j["transcode"]["concurrent_loads"].get<int>();
            }
            if (j["transcode"].contains("openexr_threads")) {
                transcode_settings.openexr_threads = j["transcode"]["openexr_threads"].get<int>();
            }
        }

        // Store ImGui layout to load after ImGui is initialized
        if (j.contains("imgui_layout")) {
            saved_imgui_layout = j["imgui_layout"].get<std::string>();
            if (!saved_imgui_layout.empty()) {
                first_time_setup = false; // We have a saved layout, don't rebuild
                Debug::Log("Found saved ImGui layout (will load after ImGui init)");
            }
        } else {
            // No saved layout, need to build default layout
            first_time_setup = true;
            Debug::Log("No saved layout - will build default layout");
        }

        // Load panel visibility
        if (j.contains("panels")) {
            if (j["panels"].contains("show_project")) {
                show_project_panel = j["panels"]["show_project"].get<bool>();
            }
            if (j["panels"].contains("show_inspector")) {
                show_inspector_panel = j["panels"]["show_inspector"].get<bool>();
            }
            if (j["panels"].contains("show_timeline")) {
                show_timeline_panel = j["panels"]["show_timeline"].get<bool>();
            }
            if (j["panels"].contains("show_annotations")) {
                show_annotation_panel = j["panels"]["show_annotations"].get<bool>();
            }
            if (j["panels"].contains("show_color")) {
                show_color_panels = j["panels"]["show_color"].get<bool>();
            }
            if (j["panels"].contains("show_sidebar")) {
                show_sidebar_panel = j["panels"]["show_sidebar"].get<bool>();
            }
        }

        Debug::Log("Loaded user settings from: " + settings_path);

    } catch (const std::exception& e) {
        Debug::Log("Error loading settings: " + std::string(e.what()));
    }
}

void Application::SaveSettings() {
    try {
        nlohmann::json j;

        // Appearance settings
        j["appearance"]["use_windows_accent_color"] = use_windows_accent_color;
        j["appearance"]["custom_accent_color_index"] = custom_accent_color_index;
        j["appearance"]["custom_picker_color_r"] = custom_picker_color.x;
        j["appearance"]["custom_picker_color_g"] = custom_picker_color.y;
        j["appearance"]["custom_picker_color_b"] = custom_picker_color.z;

        std::string bg_str = "BLACK";
        switch (video_background_type) {
            case VideoBackgroundType::BLACK: bg_str = "BLACK"; break;
            case VideoBackgroundType::DEFAULT: bg_str = "DEFAULT"; break;
            case VideoBackgroundType::DARK_CHECKERBOARD: bg_str = "DARK_CHECKERBOARD"; break;
            case VideoBackgroundType::LIGHT_CHECKERBOARD: bg_str = "LIGHT_CHECKERBOARD"; break;
        }
        j["appearance"]["video_background"] = bg_str;
        j["appearance"]["font_scale"] = g_font_scale;

        // Window position and size
        if (window) {
            int x, y, width, height;
            glfwGetWindowPos(window, &x, &y);
            glfwGetWindowSize(window, &width, &height);
            j["window"]["x"] = x;
            j["window"]["y"] = y;
            j["window"]["width"] = width;
            j["window"]["height"] = height;
        }

        // Frame.io settings
        // Save token if present (base64 encoded for obfuscation)
        if (strlen(frameio_import_state.token_buffer) > 0) {
            std::string token(frameio_import_state.token_buffer);
            std::string encoded = EncodeBase64(token);
            j["frameio"]["saved_token"] = encoded;
        } else {
            // If token is empty, remove it from settings
            if (j["frameio"].contains("saved_token")) {
                j["frameio"].erase("saved_token");
            }
        }

        // Video cache settings
        j["video_cache"]["cache_enabled"] = cache_enabled;
        j["video_cache"]["max_cache_seconds"] = cache_settings.max_cache_seconds;
        j["video_cache"]["enable_nvidia_decode"] = cache_settings.enable_nvidia_decode;
        j["video_cache"]["max_batch_size"] = cache_settings.max_batch_size;
        j["video_cache"]["max_concurrent_batches"] = cache_settings.max_concurrent_batches;
        j["video_cache"]["pipeline_mode"] = PipelineModeToString(cache_settings.current_pipeline_mode);

        // Image sequence cache settings (EXR/TIFF/PNG/JPEG)
        j["exr_cache"]["read_ahead_frames"] = g_exr_read_ahead_frames;
        j["exr_cache"]["read_behind_frames"] = g_read_behind_frames;

        // Timeline cache settings (EDL/OTIO playback)
        j["timeline_cache"]["read_ahead_frames"] = g_timeline_read_ahead_frames;
        j["timeline_cache"]["read_behind_frames"] = g_timeline_read_behind_frames;
        j["timeline_cache"]["max_textures"] = g_timeline_max_textures;
        j["timeline_cache"]["io_threads"] = g_timeline_io_threads;
        j["timeline_cache"]["decode_threads"] = g_video_decode_threads;

        // Performance settings (image sequence I/O + EXR transcode)
        j["performance"]["exr_io_threads"] = g_exr_thread_count;
        j["performance"]["exr_transcode_threads"] = g_exr_transcode_threads;

        // Disk cache settings
        j["disk_cache"]["custom_path"] = g_custom_cache_path;
        j["disk_cache"]["retention_days"] = g_cache_retention_days;
        j["disk_cache"]["transcode_cache_max_gb"] = g_transcode_cache_max_gb;
        j["disk_cache"]["clear_on_exit"] = g_clear_cache_on_exit;

        // Thumbnail scrubbing settings
        j["thumbnails"]["enabled"] = cache_settings.enable_thumbnails;
        j["thumbnails"]["width"] = cache_settings.thumbnail_width;
        j["thumbnails"]["height"] = cache_settings.thumbnail_height;
        j["thumbnails"]["cache_size"] = cache_settings.thumbnail_cache_size;

        // Playback settings
        j["playback"]["auto_play_on_load"] = cache_settings.auto_play_on_load;
        j["playback"]["loop_enabled"] = cache_settings.loop_enabled;

        // Audio sync settings
        j["audio_sync"]["display_latency_preset"] = cache_settings.display_latency_preset;
        j["audio_sync"]["fine_tune_ms"] = cache_settings.audio_fine_tune_ms;
        j["audio_sync"]["custom_display_latency_ms"] = cache_settings.custom_display_latency_ms;

        // Color management settings
        j["color_management"]["auto_121_enabled"] = cache_settings.auto_121_enabled;

        // Video settings
        j["video"]["range_mode"] = cache_settings.video_range_mode;

        // Safety overlay settings
        std::string overlay_type_str = "NONE";
        switch (safety_settings.type) {
            case SafetyOverlayType::TITLE_SAFE_16_9: overlay_type_str = "TITLE_SAFE_16_9"; break;
            case SafetyOverlayType::ACTION_SAFE_16_9: overlay_type_str = "ACTION_SAFE_16_9"; break;
            case SafetyOverlayType::TITLE_SAFE_9_16: overlay_type_str = "TITLE_SAFE_9_16"; break;
            case SafetyOverlayType::ACTION_SAFE_9_16: overlay_type_str = "ACTION_SAFE_9_16"; break;
            case SafetyOverlayType::BROADCAST_SAFE: overlay_type_str = "BROADCAST_SAFE"; break;
            case SafetyOverlayType::CENTER_CUT_SAFE: overlay_type_str = "CENTER_CUT_SAFE"; break;
            case SafetyOverlayType::CUSTOM_GUIDE: overlay_type_str = "CUSTOM_GUIDE"; break;
            case SafetyOverlayType::NONE: overlay_type_str = "NONE"; break;
        }
        j["safety_overlay"]["type"] = overlay_type_str;
        j["safety_overlay"]["opacity"] = safety_settings.opacity;
        j["safety_overlay"]["line_width"] = safety_settings.line_width;
        j["safety_overlay"]["color_r"] = safety_settings.color[0];
        j["safety_overlay"]["color_g"] = safety_settings.color[1];
        j["safety_overlay"]["color_b"] = safety_settings.color[2];
        j["safety_overlay"]["color_a"] = safety_settings.color[3];
        j["safety_overlay"]["show_labels"] = safety_settings.show_labels;
        j["safety_overlay"]["show_percentage_marks"] = safety_settings.show_percentage_marks;

        // Transcode performance settings
        j["transcode"]["encoder_threads"] = transcode_settings.encoder_thread_count;
        j["transcode"]["prefer_hardware"] = transcode_settings.prefer_hardware_encoding;
        j["transcode"]["hardware_encoder"] = transcode_settings.hardware_encoder;
        j["transcode"]["default_workers"] = transcode_settings.default_worker_count;
        j["transcode"]["max_workers"] = transcode_settings.max_worker_count;
        j["transcode"]["prefetch_buffer"] = transcode_settings.prefetch_buffer_size;
        j["transcode"]["prefetch_ahead"] = transcode_settings.prefetch_ahead_count;
        j["transcode"]["concurrent_loads"] = transcode_settings.concurrent_loads;
        j["transcode"]["openexr_threads"] = transcode_settings.openexr_threads;

        // Save ImGui layout to memory
        size_t ini_size = 0;
        const char* ini_data = ImGui::SaveIniSettingsToMemory(&ini_size);
        if (ini_data && ini_size > 0) {
            j["imgui_layout"] = std::string(ini_data, ini_size);
        }

        // Save panel visibility states
        j["panels"]["show_project"] = show_project_panel;
        j["panels"]["show_inspector"] = show_inspector_panel;
        j["panels"]["show_timeline"] = show_timeline_panel;
        j["panels"]["show_annotations"] = show_annotation_panel;
        j["panels"]["show_color"] = show_color_panels;
        j["panels"]["show_sidebar"] = show_sidebar_panel;

        std::string settings_path = GetSettingsPath();
        std::ofstream file(settings_path);
        if (file.is_open()) {
            file << j.dump(2);  // Pretty print with 2-space indent
            file.close();
            Debug::Log("Saved user settings to: " + settings_path);
        } else {
            Debug::Log("Failed to save settings file");
        }

    } catch (const std::exception& e) {
        Debug::Log("Error saving settings: " + std::string(e.what()));
    }
}

void Application::DeleteAllPreferences() {
    try {
        std::string settings_path = GetSettingsPath();

        if (std::filesystem::exists(settings_path)) {
            std::filesystem::remove(settings_path);
            Debug::Log("Deleted settings file: " + settings_path);
            Debug::Log("Application will restart with default settings");

            // Show a message to the user
            ImGui::OpenPopup("Preferences Deleted");
        } else {
            Debug::Log("No settings file to delete");
        }
    } catch (const std::exception& e) {
        Debug::Log("Error deleting settings: " + std::string(e.what()));
    }
}
