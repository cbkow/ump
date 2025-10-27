// Transcode Queue Integration for ProjectManager
// This file contains transcode-specific methods that should be integrated into project_manager.cpp

#include "project_manager.h"
#include "../utils/debug_utils.h"
#include "../color/ocio_pipeline.h"
#include "../color/ocio_config_manager.h"
#include "../player/video_player.h"
#include "../nodes/node_manager.h"
#include "../nodes/node_base.h"
#include "../transcode/ffmpeg_video_encoder.h"
#include "../metadata/adobe_metadata.h"
#include "../metadata/ffmpeg_metadata_extractor.h"
#include <imgui.h>
#include <nfd.h>
#include <filesystem>
#include <sstream>
#include <regex>
#include <iomanip>
#include <algorithm>

// External global OCIO manager (defined in main.cpp)
extern std::unique_ptr<OCIOConfigManager> ocio_manager;

// External transcode performance settings (defined in main.cpp)
extern struct {
    int encoder_thread_count;
    bool prefer_hardware_encoding;
    std::string hardware_encoder;
    int default_worker_count;
    int max_worker_count;
    int prefetch_buffer_size;
    int prefetch_ahead_count;
} transcode_settings;

namespace ump {

// ============================================================================
// INITIALIZATION (Add to ProjectManager constructor)
// ============================================================================

/*
 * Add to ProjectManager::ProjectManager() constructor, after video_cache_manager initialization:
 *
 * // Initialize transcode queue
 * transcode_queue_ = std::make_unique<TranscodeQueue>();
 * transcode_worker_pool_ = std::make_unique<TranscodeWorkerPool>(transcode_queue_.get(), 2);
 * transcode_queue_window_ = std::make_unique<TranscodeQueueWindow>(transcode_queue_.get(), transcode_worker_pool_.get());
 *
 * // Set auto-save path
 * std::string queue_save_path = std::string(getenv("USERPROFILE") ? getenv("USERPROFILE") : getenv("HOME")) + "/.unionplayer/transcode_queue.json";
 * transcode_queue_->SetAutoSavePath(queue_save_path);
 *
 * // Load existing queue
 * transcode_queue_->LoadQueue(queue_save_path);
 *
 * // Start worker pool
 * transcode_worker_pool_->Start();
 *
 * Debug::Log("ProjectManager: Transcode queue initialized");
 */

// ============================================================================
// CLEANUP (Add to ProjectManager destructor)
// ============================================================================

/*
 * Add to ProjectManager::~ProjectManager() destructor, before other cleanup:
 *
 * // Stop transcode workers
 * if (transcode_worker_pool_) {
 *     transcode_worker_pool_->Stop();
 * }
 *
 * // Save queue
 * if (transcode_queue_) {
 *     std::string queue_save_path = std::string(getenv("USERPROFILE") ? getenv("USERPROFILE") : getenv("HOME")) + "/.unionplayer/transcode_queue.json";
 *     transcode_queue_->SaveQueue(queue_save_path);
 * }
 */

// ============================================================================
// PUBLIC METHODS
// ============================================================================

void ProjectManager::ShowTranscodeQueueWindow() {
    if (transcode_queue_window_) {
        transcode_queue_window_->Show();
    }
}

void ProjectManager::ToggleTranscodeQueueWindow() {
    if (transcode_queue_window_) {
        transcode_queue_window_->Toggle();
    }
}

bool ProjectManager::IsTranscodeQueueWindowOpen() const {
    return transcode_queue_window_ && transcode_queue_window_->IsOpen();
}

void ProjectManager::RenderTranscodeQueueWindow() {
    if (transcode_queue_window_) {
        transcode_queue_window_->Render();
    }
}

void ProjectManager::AddSelectedItemsToTranscodeQueue() {
    if (selected_media_items.empty()) {
        Debug::Log("ProjectManager::AddSelectedItemsToTranscodeQueue - No items selected");
        return;
    }

    Debug::Log("ProjectManager: Opening transcode settings dialog for " +
               std::to_string(selected_media_items.size()) + " items");

    show_transcode_settings_dialog = true;
    transcode_settings_dialog_opened = false;

    // Set default output directory to parent directory of first selected item
    auto selected_items = GetSelectedItems();
    if (!selected_items.empty()) {
        std::string default_dir;

        // Get the parent directory from the first selected item's ffmpeg_pattern or path
        if (!selected_items[0].ffmpeg_pattern.empty()) {
            std::filesystem::path source_path(selected_items[0].ffmpeg_pattern);
            default_dir = source_path.parent_path().string();
        } else if (!selected_items[0].path.empty()) {
            std::filesystem::path source_path(selected_items[0].path);
            default_dir = source_path.parent_path().string();
        }

        // Fallback to Videos folder if we couldn't determine source directory
        if (default_dir.empty()) {
            #ifdef _WIN32
            default_dir = std::string(getenv("USERPROFILE")) + "\\Videos";
            #else
            default_dir = std::string(getenv("HOME")) + "/Videos";
            #endif
        }

        strncpy(transcode_dialog_settings.output_directory, default_dir.c_str(),
                sizeof(transcode_dialog_settings.output_directory) - 1);
    }
}

void ProjectManager::ShowTranscodeSettingsDialog() {
    show_transcode_settings_dialog = true;
}

// ============================================================================
// DIALOG RENDERING
// ============================================================================

void ProjectManager::RenderTranscodeSettingsDialog() {
    if (!show_transcode_settings_dialog) {
        return;
    }

    // Open modal popup on first show
    if (!transcode_settings_dialog_opened) {
        ImGui::OpenPopup("Transcode Settings");
        transcode_settings_dialog_opened = true;
    }

    // Center the modal
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);

    if (!ImGui::BeginPopupModal("Transcode Settings", &show_transcode_settings_dialog, ImGuiWindowFlags_NoResize)) {
        return;
    }

    auto& settings = transcode_dialog_settings;

    ImGui::Text("Add %zu item(s) to transcode queue", selected_media_items.size());
    ImGui::Separator();

    // Codec selection
    ImGui::Text("Codec:");
    const char* codec_items[] = {"H.264 (libx264)", "H.265 (libx265)", "ProRes 422 LT", "ProRes 422 HQ", "ProRes 4444"};
    ImGui::Combo("##Codec", &settings.codec_index, codec_items, IM_ARRAYSIZE(codec_items));

    // Quality settings (CRF for H.264/H.265)
    if (settings.codec_index == 0 || settings.codec_index == 1) {
        ImGui::Text("Quality (CRF):");
        ImGui::SliderInt("##CRF", &settings.crf, 0, 51);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Lower = Better Quality, Higher File Size\n0 = Lossless, 18 = Visually Lossless, 23 = Default, 51 = Worst");
        }

        // Preset selection
        ImGui::Text("Encoding Speed:");
        const char* preset_items[] = {"Ultrafast", "Fast", "Slow", "Very Slow"};
        ImGui::Combo("##Preset", &settings.preset_index, preset_items, IM_ARRAYSIZE(preset_items));
    }

    ImGui::Separator();

    // Resolution
    ImGui::Text("Resolution:");
    const char* resolution_items[] = {"Source", "4K (3840x2160)", "1080p (1920x1080)", "Custom"};
    ImGui::Combo("##Resolution", &settings.resolution_mode, resolution_items, IM_ARRAYSIZE(resolution_items));

    if (settings.resolution_mode == 3) {  // Custom
        ImGui::InputInt("Width", &settings.custom_width);
        ImGui::InputInt("Height", &settings.custom_height);
    }

    ImGui::Separator();

    // OCIO settings
    ImGui::Checkbox("Use Current OCIO Settings", &settings.use_current_ocio);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Apply current color pipeline (colorspace, display, view, LUTs) to transcode");
    }

    // TODO: Display current OCIO settings here
    if (settings.use_current_ocio) {
        ImGui::Indent();
        ImGui::TextDisabled("Current OCIO settings will be applied");
        // ImGui::Text("Colorspace: %s", current_colorspace.c_str());
        // ImGui::Text("Display: %s / %s", current_display.c_str(), current_view.c_str());
        ImGui::Unindent();
    }

    ImGui::Separator();

    // Output directory
    ImGui::Text("Output Directory:");
    ImGui::InputText("##OutputDir", settings.output_directory,
                    sizeof(settings.output_directory));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        nfdchar_t* out_path = nullptr;
        nfdresult_t result = NFD_PickFolder(&out_path, settings.output_directory);

        if (result == NFD_OKAY) {
            strncpy(settings.output_directory, out_path,
                    sizeof(settings.output_directory) - 1);
            settings.output_directory[sizeof(settings.output_directory) - 1] = '\0';
            NFD_FreePath(out_path);
            Debug::Log("Output directory selected: " + std::string(settings.output_directory));
        } else if (result == NFD_ERROR) {
            Debug::Log("ERROR: Directory picker failed: " + std::string(NFD_GetError()));
        }
    }

    ImGui::Separator();

    // Priority
    ImGui::Text("Queue Priority:");
    const char* priority_items[] = {"Low", "Normal", "High", "Urgent"};
    ImGui::Combo("##Priority", &settings.priority_index, priority_items, IM_ARRAYSIZE(priority_items));

    ImGui::Separator();

    // Action buttons
    if (ImGui::Button("Add to Queue", ImVec2(150, 30))) {
        ProcessAddToTranscodeQueue();
        show_transcode_settings_dialog = false;
        transcode_settings_dialog_opened = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(150, 30))) {
        show_transcode_settings_dialog = false;
        transcode_settings_dialog_opened = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();

    // Reset dialog state when closed by X button or Escape
    if (!show_transcode_settings_dialog) {
        transcode_settings_dialog_opened = false;
    }
}

// ============================================================================
// JOB CREATION
// ============================================================================

// Helper function to sanitize filename for Windows/cross-platform compatibility
static std::string SanitizeFilename(const std::string& filename) {
    std::string result = filename;

    // Replace spaces with underscores
    std::replace(result.begin(), result.end(), ' ', '_');

    // Remove or replace illegal Windows characters: < > : " / \ | ? * and brackets [ ]
    std::string illegal_chars = "<>:\"/\\|?*[]";
    result.erase(std::remove_if(result.begin(), result.end(),
        [&illegal_chars](char c) {
            return illegal_chars.find(c) != std::string::npos;
        }), result.end());

    // Replace multiple underscores with single underscore
    auto new_end = std::unique(result.begin(), result.end(),
        [](char a, char b) { return a == '_' && b == '_'; });
    result.erase(new_end, result.end());

    // Remove leading/trailing underscores
    while (!result.empty() && result.front() == '_') result.erase(0, 1);
    while (!result.empty() && result.back() == '_') result.pop_back();

    // Ensure filename is not empty
    if (result.empty()) {
        result = "transcoded_video";
    }

    return result;
}

// Helper function to expand ffmpeg pattern into individual file paths
static std::vector<std::string> BuildFileListFromPattern(
    const std::string& ffmpeg_pattern,
    int start_frame,
    int end_frame
) {
    std::vector<std::string> files;

    // Find the %d pattern in the ffmpeg_pattern
    // Pattern will be like: /path/shot_%04d.exr or /path/shot_%d.exr
    std::regex pattern_regex(R"(%0?(\d*)d)");
    std::smatch match;

    if (!std::regex_search(ffmpeg_pattern, match, pattern_regex)) {
        Debug::Log("ERROR: Could not find frame number pattern in: " + ffmpeg_pattern);
        return files;
    }

    // Extract padding from pattern (%04d -> padding=4, %d -> padding=1)
    int padding = 1;
    if (!match[1].str().empty()) {
        padding = std::stoi(match[1].str());
    }

    // Generate file paths for each frame
    files.reserve(end_frame - start_frame + 1);
    for (int frame = start_frame; frame <= end_frame; ++frame) {
        std::ostringstream frame_str;
        frame_str << std::setfill('0') << std::setw(padding) << frame;

        std::string file_path = std::regex_replace(
            ffmpeg_pattern,
            pattern_regex,
            frame_str.str()
        );

        files.push_back(file_path);
    }

    Debug::Log("Built file list: " + std::to_string(files.size()) + " frames (" +
               std::to_string(start_frame) + "-" + std::to_string(end_frame) + ")");

    return files;
}

// Helper struct to store extracted OCIO settings
struct OCIOSettings {
    std::string src_colorspace;
    std::string display;
    std::string view;
    std::string looks;
    std::vector<std::string> scene_luts;
    std::vector<std::string> display_luts;
    bool has_settings = false;
};

// Extract current OCIO settings from node graph
static OCIOSettings ExtractOCIOSettingsFromNodeGraph(NodeManager* node_manager) {
    OCIOSettings settings;

    if (!node_manager) {
        Debug::Log("ExtractOCIOSettingsFromNodeGraph: No node manager");
        return settings;
    }

    // Get nodes in pipeline order
    auto pipeline_nodes = node_manager->GetPipelineOrder();

    if (pipeline_nodes.empty()) {
        Debug::Log("ExtractOCIOSettingsFromNodeGraph: Empty pipeline");
        return settings;
    }

    Debug::Log("ExtractOCIOSettingsFromNodeGraph: Found " + std::to_string(pipeline_nodes.size()) + " nodes in pipeline");

    // Walk through the pipeline and extract settings
    for (auto* node : pipeline_nodes) {
        if (!node) continue;

        NodeType type = node->GetType();

        if (type == NodeType::INPUT_COLORSPACE) {
            auto* input_node = dynamic_cast<InputColorSpaceNode*>(node);
            if (input_node) {
                settings.src_colorspace = input_node->GetColorSpace();
                Debug::Log("  Found input colorspace: " + settings.src_colorspace);
            }
        }
        else if (type == NodeType::OUTPUT_DISPLAY) {
            auto* display_node = dynamic_cast<OutputDisplayNode*>(node);
            if (display_node) {
                // Try to use aliases first (config-portable), fall back to full names
                std::string display_alias = display_node->GetDisplayAlias();
                std::string view_alias = display_node->GetViewAlias();

                if (!display_alias.empty() && ocio_manager && ocio_manager->IsConfigLoaded()) {
                    // Resolve alias to current config's full name
                    settings.display = ocio_manager->ResolveAlias(display_alias);
                    Debug::Log("  Found output display alias: " + display_alias + " -> " + settings.display);
                } else {
                    // No alias stored (old project) or no OCIO manager - use full name directly
                    settings.display = display_node->GetDisplay();
                    Debug::Log("  Found output display (no alias): " + settings.display);
                }

                if (!view_alias.empty() && ocio_manager && ocio_manager->IsConfigLoaded()) {
                    settings.view = ocio_manager->ResolveAlias(view_alias);
                    Debug::Log("  Found view alias: " + view_alias + " -> " + settings.view);
                } else {
                    settings.view = display_node->GetView();
                    Debug::Log("  Found view (no alias): " + settings.view);
                }
            }
        }
        else if (type == NodeType::LOOK) {
            auto* look_node = dynamic_cast<LookNode*>(node);
            if (look_node) {
                // Append look to looks string (comma-separated if multiple)
                std::string look = look_node->GetLook();
                if (!look.empty()) {
                    if (!settings.looks.empty()) {
                        settings.looks += ",";
                    }
                    settings.looks += look;
                    Debug::Log("  Found look: " + look);
                }
            }
        }
        else if (type == NodeType::SCENE_LUT) {
            auto* lut_node = dynamic_cast<SceneLUTNode*>(node);
            if (lut_node) {
                settings.scene_luts.push_back(lut_node->GetLUTPath());
                Debug::Log("  Found scene LUT: " + lut_node->GetLUTFileName());
            }
        }
        else if (type == NodeType::DISPLAY_LUT) {
            auto* lut_node = dynamic_cast<DisplayLUTNode*>(node);
            if (lut_node) {
                settings.display_luts.push_back(lut_node->GetLUTPath());
                Debug::Log("  Found display LUT: " + lut_node->GetLUTFileName());
            }
        }
    }

    // Valid if we have at least an input colorspace OR output display
    settings.has_settings = !settings.src_colorspace.empty() || !settings.display.empty();

    if (settings.has_settings) {
        Debug::Log("ExtractOCIOSettingsFromNodeGraph: Successfully extracted OCIO settings");
    } else {
        Debug::Log("ExtractOCIOSettingsFromNodeGraph: No valid OCIO settings found");
    }

    return settings;
}

// Helper function to determine pipeline mode from video metadata
static PipelineMode DeterminePipelineModeFromMetadata(const VideoMetadata* video_meta) {
    if (!video_meta) {
        return PipelineMode::NORMAL;  // Fallback
    }

    // HDR content → HDR_RES mode (uses RGBA16F for HDR processing)
    if (video_meta->is_hdr_content) {
        Debug::Log("Pipeline mode: HDR_RES (HDR content detected)");
        return PipelineMode::HDR_RES;
    }

    // 10-bit+ content → HIGH_RES mode (uses RGBA16 integer)
    // Handles: 10-bit (yuv420p10le), 12-bit ProRes 4444/4444XQ (yuva444p12le)
    if (video_meta->bit_depth >= 10) {
        Debug::Log("Pipeline mode: HIGH_RES (" + std::to_string(video_meta->bit_depth) + "-bit content)");
        return PipelineMode::HIGH_RES;
    }

    // 8-bit content → NORMAL mode (uses RGBA8)
    Debug::Log("Pipeline mode: NORMAL (8-bit content)");
    return PipelineMode::NORMAL;
}

void ProjectManager::ProcessAddToTranscodeQueue() {
    if (!transcode_queue_) {
        Debug::Log("ERROR: Transcode queue not initialized");
        return;
    }

    auto& settings = transcode_dialog_settings;

    // Get selected items
    auto selected_items = GetSelectedItems();

    Debug::Log("ProjectManager: Creating " + std::to_string(selected_items.size()) + " transcode jobs");

    int jobs_created = 0;

    for (const auto& item : selected_items) {
        // For demonstration, create empty job config
        TranscodeJob::Config job_config;
        job_config.job_name = item.name;

        // Track if we're trimming (for timecode offset)
        bool has_trim_offset = false;
        int trim_offset_frames = 0;

        // For sequences: store actual file frame range when trimmed with in/out points
        int trimmed_file_start_frame = -1;
        int trimmed_file_end_frame = -1;
        int trimmed_file_frame_count = -1;

        // Set priority
        switch (settings.priority_index) {
            case 0: job_config.priority = TranscodeJob::Priority::LOW; break;
            case 1: job_config.priority = TranscodeJob::Priority::NORMAL; break;
            case 2: job_config.priority = TranscodeJob::Priority::HIGH; break;
            case 3: job_config.priority = TranscodeJob::Priority::URGENT; break;
        }

        // Configure transcode settings based on media type
        auto& tc = job_config.transcode_config;

        if (item.type == MediaType::VIDEO) {
            // Video file mode
            Debug::Log("Video: " + item.name);

            tc.input_mode = VideoTranscoder::InputMode::VIDEO_FILE;
            tc.input_video_path = item.path;
            tc.fps = item.frame_rate > 0 ? item.frame_rate : 24.0;  // Use video's native FPS

            // Use precise FFmpeg duration for transcoder (not rounded through frame count)
            double precise_duration = ump::FFmpegMetadataExtractor::ProbeDuration(item.path);
            tc.video_duration = (precise_duration > 0) ? precise_duration : item.duration;
            Debug::Log("Transcode: Using precise duration = " + std::to_string(tc.video_duration) + "s (item.duration was " + std::to_string(item.duration) + "s)");

            tc.start_frame = 0;
            tc.end_frame = -1;  // Process entire video

            // Apply In/Out point range if both are set on this MediaItem
            if (item.in_point >= 0.0 && item.out_point >= 0.0) {
                double in_time = item.in_point;
                double out_time = item.out_point;

                // Convert timestamps to frame numbers
                tc.start_frame = static_cast<int>(std::round(in_time * tc.fps));
                tc.end_frame = static_cast<int>(std::round(out_time * tc.fps));

                // NOTE: Do NOT modify tc.video_duration - it must remain the source duration
                // The transcoder uses it to calculate max_frame_index from the source file
                // start_frame and end_frame already define the output trim range

                Debug::Log("Applying In/Out range for \"" + item.name + "\": " + std::to_string(in_time) + "s to " + std::to_string(out_time) + "s" +
                           " (frames " + std::to_string(tc.start_frame) + " to " + std::to_string(tc.end_frame) + ")");

                // Store offset for timecode adjustment (will be used later in job config)
                has_trim_offset = true;
                trim_offset_frames = tc.start_frame;
            }

            // Determine pipeline mode for video transcode
            // Strategy: Use current player mode if video is loaded, otherwise auto-detect
            PipelineMode video_pipeline_mode = PipelineMode::NORMAL;

            // Check if this is the currently loaded video
            if (current_file_path && *current_file_path == item.path && video_player) {
                // Use user's selected pipeline mode from View menu
                video_pipeline_mode = video_player->GetPipelineMode();
                Debug::Log("Transcode (video): Using current player pipeline mode: " +
                           std::string(PipelineModeToString(video_pipeline_mode)));
            } else {
                // Auto-detect from cached video metadata
                auto metadata = GetCachedMetadata(item.path);
                if (metadata && metadata->video_meta) {
                    video_pipeline_mode = DeterminePipelineModeFromMetadata(metadata->video_meta.get());
                    Debug::Log("Transcode (video): Auto-detected from metadata - " +
                               std::string(PipelineModeToString(video_pipeline_mode)) +
                               " (bit_depth=" + std::to_string(metadata->video_meta->bit_depth) +
                               ", pixel_format=" + metadata->video_meta->pixel_format + ")");
                } else {
                    Debug::Log("Transcode (video): No metadata available, defaulting to NORMAL mode");
                }
            }

            tc.pipeline_mode = video_pipeline_mode;

            Debug::Log("Transcode config (video): fps=" + std::to_string(tc.fps) +
                       ", duration=" + std::to_string(tc.video_duration) + "s" +
                       ", frames=" + std::to_string(static_cast<int>(tc.video_duration * tc.fps)) +
                       ", pipeline_mode=" + std::string(PipelineModeToString(tc.pipeline_mode)));
        } else if (item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) {
            // Image sequence mode
            Debug::Log("Sequence: " + item.name);

            // Build file list for sequence from MediaItem's ffmpeg_pattern
            std::vector<std::string> input_files = BuildFileListFromPattern(
                item.ffmpeg_pattern,
                item.start_frame,
                item.end_frame
            );

            if (input_files.empty()) {
                Debug::Log("ERROR: Failed to build file list for: " + item.name);
                continue;
            }

            tc.input_mode = VideoTranscoder::InputMode::IMAGE_SEQUENCE;
            tc.input_files = input_files;
            tc.fps = item.frame_rate;  // Use frame rate from MediaItem
            tc.pipeline_mode = item.pipeline_mode;  // Use auto-detected pipeline mode
            tc.exr_layer = item.exr_layer;  // Use EXR layer from MediaItem
            tc.start_frame = 0;  // Always start from first frame in the file list
            tc.end_frame = -1;   // -1 means process all frames in the list

            // Apply In/Out point range if both are set on this MediaItem
            if (item.in_point >= 0.0 && item.out_point >= 0.0) {
                double in_time = item.in_point;
                double out_time = item.out_point;

                // Convert timestamps to playback frame indices (0-based from video start)
                int in_frame_index = static_cast<int>(std::round(in_time * tc.fps));
                int out_frame_index = static_cast<int>(std::round(out_time * tc.fps));

                // Clamp to sequence bounds
                if (in_frame_index < 0) in_frame_index = 0;
                if (out_frame_index >= static_cast<int>(input_files.size())) {
                    out_frame_index = static_cast<int>(input_files.size()) - 1;
                }

                // Calculate actual file frame numbers for filename generation
                int file_start_frame = item.start_frame + in_frame_index;
                int file_end_frame = item.start_frame + out_frame_index;
                int file_frame_count = out_frame_index - in_frame_index + 1;

                Debug::Log("Applying In/Out range for \"" + item.name + "\": " + std::to_string(in_time) + "s to " + std::to_string(out_time) + "s" +
                           " (array indices " + std::to_string(in_frame_index) + " to " + std::to_string(out_frame_index) +
                           ", file frames " + std::to_string(file_start_frame) + " to " + std::to_string(file_end_frame) + ")");

                // Create a subset of input_files containing only the trimmed range
                // This ensures the SequentialFrameLoader loads the correct frames
                std::vector<std::string> trimmed_files(
                    input_files.begin() + in_frame_index,
                    input_files.begin() + out_frame_index + 1
                );
                tc.input_files = trimmed_files;

                Debug::Log("Trimmed input_files from " + std::to_string(input_files.size()) +
                           " to " + std::to_string(trimmed_files.size()) + " files");

                // Store offset for timecode adjustment (use the original start index)
                has_trim_offset = true;
                trim_offset_frames = in_frame_index;

                // Store file frame range for filename update
                trimmed_file_start_frame = file_start_frame;
                trimmed_file_end_frame = file_end_frame;
                trimmed_file_frame_count = file_frame_count;

                // Reset start/end to process all frames in the trimmed subset
                tc.start_frame = 0;
                tc.end_frame = -1;  // -1 means process all frames in trimmed list
            }

            Debug::Log("Transcode config (sequence): fps=" + std::to_string(tc.fps) +
                       ", frames=" + std::to_string(tc.input_files.size()) +
                       ", pipeline_mode=" + std::string(PipelineModeToString(tc.pipeline_mode)) +
                       (tc.exr_layer.empty() ? "" : ", exr_layer=" + tc.exr_layer));
        } else {
            Debug::Log("WARNING: Skipping unsupported media type: " + item.name);
            continue;
        }

        // Codec settings with hardware encoding support
        if (settings.codec_index == 0 || settings.codec_index == 1) {
            // H.264 / H.265 - use hardware encoding if available
            std::string base_codec = (settings.codec_index == 0) ? "h264" : "h265";
            tc.encoder_settings.codec = FFMPEGVideoEncoder::SelectBestEncoder(
                base_codec,
                transcode_settings.prefer_hardware_encoding,
                transcode_settings.hardware_encoder
            );

            tc.encoder_settings.crf = settings.crf;

            // Note: Hardware encoders have different preset options
            // libx264/libx265: ultrafast, fast, slow, veryslow
            // NVENC/QSV: may not use preset the same way
            const char* preset_names[] = {"ultrafast", "fast", "slow", "veryslow"};
            tc.encoder_settings.preset = preset_names[settings.preset_index];
            tc.encoder_settings.pixel_format = "yuv420p";
        } else if (settings.codec_index == 2) {
            // ProRes 422 LT - no hardware acceleration
            tc.encoder_settings.codec = "prores_ks";
            tc.encoder_settings.pixel_format = "yuv422p10le";
            tc.encoder_settings.prores_profile = 1;  // LT
        } else if (settings.codec_index == 3) {
            // ProRes 422 HQ - no hardware acceleration
            tc.encoder_settings.codec = "prores_ks";
            tc.encoder_settings.pixel_format = "yuv422p10le";
            tc.encoder_settings.prores_profile = 3;  // HQ
        } else if (settings.codec_index == 4) {
            // ProRes 4444 - no hardware acceleration
            tc.encoder_settings.codec = "prores_ks";
            tc.encoder_settings.pixel_format = "yuva444p10le";
            tc.encoder_settings.prores_profile = 4;  // 4444
        }

        // Performance: encoder threading
        tc.encoder_settings.thread_count = transcode_settings.encoder_thread_count;

        // Resolution
        switch (settings.resolution_mode) {
            case 0:  // Source
                tc.output_width = -1;
                tc.output_height = -1;
                break;
            case 1:  // 4K
                tc.output_width = 3840;
                tc.output_height = 2160;
                break;
            case 2:  // 1080p
                tc.output_width = 1920;
                tc.output_height = 1080;
                break;
            case 3:  // Custom
                tc.output_width = settings.custom_width;
                tc.output_height = settings.custom_height;
                break;
        }

        // OCIO settings - extract from node graph
        if (settings.use_current_ocio) {
            NodeManager* node_manager = static_cast<NodeManager*>(GetNodeManager());

            // CRITICAL: Switch to the config that was used when creating the nodes
            // This ensures display/view names are valid for that config
            if (node_manager && ocio_manager) {
                OCIOConfigType required_config = node_manager->GetActiveOCIOConfig();
                if (!ocio_manager->SwitchToConfig(required_config)) {
                    Debug::Log("WARNING: Failed to switch to required OCIO config for transcode");
                } else {
                    Debug::Log("Switched to config: " + std::to_string(static_cast<int>(required_config)) + " for transcode");
                }
            }

            OCIOSettings ocio = ExtractOCIOSettingsFromNodeGraph(node_manager);

            if (ocio.has_settings) {
                tc.src_colorspace = ocio.src_colorspace;
                tc.display = ocio.display;
                tc.view = ocio.view;
                tc.looks = ocio.looks;
                tc.scene_luts = ocio.scene_luts;
                tc.display_luts = ocio.display_luts;

                Debug::Log("Using OCIO settings from node graph:");
                Debug::Log("  Colorspace: " + tc.src_colorspace);
                Debug::Log("  Display: " + tc.display + " / " + tc.view);
                if (!tc.looks.empty()) {
                    Debug::Log("  Looks: " + tc.looks);
                }
                if (!tc.scene_luts.empty()) {
                    Debug::Log("  Scene LUTs: " + std::to_string(tc.scene_luts.size()));
                }
                if (!tc.display_luts.empty()) {
                    Debug::Log("  Display LUTs: " + std::to_string(tc.display_luts.size()));
                }
            } else {
                Debug::Log("WARNING: No OCIO settings found in node graph - using passthrough");
                tc.src_colorspace = "";
                tc.display = "";
                tc.view = "";
            }
        } else {
            // User unchecked the box - use passthrough
            tc.src_colorspace = "";
            tc.display = "";
            tc.view = "";
        }

        // Output path
        std::string output_dir(settings.output_directory);

        // Create output directory if it doesn't exist
        try {
            std::filesystem::path dir_path(output_dir);
            if (!std::filesystem::exists(dir_path)) {
                std::filesystem::create_directories(dir_path);
                Debug::Log("Created output directory: " + output_dir);
            }
        } catch (const std::exception& e) {
            Debug::Log("ERROR: Failed to create output directory: " + std::string(e.what()));
            continue;  // Skip this job
        }

        // Strip extension from video files before adding new extension
        std::string base_name = item.name;
        if (item.type == MediaType::VIDEO) {
            // Remove existing extension (e.g., "my_video.mov" -> "my_video")
            std::filesystem::path p(item.name);
            base_name = p.stem().string();
        }

        // For sequences with in/out points: update filename to reflect trimmed frame range
        if ((item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) &&
            trimmed_file_start_frame >= 0) {

            // Pattern: "BaseName [N frames START-END] - Layer" -> "BaseName [N frames START-END] - Layer"
            // Replace the frame count and range with actual trimmed values
            std::regex frame_range_pattern(R"(\[\d+\s+frames\s+\d+-\d+\])");
            std::string new_range = "[" + std::to_string(trimmed_file_frame_count) + " frames " +
                                   std::to_string(trimmed_file_start_frame) + "-" +
                                   std::to_string(trimmed_file_end_frame) + "]";
            base_name = std::regex_replace(base_name, frame_range_pattern, new_range);

            Debug::Log("Updated filename for trimmed sequence: " + base_name);
        }

        std::string safe_name = SanitizeFilename(base_name);
        std::string extension = (settings.codec_index >= 2) ? ".mov" : ".mp4";
        std::string output_filename = safe_name + extension;

        tc.output_path = output_dir + "/" + output_filename;

        // Safety: Check if file exists and add suffix if needed (-1, -2, etc.)
        if (std::filesystem::exists(tc.output_path)) {
            std::string original_output = tc.output_path;
            int suffix = 1;

            while (std::filesystem::exists(tc.output_path) && suffix < 1000) {
                // Construct new path: "filename-1.ext", "filename-2.ext", etc.
                tc.output_path = output_dir + "/" + safe_name + "-" + std::to_string(suffix) + extension;
                suffix++;
            }

            Debug::Log("File exists, renamed: '" + output_filename + "' -> '" +
                       std::filesystem::path(tc.output_path).filename().string() + "'");
        } else {
            Debug::Log("Sanitized filename: '" + item.name + "' -> '" + safe_name + extension + "'");
        }

        // Set source file path for metadata preservation
        job_config.source_file_path = item.path;

        // Set timecode offset for trimmed transcodes
        if (has_trim_offset) {
            job_config.timecode_offset_frames = trim_offset_frames;
            Debug::Log("Set timecode offset: " + std::to_string(trim_offset_frames) + " frames");
        }

        // Set metadata write callback (preserves Adobe project links and timecodes)
        job_config.metadata_write_callback = [this](const std::string& source_path, const std::string& output_path, int offset_frames) {
            Debug::Log("Metadata callback: Looking up cached metadata for: " + source_path);

            // Lookup cached metadata for source file
            const CombinedMetadata* metadata = GetCachedMetadata(source_path);
            if (metadata && metadata->adobe_meta) {
                Debug::Log("Metadata callback: Found cached metadata, writing to: " + output_path);

                // Write Adobe metadata to output file using ExifTool (with timecode offset)
                bool success = AdobeMetadataExtractor::WriteMetadata(output_path, metadata->adobe_meta.get(), offset_frames);

                if (success) {
                    Debug::Log("Metadata callback: Successfully wrote metadata to transcoded file");
                } else {
                    Debug::Log("Metadata callback: Failed to write metadata (ExifTool error)");
                }
            } else {
                Debug::Log("Metadata callback: No cached metadata found for source file");
            }
        };

        // Create job
        auto job = std::make_unique<TranscodeJob>(job_config);

        // Add to queue
        transcode_queue_->AddJob(std::move(job));

        jobs_created++;
    }

    Debug::Log("ProjectManager: Created " + std::to_string(jobs_created) + " transcode jobs");

    // Show queue window
    ShowTranscodeQueueWindow();
}

} // namespace ump
