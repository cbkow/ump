// ============================================================================
// PLATFORM AND SYSTEM INCLUDES
// ============================================================================
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <VersionHelpers.h>
#include <shobjidl.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

// ============================================================================
// GRAPHICS AND UI INCLUDES
// ============================================================================
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>
#include <nfd.h>
#define STB_IMAGE_IMPLEMENTATION
#include "../external/glfw/deps/stb_image.h"
#include "../external/glfw/deps/stb_image_write.h"

// ============================================================================
// STANDARD LIBRARY INCLUDES
// ============================================================================
#include <iostream>
#include <memory>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <future>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <map>
#include <filesystem>
#include <nlohmann/json.hpp>

// ============================================================================
// FFMPEG INCLUDES (for base64 encoding)
// ============================================================================
extern "C" {
#include <libavutil/base64.h>
}

// ============================================================================
// PROJECT INCLUDES
// ============================================================================
#include "player/video_player.h"
#include "player/thumbnail_cache.h"
#include "utils/exiftool_helper.h"
#include "utils/debug_utils.h"
#include "utils/frame_indexing.h"
#include "utils/system_pressure_monitor.h"
#include "project/project_manager.h"
#include "imnodes/imnodes.h"
#include "color/ocio_config_manager.h"
#include "ui/node_editor_theme.h"
#include "nodes/node_manager.h"
#include "overlay/safety_overlay_system.h"
#include "nodes/node_base.h"
#include "color/ocio_pipeline.h"
#include "ui/timeline_manager.h"
#include "annotations/annotation_manager.h"
#include "ui/annotation_panel.h"
#include "annotations/viewport_annotator.h"
#include "annotations/annotation_toolbar.h"
#include "annotations/annotation_renderer.h"
#include "annotations/stroke_smoother.h"
#include "annotations/annotation_serializer.h"
#include "annotations/annotation_exporter.h"
#include "integrations/frameio_url_parser.h"
#include "integrations/frameio_client.h"
#include "integrations/frameio_converter.h"

// ============================================================================
// COLOR INCLUDES
// ============================================================================
std::unique_ptr<OCIOConfigManager> ocio_manager;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
ImFont* font_regular = nullptr;
ImFont* font_mono = nullptr;
ImFont* font_icons = nullptr;

// Windows accent color toggle state
bool use_windows_accent_color = false;

// ============================================================================
// TIMECODE MODE VARIABLES
// ============================================================================
bool timecode_mode_enabled = false;
std::string cached_start_timecode = "";
bool start_timecode_checked = false;
enum TimecodeState {
    NOT_CHECKED,        // Haven't looked for timecode yet
    CHECKING,           // Currently extracting metadata
    AVAILABLE,          // Found valid start timecode
    NOT_AVAILABLE       // No start timecode found
};
TimecodeState timecode_state = NOT_CHECKED;

// ============================================================================
// CACHE CONTROL VARIABLES
// ============================================================================
bool cache_enabled = true;
bool show_cache_settings = false;

// Pipeline-aware cache settings
static struct {
    // SHARED SETTINGS (apply to both video and EXR)
    int max_cache_size_mb = 12288;        // DEPRECATED: Individual cache size (now unlimited)
    int max_cache_seconds = 20;           // Shared: Maximum seconds to cache (video + EXR)
    PipelineMode current_pipeline_mode = PipelineMode::NORMAL;  // Current pipeline mode
    bool show_pipeline_info = true;       // Show pipeline impact info

    // VIDEO-SPECIFIC SETTINGS (FFmpeg/MPV only)
    bool enable_nvidia_decode = false;    // NVIDIA hardware decode setting
    int max_batch_size = 8;               // Frames per extraction batch
    int max_concurrent_batches = 8;       // Number of parallel extraction threads

    // EXR SEQUENCE SETTINGS - Auto-configured based on CPU
    int exr_oiio_threads = 8;             // EXR I/O worker thread count (1-16) - auto-detected (will be 8-16 for modern CPUs)
    int exr_oiio_threads_per_worker = 1;  // EXR internal threads per worker (NOT used in PER_WORKER mode)
    int exr_threading_mode = 1;           // EXR decompression threading: 0=DISABLED, 1=PER_WORKER (1:1 fastest!), 2=AUTO

    // GPU MEMORY SETTINGS
    int gpu_memory_pool_mb = 2048;        // GPU VRAM limit for texture caching in MB (512-8192)

    // THUMBNAIL SCRUBBING SETTINGS
    bool enable_thumbnails = true;        // Enable/disable thumbnail scrubbing on timeline hover
    int thumbnail_width = 320;            // Thumbnail width in pixels (160-640)
    int thumbnail_height = 180;           // Thumbnail height in pixels (90-360)
    int thumbnail_cache_size = 100;       // Number of thumbnails to keep in RAM (50-500)

    // PLAYBACK SETTINGS
    bool auto_play_on_load = false;       // Auto-play videos after loading (with 500ms delay)
} cache_settings;

// ============================================================================
// AUTO-PLAY ON LOAD STATE
// ============================================================================
bool pending_auto_play = false;
std::chrono::steady_clock::time_point auto_play_timer;

// ============================================================================
// EXR TRANSCODE PROGRESS STATE
// ============================================================================
bool show_transcode_progress = false;
std::atomic<int> transcode_current_frame{0};
std::atomic<int> transcode_total_frames{0};
std::string transcode_status_message = "";
std::mutex transcode_status_mutex;

// ============================================================================
// CACHE CLEAR DIALOG STATE
// ============================================================================
bool show_cannot_clear_exr_cache_error = false;
bool show_exr_cache_cleared_success = false;
size_t exr_cache_bytes_cleared = 0;

// ============================================================================
// SYSTEM PRESSURE MONITOR STATE
// ============================================================================
static std::unique_ptr<ump::SystemPressureMonitor> pressure_monitor;
static bool show_pressure_warning_banner = false;  // Deprecated - no longer used
static bool show_pressure_critical_dialog = false;
static bool in_emergency_mode = false;  // Track if we're in critical emergency state
static ump::SystemPressureStatus last_pressure_status;

// Stats bar notification system
static std::string stats_bar_notification_message;
static std::chrono::steady_clock::time_point notification_start_time;
static float notification_timeout_seconds = 8.0f;  // Auto-dismiss after 8 seconds (for recovery message)
static bool show_notification_permanent = false;   // true = permanent (critical), false = timed (recovery)
static ImGuiID saved_stats_dock_id = 0;            // Store dock ID from layout setup

// Stats bar cached status (refresh every 500ms to reduce mutex locks)
static ump::SystemPressureStatus cached_stats_bar_status;
static std::chrono::steady_clock::time_point last_stats_bar_refresh;
static constexpr double stats_bar_refresh_interval = 0.5;  // 500ms = 2Hz refresh rate

// Track if EXR cache was shut down during emergency (to restore on recovery)
static bool exr_cache_was_active = false;
static std::string exr_video_path_before_shutdown;

// Auto-configure EXR threading settings based on CPU capabilities
void AutoConfigureEXRThreading(decltype(cache_settings)& settings) {
    int hardware_threads = static_cast<int>(std::thread::hardware_concurrency());

    if (hardware_threads <= 0) {
        printf("EXR Auto-Config: Unable to detect CPU cores, using defaults\n");
        return;
    }

    printf("EXR Auto-Config: Detected %d hardware threads\n", hardware_threads);

    // Configure EXR I/O worker threads - AGGRESSIVE settings for max throughput
    // With generation tagging + PER_WORKER mode (1:1 mapping), we can use ALL cores
    if (hardware_threads <= 4) {
        settings.exr_oiio_threads = std::max(1, hardware_threads - 1);  // 4 cores ? 3 workers
    } else if (hardware_threads <= 8) {
        settings.exr_oiio_threads = hardware_threads;  // 8 cores ? 8 workers
    } else if (hardware_threads <= 16) {
        settings.exr_oiio_threads = hardware_threads - 2;  // 16 cores ? 14 workers (leave room for OS)
    } else {
        settings.exr_oiio_threads = 16;  // 32+ cores ? cap at 16 workers (reasonable limit)
    }

    // Ensure bounds
    settings.exr_oiio_threads = std::clamp(settings.exr_oiio_threads, 1, 16);

    printf("EXR Auto-Config: %d OIIO worker threads (PER_WORKER 1:1 mode + generation tagging)\n",
           settings.exr_oiio_threads);
}

// Create DirectEXRCacheConfig from current cache_settings (NEW: Simplified config)
// Global EXR cache settings (visible to project_manager)
double g_exr_cache_gb = 18.0;
float g_read_behind_seconds = 0.5f;
int g_exr_thread_count = 16;  // DirectEXRCache parallel I/O threads
int g_exr_transcode_threads = 8;  // EXRTranscoder parallel transcode threads

// Global disk cache settings
std::string g_custom_cache_path = "";  // Empty = use default %LOCALAPPDATA%
int g_cache_retention_days = 7;  // Auto-cleanup files older than N days
int g_dummy_cache_max_gb = 1;  // Dummy video cache size limit
int g_transcode_cache_max_gb = 10;  // EXR transcode cache size limit
bool g_clear_cache_on_exit = false;  // Clear all cache on app exit

ump::DirectEXRCacheConfig GetCurrentEXRCacheConfig() {
    ump::DirectEXRCacheConfig config;

    // Use UI-configured values
    config.cacheGB = g_exr_cache_gb;
    config.readBehindSeconds = g_read_behind_seconds;
    config.threadCount = static_cast<size_t>(g_exr_thread_count);

    return config;
}

ump::ThumbnailConfig GetCurrentThumbnailConfig() {
    ump::ThumbnailConfig config;

    // Use cache_settings values
    config.width = cache_settings.thumbnail_width;
    config.height = cache_settings.thumbnail_height;
    config.cache_size = cache_settings.thumbnail_cache_size;
    config.enabled = cache_settings.enable_thumbnails;

    return config;
}

// ============================================================================
// ICON DEFINITIONS
// ============================================================================
#define ICON_SKIP_PREVIOUS          u8"\uE045"
#define ICON_FAST_REWIND            u8"\uE020"
#define ICON_ARROW_LEFT             u8"\uE5CB"
#define ICON_PLAY_ARROW             u8"\uE037"
#define ICON_PAUSE                  u8"\uE034"
#define ICON_ARROW_RIGHT            u8"\uE5CC"
#define ICON_FAST_FORWARD           u8"\uE01F"
#define ICON_SKIP_NEXT              u8"\uE044"
#define ICON_FOLDER_OPEN            u8"\uE2C8"
#define ICON_CONTENT_COPY           u8"\xE14D"
#define ICON_ARTICLE                u8"\uEF42"
#define ICON_SCREENSHOT_CLIPBOARD   u8"\uF7d3"
#define ICON_SCREENSHOT_DESKTOP     u8"\uF727"
#define ICON_FULLSCREEN             u8"\uE5d0"
#define ICON_TONALITY               u8"\uE427"
#define ICON_MASK                   u8"\uE72E"
#define ICON_WINDOWS                u8"\uE6FA"
#define ICON_TIMECODE               u8"\uE264"
#define ICON_SPLIT_SCREEN_LEFT      u8"\uF675"
#define ICON_SPLIT_SCREEN_BOTTOM    u8"\uF676"
#define ICON_BACK                   u8"\uE5C4"
#define ICON_FORWARD                u8"\uE5C8"
#define ICON_VIEW_TIMELINE          u8"\uEB85"
#define ICON_FLOWCHART              u8"\uF38D"
#define ICON_CLOSE                  u8"\uE5CD"
#define ICON_HOURGLASS_EMPTY        u8"\uE88B"  
#define ICON_DELETE                 u8"\uE872"  
#define ICON_LIST                   u8"\uE896"
#define ICON_LOOP                   u8"\uE863"  
#define ICON_LOOP_OFF               u8"\uF826"  
#define ICON_INFO                   u8"\ue88e"           
#define ICON_LINK                   u8"\ue157"                   
#define ICON_VIDEOCAM               u8"\ue04b"

// EXR/Render Pass Icons
#define ICON_PALETTE                u8"\uE40A"   // Beauty/Combined pass
#define ICON_LIGHTBULB              u8"\uE8F4"   // Emission
#define ICON_BRIGHTNESS_6           u8"\uE40C"   // Diffuse lighting
#define ICON_AUTO_AWESOME           u8"\uE65F"   // Specular/Glossy
#define ICON_LAYERS                 u8"\uE53B"   // General layer
#define ICON_STRAIGHTEN             u8"\uE3E2"   // Normal pass
#define ICON_SQUARE_FOOT            u8"\uEA04"   // Depth/Z pass
#define ICON_GRID_ON                u8"\uE90D"   // Cryptomatte (hidden)
#define ICON_VISIBILITY_OFF         u8"\uE8F5"   // Hidden indicator
#define ICON_VOLUME_UP              u8"\ue050"
#define ICON_VIDEO_LIBRARY          u8"\ue04a"
#define ICON_PLAY_CIRCLE            u8"\ue037"
#define ICON_AUDIO_TRACK            u8"\uE3A1" 
#define ICON_MOVIE                  u8"\uE02C"
#define ICON_VOLUME_MUTE            u8"\uE04F"
#define ICON_MODEL_TRAINING         u8"\uF0CF"
#define ICON_DISPLAY_SETTINGS       u8"\uEB97"

// Annotation Drawing Icons
#define ICON_DRAW                   u8"\uE22B"   // Freehand/Brush
#define ICON_RECTANGLE              u8"\ueb36"   // Rectangle (crop_square)
#define ICON_CIRCLE                 u8"\uEF4A"   // Circle/Oval
#define ICON_ARROW_FORWARD          u8"\uE5C8"   // Arrow
#define ICON_REMOVE                 u8"\uE15B"   // Line (horizontal line icon)
#define ICON_UNDO                   u8"\uE166"   // Undo
#define ICON_REDO                   u8"\uE15A"   // Redo
#define ICON_CHECK                  u8"\uE5CA"   // Done/Check
#define ICON_CANCEL                 u8"\uE5C9"   // Cancel/X
#define ICON_DELETE_SWEEP           u8"\uE16C"   // Clear All
#define ICON_PALETTE_COLOR          u8"\uE40A"   // Color picker
#define ICON_INPUT_SETTINGS         u8"\uE8C0"
#define ICON_GRID_VIEW              u8"\uE9B0"
#define ICON_HOME_MAX               u8"\uF024"
#define ICON_BACKGROUND             u8"\uF20A"
#define ICON_NOTE_STACK             u8"\uF562"
#define ICON_CLOSE_LARGE            u8"\uE5CD"
#define ICON_WARNING                u8"\uE002"   // Warning triangle
#define ICON_DONE                   u8"\uE876"   // Done/Complete


class Application {
public:
    // ------------------------------------------------------------------------
    // CONSTRUCTOR & DESTRUCTOR
    // ------------------------------------------------------------------------
    Application() : window(nullptr), video_player(nullptr), first_time_setup(false),
        show_project_panel(true), show_inspector_panel(true),
        show_timeline_panel(true), show_transport_controls(true),
        show_status_bar(true), show_color_panels(false) {

        app_instance = this; // Set static pointer for window procedure
        node_manager = std::make_unique<ump::NodeManager>();
        timeline_manager = std::make_unique<TimelineManager>();
        annotation_manager = std::make_unique<ump::AnnotationManager>();
        annotation_panel = std::make_unique<ump::AnnotationPanel>();
        annotation_exporter = std::make_unique<ump::Annotations::AnnotationExporter>();
        viewport_annotator = std::make_unique<ump::Annotations::ViewportAnnotator>();
        annotation_toolbar = std::make_unique<ump::Annotations::AnnotationToolbar>();
        annotation_renderer = std::make_unique<ump::Annotations::AnnotationRenderer>();

        //node_manager->on_connections_changed = [this]() {
        //    Debug::Log("Connections changed - updating color pipeline");
        //    UpdateColorPipeline();
        //    };

        ImNodesEditorContext* nodes_editor_context = nullptr;

    }

    // ------------------------------------------------------------------------
    // CORE LIFECYCLE METHODS
    // ------------------------------------------------------------------------
    bool Initialize(const std::vector<std::string>& initial_files = {}) {
#ifdef _WIN32
        // Set DPI awareness for accurate window positioning on Windows
        SetProcessDPIAware();
#endif

        // Load user settings BEFORE creating window
        LoadSettings();

        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW" << std::endl;
            return false;
        }

        // Configure GLFW
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        // Create window (use saved size if available)
        window = glfwCreateWindow(saved_window_width, saved_window_height, "u.m.p.", nullptr, nullptr);
        if (!window) {
            std::cerr << "Failed to create GLFW window" << std::endl;
            glfwTerminate();
            return false;
        }

        // Apply saved window position if available
        if (has_saved_window_settings && saved_window_x >= 0 && saved_window_y >= 0) {
            glfwSetWindowPos(window, saved_window_x, saved_window_y);
            Debug::Log("Restored window position: " + std::to_string(saved_window_x) + ", " + std::to_string(saved_window_y));
        }

#ifdef _WIN32
        // Set window icon using Windows API
        HWND hwnd = glfwGetWin32Window(window);
        HICON hIcon = (HICON)LoadImage(NULL, TEXT("assets/icons/ump.ico"), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        }

        // Register window class for single-instance communication
        SetupSingleInstanceMessaging(hwnd);
#endif

        SetupDragDrop();

#ifdef _WIN32
        EnableDarkModeWindow(window);
#endif

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // Enable vsync

        // Initialize GLAD
        if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
            std::cerr << "Failed to initialize GLAD" << std::endl;
            return false;
        }

        // Setup ImGui and OCIO
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImNodes::CreateContext();
        NodeEditorTheme::ApplyDarkTheme();
        ocio_manager = std::make_unique<OCIOConfigManager>();
        ImGuiIO& io = ImGui::GetIO();

        // Disable automatic .ini file - we'll save layout manually to settings.ump
        io.IniFilename = nullptr;
        Debug::Log("ImGui layout will be saved to settings.ump (not imgui.ini)");

        // Load saved ImGui layout if we have one
        if (!saved_imgui_layout.empty()) {
            ImGui::LoadIniSettingsFromMemory(saved_imgui_layout.c_str(), saved_imgui_layout.size());
            Debug::Log("Loaded ImGui layout from settings");
        }

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable multi-viewport

        // Setup ImGui style
        LoadCustomFonts();
        SetupImGuiStyle();

        // Setup Platform/Renderer backends
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 450");

        // Initialize video player
        video_player = std::make_unique<VideoPlayer>();
        if (!video_player->Initialize()) {
            std::cerr << "Failed to initialize video player" << std::endl;
            return false;
        }

        video_player->SetupPropertyObservation();

        // Initialize project manager after video player
        project_manager = std::make_unique<ump::ProjectManager>(
            video_player.get(),  // VideoPlayer*
            &current_file_path,  // std::string*
            &show_inspector_panel, // bool*
            cache_enabled        // bool - user's cache preference
        );

        project_manager->SetVideoChangeCallback([this](const std::string& file_path) {
            this->OnVideoChanged(file_path);
            });

        // Set up cache clear callback for cross-cache eviction (EXR <-> Video)
        video_player->SetCacheClearCallback([this]() {
            if (project_manager) {
                project_manager->ClearAllCaches();
            }
            });

        // Connect TimelineManager to ProjectManager for cache access
        timeline_manager->SetProjectManager(project_manager.get());

        // Connect AnnotationPanel to AnnotationManager
        annotation_panel->SetAnnotationManager(annotation_manager.get());

        // Set up annotation panel callbacks
        annotation_panel->SetSeekCallback([this](double timestamp) {
            if (video_player) {
                video_player->Seek(timestamp);
            }
        });

        annotation_panel->SetCaptureScreenshotCallback([this](const std::string& directory_path, const std::string& filename) -> bool {
            if (!video_player) {
                Debug::Log("Cannot capture screenshot: No video player");
                return false;
            }
            return video_player->CaptureScreenshotToPath(directory_path, filename);
        });

        annotation_panel->SetGetCurrentStateCallback([this](double& timestamp, std::string& timecode, int& frame) {
            if (video_player) {
                timestamp = video_player->GetPosition();
                timecode = FormatCurrentTimecodeWithOffset(timestamp);
                frame = video_player->GetCurrentFrame();
            }
        });

        annotation_panel->SetGetBrightAccentColorCallback([this]() -> ImVec4 {
            return Bright(GetWindowsAccentColor());
        });

        annotation_panel->SetAnnotationsEnabled(&annotations_enabled);

        // Callback to check if a timecode is currently being edited
        annotation_panel->SetIsEditingCallback([this](const std::string& timecode) {
            return current_editing_timecode_ == timecode;
        });

        annotation_panel->SetEnterEditModeCallback([this](const std::string& timecode, double timestamp, int frame, const std::string& annotation_data) {
            if (!video_player || !viewport_annotator || !annotation_toolbar) {
                Debug::Log("Cannot enter edit mode: Missing required components");
                return;
            }

            // Auto-save if switching from a different annotation
            if (viewport_annotator->IsAnnotationMode() && !current_editing_timecode_.empty() && current_editing_timecode_ != timecode) {
                Debug::Log("Auto-saving current annotation before switching: " + current_editing_timecode_);

                // Finalize any active stroke being drawn
                auto active_stroke = viewport_annotator->FinalizeStroke();
                if (active_stroke) {
                    current_annotation_strokes_.push_back(*active_stroke);
                }

                // Save to annotation manager
                if (annotation_manager) {
                    std::string json_data = ump::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);
                    annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);
                    Debug::Log("Auto-saved annotation: " + current_editing_timecode_);
                }
            }

            Debug::Log("Entering annotation edit mode for: " + timecode);

            // Store which annotation we're editing
            current_editing_timecode_ = timecode;

            // Pause video at this frame
            video_player->Pause();
            video_player->Seek(timestamp);

            // Load existing strokes from JSON if they exist
            current_annotation_strokes_.clear();
            // Clear undo/redo stacks when entering edit mode
            annotation_undo_stack_.clear();
            annotation_redo_stack_.clear();

            if (!annotation_data.empty()) {
                current_annotation_strokes_ = ump::Annotations::AnnotationSerializer::JsonStringToStrokes(annotation_data);
                Debug::Log("Loaded " + std::to_string(current_annotation_strokes_.size()) + " existing strokes");
            } else {
                Debug::Log("No existing strokes - starting with blank canvas");
            }

            // Enter annotation mode
            viewport_annotator->SetMode(ump::Annotations::ViewportMode::ANNOTATION);

            // Set freehand tool as default
            viewport_annotator->SetActiveTool(ump::Annotations::DrawingTool::FREEHAND);

            // Show the toolbar
            annotation_toolbar->SetVisible(true);

            Debug::Log("Edit mode activated - you can now draw on the viewport");
        });

        // Callback to exit edit mode (saves and exits)
        annotation_panel->SetExitEditModeCallback([this]() {
            if (!viewport_annotator || !annotation_toolbar) {
                Debug::Log("Cannot exit edit mode: Missing required components");
                return;
            }

            Debug::Log("Exiting annotation edit mode");

            // Finalize any active stroke being drawn
            auto active_stroke = viewport_annotator->FinalizeStroke();
            if (active_stroke) {
                current_annotation_strokes_.push_back(*active_stroke);
            }

            // Save to annotation manager
            if (annotation_manager && !current_editing_timecode_.empty()) {
                std::string json_data = ump::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);
                annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);
                Debug::Log("Saved annotation: " + current_editing_timecode_);
            }

            // Exit annotation mode
            viewport_annotator->SetMode(ump::Annotations::ViewportMode::PLAYBACK);
            annotation_toolbar->SetVisible(false);
            current_editing_timecode_.clear();
            current_annotation_strokes_.clear();

            Debug::Log("Edit mode deactivated");
        });

        // Callback for export
        annotation_panel->SetExportCallback([this](const std::string& format) {
            if (!annotation_manager || !video_player || !annotation_exporter) {
                Debug::Log("Cannot export: Missing required components");
                return;
            }

            const auto& notes = annotation_manager->GetNotes();
            if (notes.empty()) {
                Debug::Log("Cannot export: No notes to export");
                return;
            }

            // Check if export is already in progress
            if (export_state.active) {
                Debug::Log("Export already in progress");
                return;
            }

            // Show file picker for export location
            nfdu8char_t* out_path = nullptr;
            nfdresult_t result = NFD_PickFolderU8(&out_path, nullptr);

            if (result != NFD_OKAY) {
                if (result == NFD_ERROR) {
                    Debug::Log("Export cancelled: File picker error");
                }
                return;
            }

            std::string export_directory = out_path;
            NFD_FreePathU8(out_path);

            // Determine export format
            ump::Annotations::AnnotationExporter::ExportFormat export_format;
            if (format == "markdown") {
                export_format = ump::Annotations::AnnotationExporter::ExportFormat::MARKDOWN;
            } else if (format == "html") {
                export_format = ump::Annotations::AnnotationExporter::ExportFormat::HTML;
            } else if (format == "pdf") {
                export_format = ump::Annotations::AnnotationExporter::ExportFormat::PDF;
            } else {
                Debug::Log("Unknown export format: " + format);
                return;
            }

            // Set up export options
            ump::Annotations::AnnotationExporter::ExportOptions options;
            std::string current_path = project_manager->GetCurrentClipName();
            options.media_name = std::filesystem::path(current_path).filename().string();
            options.media_path = current_path;
            options.output_directory = export_directory;
            options.format = export_format;
            options.frame_rate = video_player->GetFrameRate();
            options.duration = video_player->GetDuration();
            options.width = video_player->GetVideoWidth();
            options.height = video_player->GetVideoHeight();

            // Create temporary directory for captured images
            std::string timestamp = annotation_exporter->GenerateTimestamp();
            std::string sanitized_name = annotation_exporter->SanitizeFilename(options.media_name);
            std::filesystem::path temp_dir_path = std::filesystem::temp_directory_path() /
                ("ump_export_" + timestamp);

            std::filesystem::create_directories(temp_dir_path);
            std::string temp_dir = temp_dir_path.string();

            // Start export state machine
            Debug::Log("Starting " + format + " export to: " + export_directory);
            StartExport(export_format, options, notes, temp_dir);
        });

        // Callback for Frame.io import
        annotation_panel->SetFrameioImportCallback([this]() {
            frameio_import_state.show_dialog = true;
            frameio_import_state.status_message = "";
            frameio_import_state.import_success = false;
        });

        // Note: Cache limits are now managed per-video using seconds-based windows

        // Process command-line file arguments (if any)
        if (!initial_files.empty()) {
            Debug::Log("Processing " + std::to_string(initial_files.size()) + " file(s) from command-line");

            if (initial_files.size() == 1) {
                std::string arg = initial_files[0];

                // Check if it's a ump:// URI
                if (arg.substr(0, 7) == "ump:///") {
                    Debug::Log("Detected ump:// URI from command-line");
                    std::string project_path = ParseProjectURI(arg);
                    if (!project_path.empty()) {
                        Debug::Log("Parsed project path from URI: " + project_path);
                        project_manager->LoadProject(project_path);

                        // Show project panels for context
                        show_project_panel = true;
                        show_inspector_panel = true;
                        Debug::Log("Opened Project Manager and Inspector panels");
                    } else {
                        Debug::Log("ERROR: Failed to parse URI: " + arg);
                    }
                }
                // Direct project file
                else if (arg.find(".umproj") != std::string::npos) {
                    Debug::Log("Loading project file from command-line: " + arg);
                    project_manager->LoadProject(arg);
                }
                // Regular media file
                else {
                    Debug::Log("Single file from command-line: " + arg);
                    project_manager->LoadSingleFileFromDrop(arg);
                }
            } else {
                // Multiple files - act like drag-drop multiple files
                Debug::Log("Multiple files from command-line");
                show_project_panel = true;
                project_manager->LoadMultipleFilesFromDrop(initial_files);
            }
        }

        // Start system pressure monitor (background thread)
        pressure_monitor = std::make_unique<ump::SystemPressureMonitor>();
        pressure_monitor->SetRAMCriticalThreshold(0.90f);  // 90% critical
        pressure_monitor->SetRAMWarningThreshold(0.80f);   // 80% warning
        pressure_monitor->SetCPUWarningThreshold(0.85f);   // 85% CPU warning
        pressure_monitor->SetPollInterval(3.0);            // Poll every 3 seconds
        pressure_monitor->Start();
        Debug::Log("System pressure monitor started");

        return true;
    }

    void RefreshCurrentFrame() {
        if (!video_player) return;

        // Get current position and re-seek to it to refresh the frame with new color pipeline
        // This matches what timeline scrubbing does and is the most reliable way to refresh
        double current_position = video_player->GetPosition();
        video_player->Seek(current_position);
        Debug::Log("Refreshed frame at position: " + std::to_string(current_position) + "s");
    }

    void UpdateColorPipeline() {
        if (!node_manager || !video_player) {
            Debug::Log("Cannot update pipeline: missing node_manager or video_player");
            return;
        }

        // Get all connections from node_manager
        auto connections = node_manager->GetConnections();
        if (connections.empty()) {
            Debug::Log("No connections in node graph - clearing pipeline");
            video_player->ClearColorPipeline();
            return;
        }

        // Build a simple pipeline from connections
        std::string src_colorspace;
        std::string display;
        std::string view;
        std::string looks;

        // Iterate through all nodes to find the pipeline components
        for (int node_id = 1; node_id < 100; ++node_id) {
            ump::NodeBase* node = node_manager->GetNode(node_id);
            if (!node) continue;

            switch (node->GetType()) {
            case ump::NodeType::INPUT_COLORSPACE: {
                auto* csNode = dynamic_cast<ump::InputColorSpaceNode*>(node);
                if (csNode) {
                    src_colorspace = csNode->GetColorSpace();
                    Debug::Log("Found Input ColorSpace: " + src_colorspace);
                }
                break;
            }
            case ump::NodeType::LOOK: {
                auto* lookNode = dynamic_cast<ump::LookNode*>(node);
                if (lookNode && !lookNode->GetLook().empty()) {  // Note: GetLook() not GetLookName()
                    if (!looks.empty()) looks += ", ";
                    looks += lookNode->GetLook();
                    Debug::Log("Found Look: " + lookNode->GetLook());
                }
                break;
            }
            case ump::NodeType::OUTPUT_DISPLAY: {
                auto* displayNode = dynamic_cast<ump::OutputDisplayNode*>(node);
                if (displayNode) {
                    // Parse display string - it might be in format "Display - View"
                    std::string display_str = displayNode->GetDisplay();
                    size_t dash_pos = display_str.find(" - ");
                    if (dash_pos != std::string::npos) {
                        display = display_str.substr(0, dash_pos);
                        view = display_str.substr(dash_pos + 3);
                    }
                    else {
                        display = display_str;
                        view = "sRGB";  // Default view
                    }
                    Debug::Log("Found Output: " + display + " - " + view);
                }
                break;
            }
            }
        }

        // Build the OCIO pipeline if we have the minimum requirements
        if (!src_colorspace.empty() && !display.empty() && !view.empty()) {
            Debug::Log("Building OCIO pipeline...");
            auto ocio_pipeline = std::make_unique<OCIOPipeline>();

            if (ocio_pipeline->BuildFromDescription(src_colorspace, display, view, looks)) {
                video_player->SetColorPipeline(std::move(ocio_pipeline));
                Debug::Log("Color pipeline activated!");
            }
            else {
                Debug::Log("Failed to build color pipeline");
                video_player->ClearColorPipeline();
            }
        }
        else {
            Debug::Log("Incomplete pipeline - need Input, Output nodes connected");
            video_player->ClearColorPipeline();
        }
    }

    void Run() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // Process deferred fullscreen toggle AFTER all events are processed
            if (pending_fullscreen_toggle) {
                pending_fullscreen_toggle = false;
                ToggleFullscreen();
            }

            // Process delayed auto-play (500ms after video load)
            if (pending_auto_play && cache_settings.auto_play_on_load) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - auto_play_timer).count();
                if (elapsed >= 500) {
                    pending_auto_play = false;
                    if (video_player && video_player->HasVideo() && !video_player->IsPlaying()) {
                        video_player->Play();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(true);
                        }
                        Debug::Log("Auto-play: Started playback after 500ms delay");
                    }
                }
            }

            // Check system pressure (atomic read - no blocking)
            if (pressure_monitor) {
                bool critical = pressure_monitor->IsRAMCritical();

                // Update status every frame while in emergency mode (for recovery check)
                if (in_emergency_mode) {
                    last_pressure_status = pressure_monitor->GetStatus();
                }

                if (critical && !in_emergency_mode) {
                    // Get detailed status (locks briefly, but only on critical)
                    last_pressure_status = pressure_monitor->GetStatus();
                    HandleCriticalPressure();
                }

                // Recovery: Auto-resume when RAM drops below 80% (12% hysteresis from 92% critical)
                if (in_emergency_mode && !critical) {
                    float ram = last_pressure_status.ram_usage_percent;
                    Debug::Log("Recovery check: RAM at " + std::to_string(ram * 100) + "% (threshold: 80%)");

                    if (ram < 0.80f) {
                        Debug::Log("SYSTEM RECOVERED: RAM at " + std::to_string(ram * 100) + "% - resuming operations");

                        // Exit emergency mode
                        in_emergency_mode = false;

                        // Re-enable caching
                        cache_enabled = true;
                        if (project_manager) {
                            project_manager->SetCacheEnabled(true);
                        }

                        // Restore EXR cache if it was active before shutdown
                        Debug::Log("Recovery: exr_cache_was_active=" + std::string(exr_cache_was_active ? "yes" : "no") +
                                   ", video_player=" + std::string(video_player ? "yes" : "no"));

                        if (exr_cache_was_active && video_player) {
                            Debug::Log("Restoring EXR cache for: " + exr_video_path_before_shutdown);

                            const auto& exr_files = video_player->GetEXRSequenceFiles();
                            std::string exr_layer = video_player->GetEXRLayerName();
                            double exr_fps = video_player->GetEXRFrameRate();

                            Debug::Log("EXR restore: files=" + std::to_string(exr_files.size()) +
                                       ", layer=" + exr_layer + ", fps=" + std::to_string(exr_fps));

                            if (!exr_files.empty()) {
                                video_player->InitializeEXRCache(exr_files, exr_layer, exr_fps);
                                Debug::Log("EXR cache restored with " + std::to_string(exr_files.size()) + " frames");
                            } else {
                                Debug::Log("WARNING: Cannot restore EXR cache - sequence files empty");
                            }

                            exr_cache_was_active = false;
                            exr_video_path_before_shutdown.clear();
                        } else {
                            Debug::Log("Skipping EXR restore (was not active during emergency)");
                        }

                        // Show recovery notification (timed, 8 seconds)
                        stats_bar_notification_message = "RAM recovered - caching resumed";
                        show_notification_permanent = false;  // Auto-dismiss after timeout
                        notification_start_time = std::chrono::steady_clock::now();

                        Debug::Log("Caching resumed after recovery - notification shown");
                    }
                }
            }

            if (video_player) {
                video_player->UpdateFromMPVEvents();
                video_player->UpdateVideoTexture();

                // Process pending thumbnail uploads (async -> GL texture upload on main thread)
                if (video_player->HasThumbnailCache()) {
                    video_player->GetThumbnailCache()->ProcessPendingUploads();
                }

                // Update timeline manager first to handle cache logic
                if (timeline_manager) {
                    timeline_manager->Update(video_player.get());
                }

                // Only update fast seeking if timeline manager isn't using cached frames
                if (!timeline_manager || !timeline_manager->IsHoldingCachedFrame()) {
                    video_player->UpdateFastSeek();
                }
            }

            HandleKeyboardShortcuts();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            CreateDockingLayout();

            ImGui::Render();
            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            ApplyBackgroundColor();
            glClear(GL_COLOR_BUFFER_BIT);

            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            ImGuiIO& io = ImGui::GetIO();
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                GLFWwindow* backup_current_context = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backup_current_context);
            }

            glfwSwapBuffers(window);
        }
    }

    void Cleanup() {
        Debug::Log("=== CLEANUP STARTED ===");

        // Save settings before shutting down
        Debug::Log("Cleanup: Saving settings...");
        SaveSettings();
        Debug::Log("Cleanup: Settings saved");

        // Set shutdown flag and render one frame showing the modal
        Debug::Log("Cleanup: Setting shutdown flag and rendering final frame...");
        is_shutting_down_ = true;

        // Render one frame with the shutdown modal visible
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        CreateDockingLayout(); // This will render the shutdown modal

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        ApplyBackgroundColor();
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        glfwSwapBuffers(window);
        glfwPollEvents(); // Process one last event cycle
        Debug::Log("Cleanup: Final frame rendered");

        // Now proceed with actual cleanup
        // Stop pressure monitor before destroying other resources
        Debug::Log("Cleanup: Stopping pressure monitor...");
        if (pressure_monitor) {
            pressure_monitor->Stop();
            pressure_monitor.reset();
            Debug::Log("Cleanup: Pressure monitor stopped and reset");
        } else {
            Debug::Log("Cleanup: No pressure monitor to stop");
        }

        // Clean up ProjectManager video caches BEFORE VideoPlayer cleanup
        // This ensures FrameCache background threads are stopped while VideoPlayer is still valid
        Debug::Log("Cleanup: Clearing ProjectManager video caches...");
        if (project_manager) {
            project_manager->ClearAllCaches();
            Debug::Log("Cleanup: ProjectManager video caches cleared");
        } else {
            Debug::Log("Cleanup: No project manager to clean up");
        }

        // Clean up video player (this includes mpv, caches, threads)
        Debug::Log("Cleanup: Starting VideoPlayer cleanup...");
        if (video_player) {
            video_player->Cleanup();
            Debug::Log("Cleanup: VideoPlayer cleanup complete");
        } else {
            Debug::Log("Cleanup: No video player to clean up");
        }

        // Shutdown ImGui and related contexts
        Debug::Log("Cleanup: Shutting down ImGui OpenGL3...");
        ImGui_ImplOpenGL3_Shutdown();
        Debug::Log("Cleanup: Shutting down ImGui GLFW...");
        ImGui_ImplGlfw_Shutdown();
        Debug::Log("Cleanup: Destroying ImNodes context...");
        ImNodes::DestroyContext();
        Debug::Log("Cleanup: Destroying ImPlot context...");
        ImPlot::DestroyContext();
        Debug::Log("Cleanup: Destroying ImGui context...");
        ImGui::DestroyContext();
        Debug::Log("Cleanup: All ImGui contexts destroyed");

        // Destroy GLFW window and terminate
        Debug::Log("Cleanup: Destroying GLFW window...");
        glfwDestroyWindow(window);
        Debug::Log("Cleanup: Terminating GLFW...");
        glfwTerminate();
        Debug::Log("=== CLEANUP COMPLETED SUCCESSFULLY ===");
    }

private:
    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Core Systems
    // ------------------------------------------------------------------------
    GLFWwindow* window;
    std::unique_ptr<VideoPlayer> video_player;
    std::unique_ptr<ump::ProjectManager> project_manager;
    std::unique_ptr<TimelineManager> timeline_manager;
    std::unique_ptr<ump::AnnotationManager> annotation_manager;
    std::unique_ptr<ump::AnnotationPanel> annotation_panel;
    std::unique_ptr<ump::Annotations::AnnotationExporter> annotation_exporter;
    std::unique_ptr<ump::Annotations::ViewportAnnotator> viewport_annotator;
    std::unique_ptr<ump::Annotations::AnnotationToolbar> annotation_toolbar;
    std::unique_ptr<ump::Annotations::AnnotationRenderer> annotation_renderer;

    // Current annotation editing state
    std::vector<ump::Annotations::ActiveStroke> current_annotation_strokes_;
    std::string current_editing_timecode_;

    // Undo/redo stacks for annotation editing
    std::vector<std::vector<ump::Annotations::ActiveStroke>> annotation_undo_stack_;
    std::vector<std::vector<ump::Annotations::ActiveStroke>> annotation_redo_stack_;

    bool first_time_setup;
    std::string layout_ini_path;  // Persistent storage for ImGui ini filename

    // Window settings from saved preferences
    int saved_window_x = -1;
    int saved_window_y = -1;
    int saved_window_width = 1914;
    int saved_window_height = 1060;
    bool has_saved_window_settings = false;
    std::string saved_imgui_layout; // Store layout data to load after ImGui init

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - UI State
    // ------------------------------------------------------------------------
    bool show_video = true;
    bool show_controls = true;
    bool show_timeline = true;
    bool show_status_bar = true;
    bool show_project_panel = true;
    bool show_inspector_panel = true;
    bool show_timeline_panel = true;
    bool show_transport_controls = true;
    bool show_color_panels = false;
    bool saved_show_color_panels = false;
    bool show_annotation_panel = false;
    bool saved_show_annotation_panel = false;
    bool show_annotation_toolbar = false;
    bool saved_show_annotation_toolbar = false;
    bool annotations_enabled = true; // Enable/disable annotation rendering during playback
    bool timeline_editing_mode = true;
    bool minimal_view_mode = false;
    bool show_system_stats_bar = false;
    bool is_fullscreen = false;
    bool pending_fullscreen_toggle = false;
    bool saved_show_project_panel = true;
    bool saved_show_inspector_panel = true;
    bool show_color = false;

    // Shutdown state
    bool is_shutting_down_ = false;
    float shutdown_animation_time_ = 0.0f;

    enum class VideoBackgroundType {
        DEFAULT,
        BLACK,
        DARK_CHECKERBOARD,
        LIGHT_CHECKERBOARD
    };

    // Safety overlay settings
    SafetyGuideSettings safety_settings;

    VideoBackgroundType video_background_type = VideoBackgroundType::BLACK;
    bool show_background_panel = false;
    bool show_colorspace_panel = false;
    bool show_safety_overlay_panel = false;


    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Input State
    // ------------------------------------------------------------------------
    bool rewind_a_held = false;
    bool fastforward_d_held = false;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Media State
    // ------------------------------------------------------------------------
    std::string current_file_path;
    std::vector<std::string> recent_files;
    const size_t max_recent_files = 10;
    float last_volume = 1.0f;
    int current_volume = 100;
    bool is_muted = false;
    int volume_before_mute = 100;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Metadata State
    // ------------------------------------------------------------------------
    std::future<std::unique_ptr<ExifToolHelper::Metadata>> metadata_future;
    std::unique_ptr<ExifToolHelper::Metadata> cached_metadata;
    std::string metadata_for_file;
    bool metadata_loading = false;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Nodes
    // ------------------------------------------------------------------------
    std::unique_ptr<ump::NodeManager> node_manager;
    struct OCIONodeDragPayload {
        ump::NodeType type;
        char name[256];
    };

    // ------------------------------------------------------------------------
    // SETUP & CONFIGURATION METHODS
    // ------------------------------------------------------------------------
    void LoadCustomFonts() {
        ImGuiIO& io = ImGui::GetIO();

        io.Fonts->AddFontDefault();
        font_regular = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter_18pt-Regular.ttf", 18.0f);
        font_mono = io.Fonts->AddFontFromFileTTF("assets/fonts/JetBrainsMono-Regular.ttf", 16.0f);

        ImFontConfig icons_config;
        icons_config.MergeMode = false;
        icons_config.PixelSnapH = true;

        static const ImWchar icons_ranges[] = { 0xE000, 0xF8FF, 0 };
        font_icons = io.Fonts->AddFontFromFileTTF("assets/fonts/MaterialSymbolsSharp-Regular.ttf", 18.0f, &icons_config, icons_ranges);

        if (font_regular) {
            io.FontDefault = font_regular;
        }

        if (font_icons) {
        }
    }

    // ========================================================================
    // WINDOWS ACCENT COLOR UTILITIES
    // ========================================================================
    ImVec4 GetFallbackYellowColor() {
        return ImVec4(0.65f, 0.55f, 0.15f, 1.0f); // Even darker softer yellow color
    }

#ifdef _WIN32
    ImVec4 GetWindowsAccentColor() {
        // Check if Windows accent color is enabled
        if (!use_windows_accent_color) {
            return GetFallbackYellowColor();
        }

        DWORD colorization_color;
        BOOL opaque_blend;
        if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
            // Convert ARGB to ImVec4 RGBA
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
        return ImVec4(0.65f, 0.55f, 0.15f, 1.0f); // Even darker softer yellow color
    }
#endif

    // Color tinting utilities for creating muted variants
    ImVec4 TintColor(const ImVec4& color, float brightness, float saturation = 1.0f) {
        // brightness: 0.0 = black, 1.0 = original, >1.0 = lighter
        // saturation: 0.0 = grayscale, 1.0 = original
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

    ImVec4 MutedDark(const ImVec4& accent) { return TintColor(accent, 0.7f, 0.4f); }
    ImVec4 MutedLight(const ImVec4& accent) { return TintColor(accent, 1.5f, 0.8f); }
    ImVec4 Bright(const ImVec4& accent) { return TintColor(accent, 2.2f, 0.5f); }

    // Convert ImVec4 to ImU32 for draw list operations
    ImU32 ToImU32(const ImVec4& color) {
        return IM_COL32(
            (int)(color.x * 255.0f),
            (int)(color.y * 255.0f),
            (int)(color.z * 255.0f),
            (int)(color.w * 255.0f)
        );
    }

    void SetupImGuiStyle() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
        style.FramePadding = ImVec2(8.0f, 8.0f);
        style.ItemSpacing = ImVec2(8.0f, 4.0f);

        colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.128f, 0.128f, 0.128f, 1.00f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.128f, 0.128f, 0.128f, 1.00f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.128f, 0.128f, 0.128f, 1.00f);
        colors[ImGuiCol_Border] = ImVec4(0.19f, 0.19f, 0.19f, 0.40f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.060f, 0.060f, 0.060f, 1.00f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.125f, 0.125f, 0.125f, 1.0f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
        colors[ImGuiCol_CheckMark] = GetWindowsAccentColor();
        colors[ImGuiCol_SliderGrab] = ImVec4(0.54f, 0.54f, 0.54f, 1.00f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.67f, 0.67f, 0.67f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.19f, 0.19f, 0.19f, 0.55f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 0.80f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.30f, 0.29f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.060f, 0.060f, 0.060f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
        colors[ImGuiCol_TabActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        colors[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.60f, 0.60f, 0.60f, 0.70f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.26f, 0.26f, 0.35f);
        colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
        colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.01f, 0.01f, 0.65f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f); 
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f); 
        colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f); 
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f); 

        style.WindowPadding = ImVec2(12.00f, 12.00f);
        style.FramePadding = ImVec2(5.00f, 2.00f);
        style.CellPadding = ImVec2(6.00f, 6.00f);
        style.ItemSpacing = ImVec2(6.00f, 6.00f);
        style.ItemInnerSpacing = ImVec2(6.00f, 6.00f);
        style.TouchExtraPadding = ImVec2(0.00f, 0.00f);
        style.IndentSpacing = 25;
        style.ScrollbarSize = 15;
        style.GrabMinSize = 10;
        style.WindowBorderSize = 0;
        style.ChildBorderSize = 0;
        style.PopupBorderSize = 0;
        style.FrameBorderSize = 0;
        style.TabBorderSize = 0;
        style.WindowRounding = 2;
        style.ChildRounding = 2;
        style.FrameRounding = 2;
        style.PopupRounding = 4;
        style.ScrollbarRounding = 9;
        style.GrabRounding = 3;
        style.LogSliderDeadzone = 4;
        style.TabRounding = 2;
    }

    void SetupDragDrop() {
        glfwSetWindowUserPointer(window, this);

        glfwSetDropCallback(window, [](GLFWwindow* window, int count, const char** paths) {
            Debug::Log("=== GLFW Drag-Drop Event STARTED ===");
            Debug::Log("Files dropped: " + std::to_string(count));

            Application* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
            if (!app || !app->project_manager) {
                Debug::Log("No app or project manager available for drag-drop");
                return;
            }

            Debug::Log("App and project manager available, processing files...");

            if (count == 1) {
                // Single file - act like "Open Video"
                Debug::Log("Single file dropped - acting like Open Video");
                std::string dropped_file = std::string(paths[0]);
                Debug::Log("Dropped file path: " + dropped_file);

                app->project_manager->LoadSingleFileFromDrop(dropped_file);
            }
            else if (count > 1) {
                // Multiple files - act like "Load Media"
                Debug::Log("Multiple files dropped - acting like Load Media");
                app->show_project_panel = true;
                std::vector<std::string> filePaths;
                for (int i = 0; i < count; i++) {
                    filePaths.push_back(std::string(paths[i]));
                    Debug::Log("  File " + std::to_string(i) + ": " + std::string(paths[i]));
                }
                app->project_manager->LoadMultipleFilesFromDrop(filePaths);
            }

            Debug::Log("=== Drag-Drop Event Complete ===");
            });

        Debug::Log("GLFW drag-drop callback registered");
    }

#ifdef _WIN32
    void EnableDarkModeWindow(GLFWwindow* window) {

        HWND hwnd = nullptr;

#ifdef GLFW_EXPOSE_NATIVE_WIN32
        hwnd = glfwGetWin32Window(window);
#else
        OutputDebugStringA("Using FindWindowA fallback...\n");
        hwnd = FindWindowA(nullptr, "ump - Professional Video Editor");
#endif

        if (hwnd) {

            BOOL useDarkMode = TRUE;
            HRESULT result = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

            if (SUCCEEDED(result)) {
            }
            else {
                result = DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
                if (SUCCEEDED(result)) {
                }
                else {
                    OutputDebugStringA("FAILED: Both attributes failed\n");
                }
            }

            // Force window refresh
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);

        }
        else {
            OutputDebugStringA("ERROR: Could not get window handle\n");
        }
    }
#endif

    // ------------------------------------------------------------------------
    // INPUT & EVENT HANDLING
    // ------------------------------------------------------------------------

    void HandleKeyboardShortcuts() {
        ImGuiIO& io = ImGui::GetIO();

        // Don't process shortcuts when typing in text fields
        if (io.WantTextInput) return;

        // Use GLFW direct key checking for more reliable fullscreen detection during video playback
        static bool f_was_pressed = false;

        bool f_pressed = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;

        // Detect key press (not hold) - trigger on press down, not while held
        if (f_pressed && !f_was_pressed) {
            pending_fullscreen_toggle = true;
        }

        f_was_pressed = f_pressed;

        // ==============================================
        // PROJECT PANEL SHORTCUTS
        // ==============================================

        // Delete key - Remove selected items from project OR delete nodes/connections
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            // First check if we're in color panels mode and have nodes/connections selected
            if (show_color_panels && node_manager) {
                bool handled_node_deletion = false;

                // Delete selected nodes
                if (node_manager->HasSelectedNode()) {
                    node_manager->DeleteSelectedNodes();
                    Debug::Log("Delete key: Deleted selected nodes");
                    UpdateColorPipeline();
                    handled_node_deletion = true;
                }

                // Delete selected connections
                int num_selected_links = ImNodes::NumSelectedLinks();
                if (num_selected_links > 0) {
                    std::vector<int> selected_links(num_selected_links);
                    ImNodes::GetSelectedLinks(selected_links.data());

                    for (int link_id : selected_links) {
                        node_manager->DeleteConnection(link_id);
                    }

                    Debug::Log("Delete key: Deleted " + std::to_string(num_selected_links) + " selected connections");
                    UpdateColorPipeline();
                    handled_node_deletion = true;
                }

                // If we didn't handle node deletion, fall back to project deletion
                if (!handled_node_deletion && project_manager && project_manager->HasSelectedItems()) {
                    project_manager->DeleteSelectedItems();
                    Debug::Log("Delete key: Removed selected items from project");
                }
            }
            // If not in color panels mode, handle project deletion
            else if (project_manager && project_manager->HasSelectedItems()) {
                project_manager->DeleteSelectedItems();
                Debug::Log("Delete key: Removed selected items from project");
            }
        }

        // X key - Alternative delete for nodes/connections (only in color panels mode)
        if (ImGui::IsKeyPressed(ImGuiKey_X)) {
            if (show_color_panels && node_manager) {
                // Delete selected nodes
                if (node_manager->HasSelectedNode()) {
                    node_manager->DeleteSelectedNodes();
                    Debug::Log("X key: Deleted selected nodes");
                    UpdateColorPipeline();
                }

                // Delete selected connections
                int num_selected_links = ImNodes::NumSelectedLinks();
                if (num_selected_links > 0) {
                    std::vector<int> selected_links(num_selected_links);
                    ImNodes::GetSelectedLinks(selected_links.data());

                    for (int link_id : selected_links) {
                        node_manager->DeleteConnection(link_id);
                    }

                    Debug::Log("X key: Deleted " + std::to_string(num_selected_links) + " selected connections");
                    UpdateColorPipeline();
                }
            }
        }

        // F2 key - Rename (if single item selected)
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            if (project_manager && project_manager->GetSelectedItemsCount() == 1) {
                project_manager->StartRenamingSelected();
                Debug::Log("F2 key: Start renaming selected item");
            }
        }

        // Ctrl+P - Create Playlist from Selection
        if (ImGui::IsKeyPressed(ImGuiKey_P) && io.KeyCtrl) {
            if (project_manager && project_manager->HasSelectedItems()) {
                project_manager->CreatePlaylistFromSelection();
                Debug::Log("Ctrl+P: Create playlist from selection");
            }
        }

        // ==============================================
        // PLAYBACK CONTROLS
        // ==============================================

        // Space and W - Play/Pause
        if (ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_W)) {
            if (video_player->IsPlaying()) {
                video_player->Pause();
                if (project_manager) {
                    project_manager->NotifyPlaybackState(false);
                }
            }
            else {
                video_player->Play();
                if (project_manager) {
                    project_manager->NotifyPlaybackState(true);
                }
            }
        }

        // L - Toggle Loop
        if (ImGui::IsKeyPressed(ImGuiKey_L)) {
            ToggleLoop();
        }

        // B - Quick Background Cycling
        if (ImGui::IsKeyPressed(ImGuiKey_B) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
            if (!io.WantTextInput) {  // Only if not typing in a text field
                // Cycle through backgrounds in same order as popup menu
                static const VideoBackgroundType cycle_order[] = {
                    VideoBackgroundType::BLACK,
                    VideoBackgroundType::DEFAULT,
                    VideoBackgroundType::DARK_CHECKERBOARD,
                    VideoBackgroundType::LIGHT_CHECKERBOARD
                };

                // Find current position in cycle
                int current_index = 0;
                for (int i = 0; i < 4; i++) {
                    if (cycle_order[i] == video_background_type) {
                        current_index = i;
                        break;
                    }
                }

                // Move to next in cycle
                current_index = (current_index + 1) % 4;
                video_background_type = cycle_order[current_index];

                const char* names[] = { "Black", "None", "Dark Checkerboard", "Light Checkerboard" };
                Debug::Log("B: Quick-switched background to: " + std::string(names[current_index]));
            }
        }

        // ==============================================
        // ==============================================
        // FRAME STEPPING
        // ==============================================

        // Q - Frame Back
        if (ImGui::IsKeyPressed(ImGuiKey_Q)) {
            video_player->StepFrame(-1);
            Debug::Log("Q - Frame back");
        }

        // E - Frame Forward
        if (ImGui::IsKeyPressed(ImGuiKey_E)) {
            video_player->StepFrame(1);
            Debug::Log("E - Frame forward");
        }

        // ==============================================
        // FAST SEEK CONTROLS
        // ==============================================

        // A - Rewind (held for continuous)
        bool a_pressed = ImGui::IsKeyDown(ImGuiKey_A);
        if (a_pressed && !rewind_a_held) {
            rewind_a_held = true;
            video_player->StartRewind();
            Debug::Log("A - Start rewind");
        }
        else if (!a_pressed && rewind_a_held) {
            rewind_a_held = false;
            video_player->StopFastSeek();
            Debug::Log("A - Stop rewind");
        }

        // D - Fast Forward (held for continuous)
        bool d_pressed = ImGui::IsKeyDown(ImGuiKey_D);
        if (d_pressed && !fastforward_d_held) {
            fastforward_d_held = true;
            video_player->StartFastForward();
            Debug::Log("D - Start fast forward");
        }
        else if (!d_pressed && fastforward_d_held) {
            fastforward_d_held = false;
            video_player->StopFastSeek();
            Debug::Log("D - Stop fast forward");
        }

        // ==============================================
        // VOLUME CONTROLS
        // ==============================================

        // Arrow Up - Volume Up
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            int new_volume = current_volume + 5;
            if (new_volume > 100) new_volume = 100;
            current_volume = new_volume;
            video_player->SetVolume(new_volume);
            Debug::Log("Volume Up: " + std::to_string(new_volume) + "%");
        }

        // Arrow Down - Volume Down
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            int new_volume = current_volume - 5;
            if (new_volume < 0) new_volume = 0;
            current_volume = new_volume;
            video_player->SetVolume(new_volume);
            Debug::Log("Volume Down: " + std::to_string(new_volume) + "%");
        }

        if (ImGui::IsKeyPressed(ImGuiKey_M)) {
            ToggleMute();
        }

        // ==============================================
        // PLAYLIST NAVIGATION
        // ==============================================

        // Arrow Left - Previous Clip
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            if (project_manager && project_manager->IsInSequenceMode()) {
                project_manager->GoToPreviousInPlaylist();
                Debug::Log("Previous clip in playlist");
            }
        }

        // Arrow Right - Next Clip
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            if (project_manager && project_manager->IsInSequenceMode()) {
                project_manager->GoToNextInPlaylist();
                Debug::Log("Next clip in playlist");
            }
        }

        // ==============================================
        // VIEW PANEL TOGGLES
        // ==============================================

        // Ctrl+1 - Project Panel
        if (ImGui::IsKeyPressed(ImGuiKey_1) && io.KeyCtrl) {
            show_project_panel = !show_project_panel;
            if (show_project_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
            Debug::Log("Toggle Project Panel: " + std::string(show_project_panel ? "ON" : "OFF"));
        }

        // Ctrl+2 - Inspector Panel
        if (ImGui::IsKeyPressed(ImGuiKey_2) && io.KeyCtrl) {
            show_inspector_panel = !show_inspector_panel;
            if (show_inspector_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
            Debug::Log("Toggle Inspector Panel: " + std::string(show_inspector_panel ? "ON" : "OFF"));
        }

        // Ctrl+3 - Timeline & Transport Panel
        if (ImGui::IsKeyPressed(ImGuiKey_3) && io.KeyCtrl) {
            show_timeline_panel = !show_timeline_panel;
            if (show_timeline_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
            first_time_setup = true; // Trigger layout rebuild when timeline visibility changes
            Debug::Log("Toggle Timeline Panel: " + std::string(show_timeline_panel ? "ON" : "OFF"));
        }

        // Ctrl+5 - Annotations Panel
        if (ImGui::IsKeyPressed(ImGuiKey_5) && io.KeyCtrl) {
            show_annotation_panel = !show_annotation_panel;
            if (show_annotation_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
            Debug::Log("Toggle Annotations Panel: " + std::string(show_annotation_panel ? "ON" : "OFF"));
        }

        // Ctrl+6 - Annotation Toolbar
        if (ImGui::IsKeyPressed(ImGuiKey_6) && io.KeyCtrl) {
            show_annotation_toolbar = !show_annotation_toolbar;
            annotation_toolbar->SetVisible(show_annotation_toolbar);
            Debug::Log("Toggle Annotation Toolbar: " + std::string(show_annotation_toolbar ? "ON" : "OFF"));
        }

        // Ctrl+7 - System Stats Bar
        if (ImGui::IsKeyPressed(ImGuiKey_7) && io.KeyCtrl) {
            show_system_stats_bar = !show_system_stats_bar;
            Debug::Log("Toggle System Stats Bar: " + std::string(show_system_stats_bar ? "ON" : "OFF"));
        }

        // Escape - Cancel annotation mode (if active)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && viewport_annotator && viewport_annotator->IsAnnotationMode()) {
            Debug::Log("Escape: Canceling annotation mode");
            viewport_annotator->ClearActiveStroke();
            current_annotation_strokes_.clear();
            current_editing_timecode_.clear();
            viewport_annotator->SetMode(ump::Annotations::ViewportMode::PLAYBACK);
            if (annotation_toolbar) {
                annotation_toolbar->SetVisible(false);
            }
        }

        // Enter - Done with annotation (if in annotation mode)
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && viewport_annotator && viewport_annotator->IsAnnotationMode()) {
            Debug::Log("Enter: Saving annotation and exiting mode");

            // Finalize any active stroke being drawn
            auto active_stroke = viewport_annotator->FinalizeStroke();
            if (active_stroke) {
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

            viewport_annotator->SetMode(ump::Annotations::ViewportMode::PLAYBACK);
            if (annotation_toolbar) {
                annotation_toolbar->SetVisible(false);
            }
        }

        // Ctrl+0 - Default View
        if (ImGui::IsKeyPressed(ImGuiKey_0) && io.KeyCtrl) {
            minimal_view_mode = false;
            SetDefaultView();
        }

        // Ctrl+9 - Show All Panels
        if (ImGui::IsKeyPressed(ImGuiKey_9) && io.KeyCtrl) {
            minimal_view_mode = false;
            ShowAllPanels();
        }

        // Ctrl+4 - Toggle color Panels
        if (ImGui::IsKeyPressed(ImGuiKey_4) && io.KeyCtrl) {
            show_color_panels = !show_color_panels;
            first_time_setup = true;
        }


        // ==============================================
        // VIEW TOGGLES 
        // ==============================================

        // Ctrl+- - Toggle Minimal View
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) && io.KeyCtrl) {
            minimal_view_mode = !minimal_view_mode;
            if (minimal_view_mode) {
                // Save current state before entering minimal view
                saved_show_project_panel = show_project_panel;
                saved_show_inspector_panel = show_inspector_panel;
                saved_show_color_panels = show_color_panels;
                saved_show_annotation_panel = show_annotation_panel;
                saved_show_annotation_toolbar = show_annotation_toolbar;

                // Enable minimal view - only video and timeline
                show_project_panel = false;
                show_inspector_panel = false;
                show_timeline_panel = true;
                show_color_panels = false;
                show_annotation_panel = false;
                show_annotation_toolbar = false;
                if (annotation_toolbar) annotation_toolbar->SetVisible(false);

                first_time_setup = true;
                Debug::Log("Minimal View: ON");
            }
            else {
                // Restore previous state when exiting minimal view
                show_project_panel = saved_show_project_panel;
                show_inspector_panel = saved_show_inspector_panel;
                show_timeline_panel = true;
                show_color_panels = saved_show_color_panels;
                show_annotation_panel = saved_show_annotation_panel;
                show_annotation_toolbar = saved_show_annotation_toolbar;
                if (annotation_toolbar) annotation_toolbar->SetVisible(saved_show_annotation_toolbar);

                first_time_setup = true;
                Debug::Log("Minimal View: OFF");
            }
        }

        // Ctrl+C - Toggle Colorspace Presets Panel
        if (ImGui::IsKeyPressed(ImGuiKey_C) && io.KeyCtrl) {
            show_colorspace_panel = !show_colorspace_panel;
            Debug::Log("Ctrl+C: Toggle colorspace presets panel");
        }

        // Ctrl+/ - Toggle Safety Overlay Panel
        if (ImGui::IsKeyPressed(ImGuiKey_Slash) && io.KeyCtrl) {
            show_safety_overlay_panel = !show_safety_overlay_panel;
            Debug::Log("Ctrl+/: Toggle safety overlay panel");
        }

        // Ctrl+Shift+B - Toggle Background Panel
        if (ImGui::IsKeyPressed(ImGuiKey_B) && io.KeyCtrl && io.KeyShift) {
            show_background_panel = !show_background_panel;
            Debug::Log("Ctrl+Shift+B: Toggle background panel");
        }

        // ==============================================
        // FILE OPERATIONS
        // ==============================================

        // Ctrl+O - Open File
        if (ImGui::IsKeyPressed(ImGuiKey_O) && io.KeyCtrl) {
            OpenFileDialog();
            Debug::Log("Open file dialog");
        }

        // Ctrl+W - Close Current File
        if (ImGui::IsKeyPressed(ImGuiKey_W) && io.KeyCtrl) {
            if (!current_file_path.empty()) {
                CloseVideo();
                Debug::Log("Close current file");
            }
        }

        // Ctrl+S - Save Project
        if (ImGui::IsKeyPressed(ImGuiKey_S) && io.KeyCtrl && !io.KeyShift) {
            if (project_manager && !project_manager->GetProjectPath().empty()) {
                project_manager->SaveProject();
                Debug::Log("Save project");
            }
        }

        // Ctrl+Shift+O - Open Project
        if (ImGui::IsKeyPressed(ImGuiKey_O) && io.KeyCtrl && io.KeyShift) {
            if (project_manager) {
                project_manager->LoadProject();
                Debug::Log("Open project dialog");
                // Show project panels for context
                show_project_panel = true;
                show_inspector_panel = true;
            }
        }

        // Ctrl+R - Reset Layout
        if (ImGui::IsKeyPressed(ImGuiKey_R) && io.KeyCtrl) {
            first_time_setup = true;
            Debug::Log("Reset layout");
        }

        // ==============================================
        // SCREENSHOT OPERATIONS
        // ==============================================

        // Ctrl+[ - Screenshot to Clipboard
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket) && io.KeyCtrl) {
            if (video_player && video_player->HasValidTexture()) {
                video_player->CaptureScreenshotToClipboard();
                Debug::Log("Ctrl+[: Screenshot to clipboard");
            }
        }

        // Ctrl+] - Screenshot to Desktop
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket) && io.KeyCtrl) {
            if (video_player && video_player->HasValidTexture()) {
                video_player->CaptureScreenshotToDesktop();
                Debug::Log("Ctrl+]: Screenshot to desktop");
            }
        }
    }

    void HandleInput() {
        if (ImGui::IsKeyPressed(ImGuiKey_O) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
            OpenFileDialog();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_W) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
            if (!current_file_path.empty()) {
                CloseVideo();
            }
        }
    }

    // ------------------------------------------------------------------------
    // UI LAYOUT & PANELS
    // ------------------------------------------------------------------------
    void CreateDockingLayout() {
        ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        if (is_fullscreen) {
            // Fullscreen ImGui window - use same monitor size as GLFW resize
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);

            // Adjust for title bar - position ImGui content to start below title bar
            ImGui::SetNextWindowPos(ImVec2(0.0f, 32.0f)); // Start below 32px title bar
            ImGui::SetNextWindowSize(ImVec2((float)mode->width, (float)mode->height - 32.0f)); // Reduce height by title bar
            ImGui::SetNextWindowViewport(viewport->ID);

            ImGuiWindowFlags fullscreen_flags = ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

            // Force square corners and no borders/margins
            ImGui::GetStyle().WindowRounding = 0.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0.0f, 0.0f));

            bool fullscreen_open = true;
            ImGui::Begin("Fullscreen Player", &fullscreen_open, fullscreen_flags);
            ImGui::PopStyleVar(11);

            // Render video content in fullscreen
            if (video_player) {
                ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
                ImVec2 canvas_size = ImGui::GetContentRegionAvail();
                DrawVideoBackground(canvas_pos, canvas_size, 40.0f);
                video_player->RenderVideoFrame();

                // NOTE: Global memory limit enforcement removed - now using seconds-based cache windows
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
        ImGui::Begin("ump Dockspace", &p_open, window_flags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_AutoHideTabBar);

        if (first_time_setup) {

            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

            if (show_color_panels) {
                // Always create dock node for stats bar at very top (even if not visible initially)
                ImGuiID stats_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.04f, nullptr, &dockspace_id);
                saved_stats_dock_id = stats_dock;  // Save for later use

                // COLOR VIEW: Color panel at very bottom, video/timeline above it
                auto bottom_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.4f, nullptr, &dockspace_id);

                // Top area can have side panels if needed
                if (show_project_panel || show_inspector_panel) {
                    auto dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f, nullptr, &dockspace_id);
                    auto dock_id_project = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.46f, nullptr, &dock_id_left);
                    auto dock_id_inspector = dock_id_left;

                    ImGuiID dock_id_video, dock_id_timeline;
                    if (show_timeline_panel) {
                        // Split the main area vertically: video on top, timeline below
                        dock_id_video = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.87f, nullptr, &dockspace_id);
                        dock_id_timeline = dockspace_id; // Timeline gets remaining space below video
                    } else {
                        // Timeline is hidden - video takes full main area
                        dock_id_video = dockspace_id;
                    }

                    // Split video area for annotations on the right
                    auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);

                    // Split video area for annotation toolbar at bottom
                    auto dock_id_annotation_toolbar = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Down, 0.055f, nullptr, &dock_id_video);

                    // Dock windows
                    ImGui::DockBuilderDockWindow("Project", dock_id_project);
                    ImGui::DockBuilderDockWindow("Inspector", dock_id_inspector);
                    ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
                    ImGui::DockBuilderDockWindow("Annotation Toolbar", dock_id_annotation_toolbar);
                    ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
                    if (show_timeline_panel) {
                        ImGui::DockBuilderDockWindow("Timeline & Transport", dock_id_timeline);
                    }
                    ImGui::DockBuilderDockWindow("Color", bottom_dock);
                    // Always dock stats bar (even if not visible initially)
                    ImGui::DockBuilderDockWindow("System Stats", stats_dock);
                }
                else {
                    ImGuiID dock_id_video, dock_id_timeline;
                    if (show_timeline_panel) {
                        // Split the main area vertically: video on top, timeline below
                        dock_id_video = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.87f, nullptr, &dockspace_id);
                        dock_id_timeline = dockspace_id; // Timeline gets remaining space below video
                    } else {
                        // Timeline is hidden - video takes full main area
                        dock_id_video = dockspace_id;
                    }

                    // Split video area for annotations on the right
                    auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);

                    // Split video area for annotation toolbar at bottom
                    auto dock_id_annotation_toolbar = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Down, 0.055f, nullptr, &dock_id_video);

                    ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
                    ImGui::DockBuilderDockWindow("Annotation Toolbar", dock_id_annotation_toolbar);
                    ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
                    if (show_timeline_panel) {
                        ImGui::DockBuilderDockWindow("Timeline & Transport", dock_id_timeline);
                    }
                    ImGui::DockBuilderDockWindow("Color", bottom_dock);
                    // Always dock stats bar (even if not visible initially)
                    ImGui::DockBuilderDockWindow("System Stats", stats_dock);
                }
            }
            else {
                // DEFAULT VIEW: Side panels on left, timeline under viewport (if shown), no color panel

                // Always create dock node for stats bar at very top (even if not visible initially)
                ImGuiID stats_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.04f, nullptr, &dockspace_id);
                saved_stats_dock_id = stats_dock;  // Save for later use

                auto dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f, nullptr, &dockspace_id);
                auto dock_id_project = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.46f, nullptr, &dock_id_left);
                auto dock_id_inspector = dock_id_left;

                ImGuiID dock_id_video, dock_id_timeline;
                if (show_timeline_panel) {
                    // Split the main area vertically: video on top, timeline below
                    dock_id_video = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.87f, nullptr, &dockspace_id);
                    dock_id_timeline = dockspace_id; // Timeline gets remaining space below video
                } else {
                    // Timeline is hidden - video takes full main area
                    dock_id_video = dockspace_id;
                }

                // Split video area for annotations on the right
                auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);

                // Split video area for annotation toolbar at bottom
                auto dock_id_annotation_toolbar = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Down, 0.055f, nullptr, &dock_id_video);

                ImGui::DockBuilderDockWindow("Project", dock_id_project);
                ImGui::DockBuilderDockWindow("Inspector", dock_id_inspector);
                ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
                ImGui::DockBuilderDockWindow("Annotation Toolbar", dock_id_annotation_toolbar);
                ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
                if (show_timeline_panel) {
                    ImGui::DockBuilderDockWindow("Timeline & Transport", dock_id_timeline);
                }
                // Always dock stats bar (even if not visible initially)
                ImGui::DockBuilderDockWindow("System Stats", stats_dock);
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
        CreateVideoViewport();
        if (!is_fullscreen) {
            if (show_timeline_panel) CreateTimelineTransportPanel();
            if (show_project_panel) CreateProjectPanel();
            if (show_inspector_panel) CreateInspectorPanel();
            if (show_annotation_panel) CreateAnnotationPanel();
            CreateAnnotationToolbar(); // Always try to render toolbar (it handles visibility internally)
            if (show_color_panels) CreateColorPanels();
            if (show_system_stats_bar) RenderSystemStatsPanel(); // System stats docking panel
            CreateCacheStatsWindow(); // Add cache monitoring window
            CreateCacheSettingsWindow(); // Add cache settings popup
        }
        RenderBackgroundSelectionPanel(video_background_type, show_background_panel);
        RenderSafetyOverlayPanel(show_safety_overlay_panel);
        RenderColorspacePresetsPanel(show_colorspace_panel);

        // Handle project manager dialogs (including image sequence frame rate dialog)
        if (project_manager) {
            project_manager->HandleProjectDialogs();
        }

        // Render top-level dialogs (outside any parent modal context for proper centering)
        CreateTranscodeProgressDialog(); // EXR transcode progress dialog
        CreateCacheClearDialogs(); // Cache clear dialogs (error + success)
        // RenderPressureWarningBanner(); // REMOVED: No longer using warning banner
        RenderPressureCriticalDialog(); // System critical emergency dialog with auto-recovery

        // Render Frame.io import dialog
        RenderFrameioImportDialog();

        // Share Project popups
        HandleShareProjectPopups();

        // Render shutdown modal if shutting down
        RenderShutdownModal();
    }

    void HandleShareProjectPopups() {
        // Success popup - URI copied to clipboard
        if (ImGui::BeginPopupModal("URI Copied##ShareProject", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Project URI copied to clipboard!");
            ImGui::Separator();
            ImGui::TextWrapped("Share this link with others to open the project.");
            ImGui::TextWrapped("Format: ump:///path/to/project.umproj");
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Error popup - No project saved
        if (ImGui::BeginPopupModal("No Project Saved##ShareProject", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("No project file has been saved yet.");
            ImGui::Separator();
            ImGui::TextWrapped("Please save your project first before sharing.");
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Preferences deleted popup
        if (ImGui::BeginPopupModal("Preferences Deleted", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("All preferences have been deleted.");
            ImGui::Separator();
            ImGui::TextWrapped("Please restart the application for changes to take effect.");
            ImGui::TextWrapped("The application will use default settings on next launch.");
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void SetupDefaultLayout(ImGuiID dockspace_id) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        // Always create dock node for stats bar at very top (even if not visible initially)
        ImGuiID dock_id_stats = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.04f, nullptr, &dockspace_id);
        saved_stats_dock_id = dock_id_stats;

        // Side panels on left
        auto dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f, nullptr, &dockspace_id);
        auto dock_id_project = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.5f, nullptr, &dock_id_left);
        auto dock_id_inspector = dock_id_left;

        ImGuiID dock_id_video, dock_id_timeline_or_bottom;
        if (show_timeline_panel) {
            // Split the main area vertically: video on top, timeline below
            dock_id_video = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.80f, nullptr, &dockspace_id);
            dock_id_timeline_or_bottom = dockspace_id; // Timeline gets remaining space below video
        } else {
            // Timeline is hidden - video takes full main area
            dock_id_video = dockspace_id;
            dock_id_timeline_or_bottom = 0; // No bottom area
        }

        // Split video area to add annotations on the right
        auto dock_id_annotations = ImGui::DockBuilderSplitNode(dock_id_video, ImGuiDir_Right, 0.25f, nullptr, &dock_id_video);

        ImGui::DockBuilderDockWindow("Project", dock_id_project);
        ImGui::DockBuilderDockWindow("Inspector", dock_id_inspector);
        ImGui::DockBuilderDockWindow("Video Viewport", dock_id_video);
        ImGui::DockBuilderDockWindow("Annotations", dock_id_annotations);
        ImGui::DockBuilderDockWindow("System Stats", dock_id_stats);
        if (show_timeline_panel) {
            ImGui::DockBuilderDockWindow("Timeline & Transport", dock_id_timeline_or_bottom);
        }

        ImGui::DockBuilderFinish(dockspace_id);
    }

    void CreateMenuBar() {
        if (ImGui::BeginMenuBar()) {
            // Custom submenu background color
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));

            if (ImGui::BeginMenu("File")) {
                ImGui::TextDisabled("Media:");
                if (ImGui::MenuItem("Open Media...", "Ctrl+O")) {
                    OpenFileDialog();
                }

                if (!current_file_path.empty()) {
                    if (ImGui::MenuItem("Close Media", "Ctrl+W")) {
                        CloseVideo();
                    }
                }

                if (!recent_files.empty()) {
                    if (ImGui::BeginMenu("Recent Files")) {
                        for (const auto& file : recent_files) {
                            if (ImGui::MenuItem(GetFileName(file).c_str())) {
                                Debug::Log("*** Loading recent file: " + file);

                                // Route through project manager for proper cache eviction
                                if (project_manager) {
                                    project_manager->LoadSingleFileFromDrop(file);
                                } else {
                                    // Fallback to direct loading if project manager unavailable
                                    current_file_path = file;
                                    video_player->LoadFile(current_file_path);
                                    if (timeline_manager) {
                                        Debug::Log("*** Calling timeline_manager->SetVideoFile() for recent file");
                                        timeline_manager->SetVideoFile(current_file_path);
                                    }
                                    else {
                                        Debug::Log("*** ERROR: timeline_manager is null!");
                                    }

                                    // Auto-seek to first frame so user sees the video immediately
                                    video_player->Seek(0.0);
                                }
                                Debug::Log("Auto-seeked to first frame after recent file load");
                            }
                        }
                        ImGui::EndMenu();
                    }
                }

                ImGui::Separator();

                // Project management
                ImGui::TextDisabled("Project:");
                if (ImGui::MenuItem("Open Project...", "Ctrl+Shift+O")) {
                    if (project_manager) {
                        project_manager->LoadProject();  // Empty path triggers file dialog
                        // Show project panels for context
                        show_project_panel = true;
                        show_inspector_panel = true;
                    }
                }

                bool has_project = project_manager && !project_manager->GetProjectPath().empty();
                if (ImGui::MenuItem("Save Project", "Ctrl+S", false, has_project)) {
                    if (project_manager) {
                        project_manager->SaveProject();
                    }
                }

                ImGui::Separator();

                // Screenshot options
                ImGui::TextDisabled("Clipboard:");
                bool has_video = video_player && video_player->HasValidTexture();
                if (ImGui::MenuItem("Screenshot to Clipboard", "Ctrl+[", false, has_video)) {
                    if (has_video) {
                        video_player->CaptureScreenshotToClipboard();
                    }
                }
                if (ImGui::MenuItem("Screenshot to Desktop", "Ctrl+]", false, has_video)) {
                    if (has_video) {
                        video_player->CaptureScreenshotToDesktop();
                    }
                }

                ImGui::Separator();

                // Project sharing
                ImGui::TextDisabled("Share:");
                if (ImGui::MenuItem("Copy Project Link...", nullptr, false, has_project)) {
                    ShareProject();
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View")) {

                ImGui::TextDisabled("Layout Controls:");
                // Layout controls
                if (ImGui::MenuItem("Reset Layout", "Ctrl+R")) {
                    first_time_setup = true;
                    Debug::Log("Layout reset requested");
                }

                ImGui::Separator();

                // View modes
                if (ImGui::MenuItem("Default View", "Ctrl+0")) {
                    minimal_view_mode = false;
                    SetDefaultView();
                }

                if (ImGui::MenuItem("Minimal View", "Ctrl+-", minimal_view_mode)) {
                    minimal_view_mode = !minimal_view_mode;

                    if (minimal_view_mode) {
                        // Save current state before entering minimal view
                        saved_show_project_panel = show_project_panel;
                        saved_show_inspector_panel = show_inspector_panel;
                        saved_show_color_panels = show_color_panels;
                        saved_show_annotation_panel = show_annotation_panel;
                        saved_show_annotation_toolbar = show_annotation_toolbar;

                        // Enable minimal view - only video and timeline
                        show_project_panel = false;
                        show_inspector_panel = false;
                        show_timeline_panel = true;
                        show_color_panels = false;
                        show_annotation_panel = false;
                        show_annotation_toolbar = false;
                        if (annotation_toolbar) annotation_toolbar->SetVisible(false);
                        first_time_setup = true;
                        Debug::Log("Minimal View: ON");
                    }
                    else {
                        // Restore previous state when exiting minimal view
                        show_project_panel = saved_show_project_panel;
                        show_inspector_panel = saved_show_inspector_panel;
                        show_timeline_panel = true;
                        show_color_panels = saved_show_color_panels;
                        show_annotation_panel = saved_show_annotation_panel;
                        show_annotation_toolbar = saved_show_annotation_toolbar;
                        if (annotation_toolbar) annotation_toolbar->SetVisible(saved_show_annotation_toolbar);

                        first_time_setup = true;
                        Debug::Log("Minimal View: OFF");
                    }
                }

                if (ImGui::MenuItem("Fullscreen", "F", is_fullscreen)) {
                    pending_fullscreen_toggle = true;
                }

                ImGui::Separator();

                // Panel visibility controls
                if (ImGui::MenuItem("Show All Panels", "Ctrl+9")) {
                    minimal_view_mode = false;
                    ShowAllPanels();
                }

                ImGui::Separator();
                ImGui::TextDisabled("Panels:");
                // Individual panel toggles
                if (ImGui::MenuItem("Project Panel", "Ctrl+1", show_project_panel)) {
                    show_project_panel = !show_project_panel;
                    if (show_project_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                }

                if (ImGui::MenuItem("Inspector Panel", "Ctrl+2", show_inspector_panel)) {
                    show_inspector_panel = !show_inspector_panel;
                    if (show_inspector_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                }

                if (ImGui::MenuItem("Timeline Panel", "Ctrl+3", show_timeline_panel)) {
                    show_timeline_panel = !show_timeline_panel;
                    if (show_timeline_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                    first_time_setup = true; // Trigger layout rebuild when timeline visibility changes
                }

                if (ImGui::MenuItem("Color Panels", "Ctrl+4", &show_color_panels)) {
                    first_time_setup = true;
                }

                if (ImGui::MenuItem("Annotations", "Ctrl+5", show_annotation_panel)) {
                    show_annotation_panel = !show_annotation_panel;
                    if (show_annotation_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                }

                if (ImGui::MenuItem("Annotation Toolbar", "Ctrl+6", show_annotation_toolbar)) {
                    show_annotation_toolbar = !show_annotation_toolbar;
                    if (annotation_toolbar) annotation_toolbar->SetVisible(show_annotation_toolbar);
                }

                ImGui::Separator();
                ImGui::TextDisabled("Stats:");

                if (ImGui::MenuItem("System Stats Bar", "Ctrl+7", show_system_stats_bar)) {
                    show_system_stats_bar = !show_system_stats_bar;
                    /*first_time_setup = true;*/
                }

                ImGui::Separator();
                ImGui::TextDisabled("Thumbnails:");

                if (ImGui::MenuItem("Enable Timeline Thumbnails", nullptr, cache_settings.enable_thumbnails)) {
                    cache_settings.enable_thumbnails = !cache_settings.enable_thumbnails;
                    SaveSettings();
                    Debug::Log(cache_settings.enable_thumbnails ? "Timeline thumbnails enabled" : "Timeline thumbnails disabled");
                }

                ImGui::Separator();
                ImGui::TextDisabled("Background & Overlays:");

                if (ImGui::MenuItem("Video Background", "Ctrl+Shift+B, B")) {
                    show_background_panel = !show_background_panel;
                }

                if (ImGui::MenuItem("Safety Overlays", "Ctrl+/")) {
                    show_safety_overlay_panel = !show_safety_overlay_panel;
                }

                if (ImGui::MenuItem("Colorspace Presets", "Ctrl+C")) {
                    show_colorspace_panel = !show_colorspace_panel;
                }

                ImGui::Separator();
                ImGui::TextDisabled("Settings:");

                if (ImGui::MenuItem("Windows Accent Color", nullptr, use_windows_accent_color)) {
                    use_windows_accent_color = !use_windows_accent_color;
                    SaveSettings();
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Annotations")) {
                ImGui::TextDisabled("Annotation Display:");

                if (ImGui::MenuItem("Enable Annotations", nullptr, annotations_enabled)) {
                    annotations_enabled = !annotations_enabled;
                    Debug::Log(annotations_enabled ? "Annotations enabled for playback" : "Annotations disabled for playback");
                }

                ImGui::Separator();

                ImGui::TextDisabled("Panels:");

                if (ImGui::MenuItem("Annotations Panel", "Ctrl+5", show_annotation_panel)) {
                    show_annotation_panel = !show_annotation_panel;
                    if (show_annotation_panel) minimal_view_mode = false;
                }

                if (ImGui::MenuItem("Annotation Toolbar", "Ctrl+6", show_annotation_toolbar)) {
                    show_annotation_toolbar = !show_annotation_toolbar;
                    if (annotation_toolbar) annotation_toolbar->SetVisible(show_annotation_toolbar);
                }

                if (ImGui::MenuItem("System Stats Bar", "Ctrl+7", show_system_stats_bar)) {
                    show_system_stats_bar = !show_system_stats_bar;
                }

                ImGui::Separator();

                // Export submenu
                bool has_notes = annotation_manager && annotation_manager->GetNoteCount() > 0;

                if (ImGui::BeginMenu("Export Notes", has_notes)) {
                    if (ImGui::MenuItem("Markdown")) {
                        if (annotation_panel) {
                            annotation_panel->TriggerExport("markdown");
                        }
                    }
                    if (ImGui::MenuItem("HTML")) {
                        if (annotation_panel) {
                            annotation_panel->TriggerExport("html");
                        }
                    }
                    if (ImGui::MenuItem("PDF")) {
                        if (annotation_panel) {
                            annotation_panel->TriggerExport("pdf");
                        }
                    }
                    ImGui::EndMenu();
                }

                if (!has_notes) {
                    ImGui::TextDisabled("(No notes to export)");
                }

                ImGui::Separator();

                // Import submenu
                if (ImGui::BeginMenu("Import Notes")) {
                    if (ImGui::MenuItem("From Frame.io...")) {
                        frameio_import_state.show_dialog = true;
                    }
                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Playback")) {
                // ==============================================
                // PRIMARY CONTROLS
                // ==============================================

                ImGui::TextDisabled("Playback:");

                if (ImGui::MenuItem("Play/Pause", "Space, W")) {
                    if (video_player->IsPlaying()) {
                        video_player->Pause();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(false);
                        }
                    }
                    else {
                        video_player->Play();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(true);
                        }
                    }
                }

                bool loop_enabled = video_player->IsLooping();
                bool is_playlist_mode = project_manager && project_manager->IsInSequenceMode();

                const char* loop_menu_text;
                const char* loop_status;
                if (is_playlist_mode) {
                    loop_menu_text = "Toggle Playlist Loop";
                    loop_status = loop_enabled ? " (ON)" : " (OFF)";
                }
                else {
                    loop_menu_text = "Toggle Single File Loop";
                    loop_status = loop_enabled ? " (ON)" : " (OFF)";
                }

                std::string full_loop_text = std::string(loop_menu_text) + loop_status;

                if (ImGui::MenuItem(full_loop_text.c_str(), "L")) {
                    ToggleLoop();
                }

                ImGui::Separator();


                // ==============================================
                // FRAME CONTROLS
                // ==============================================

                ImGui::TextDisabled("Frame Navigation:");

                if (ImGui::MenuItem("Previous Frame", "Q")) {
                    video_player->StepFrame(-1);
                }

                if (ImGui::MenuItem("Next Frame", "E")) {
                    video_player->StepFrame(1);
                }

                ImGui::Separator();

                // ==============================================
                // FAST SEEK CONTROLS (Legacy A/D system)
                // ==============================================

                ImGui::TextDisabled("Fast Seek (Legacy):");

                if (ImGui::MenuItem("Rewind", "A (hold)")) {
                    video_player->StartRewind();
                }

                if (ImGui::MenuItem("Fast Forward", "D (hold)")) {
                    video_player->StartFastForward();
                }

                ImGui::Separator();

                // ==============================================
                // PLAYLIST NAVIGATION - FIXED CONDITIONS
                // ==============================================

                ImGui::TextDisabled("Playlist Navigation:");

                bool has_playlist = project_manager && project_manager->IsInSequenceMode();

                if (ImGui::MenuItem("Previous Clip", "Left Arrow", false, has_playlist)) {
                    if (has_playlist) {
                        project_manager->GoToPreviousInPlaylist();
                    }
                }

                if (ImGui::MenuItem("Next Clip", "Right Arrow", false, has_playlist)) {
                    if (has_playlist) {
                        project_manager->GoToNextInPlaylist();
                    }
                }

                ImGui::Separator();

                // ==============================================
                // VOLUME CONTROLS
                // ==============================================

                ImGui::TextDisabled("Volume:");

                if (ImGui::MenuItem("Volume Up", "Up Arrow")) {
                    int new_volume = current_volume + 5;
                    if (new_volume > 100) new_volume = 100;
                    current_volume = new_volume;
                    video_player->SetVolume(new_volume);
                }

                if (ImGui::MenuItem("Volume Down", "Down Arrow")) {
                    int new_volume = current_volume - 5;
                    if (new_volume < 0) new_volume = 0;
                    current_volume = new_volume;
                    video_player->SetVolume(new_volume);
                }

                if (ImGui::MenuItem("Mute/Unmute", "M")) {
                    ToggleMute();
                }

                ImGui::Separator();

                // ==============================================
                // PLAYBACK OPTIONS
                // ==============================================

                ImGui::TextDisabled("Options:");

                if (ImGui::MenuItem("Auto-Play on Load", nullptr, cache_settings.auto_play_on_load)) {
                    cache_settings.auto_play_on_load = !cache_settings.auto_play_on_load;
                    SaveSettings();
                    Debug::Log(cache_settings.auto_play_on_load ? "Auto-play on load enabled" : "Auto-play on load disabled");
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Cache")) {
                // Check if we're in EXR mode (EXR has its own separate cache system)
                bool is_exr_mode = video_player && video_player->IsInEXRMode();

                if (is_exr_mode) {
                    // EXR mode: Show disabled message
                    ImGui::TextDisabled("Seek cache controls disabled in EXR mode");
                    ImGui::TextDisabled("(EXR uses automatic DirectEXR cache)");
                    ImGui::Separator();
                } else {
                    // Regular mode: Show format support
                    ImGui::TextDisabled("Cache Settings");
                    ImGui::Separator();
                }

                // === VIDEO SEEK CACHE ===
                ImGui::TextDisabled("Video Seek Cache:");

                // Disable all controls when in EXR mode
                if (is_exr_mode) ImGui::BeginDisabled();

                if (ImGui::MenuItem("Enable Video Seek Cache", nullptr, cache_enabled)) {
                    cache_enabled = !cache_enabled;
                    if (project_manager) {
                        project_manager->SetUserCachePreference(cache_enabled);
                    }
                    Debug::Log(cache_enabled ? "Video seek cache enabled" : "Video seek cache disabled");
                }

                if (ImGui::MenuItem("Clear Video Seek Cache")) {
                    if (project_manager) {
                        project_manager->ClearAllCaches();
                    }
                }

                if (ImGui::MenuItem("Restart Video Seek Cache")) {
                    if (project_manager) {
                        project_manager->RestartCache();
                    }
                }

                if (is_exr_mode) ImGui::EndDisabled();

                // === IMAGE SEQUENCE CACHE ===
                ImGui::Separator();
                ImGui::TextDisabled("Image Sequence Cache:");

                // Clear Image Sequence Disk Cache (dummies + transcodes from both default and custom paths)
                if (ImGui::MenuItem("Clear Image Sequence Disk Cache")) {
                    // Safety check: Block if EXR sequences are loaded
                    if (project_manager && project_manager->HasLoadedEXRSequences()) {
                        show_cannot_clear_exr_cache_error = true;
                    } else if (video_player) {
                        exr_cache_bytes_cleared = video_player->ClearEXRDiskCache();
                        show_exr_cache_cleared_success = true;
                    }
                }

                // === SETTINGS ===
                ImGui::Separator();
                ImGui::TextDisabled("Settings:");
                if (ImGui::MenuItem("Cache Settings...")) {
                    show_cache_settings = true;
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Pipeline")) {
                // Check if we're in EXR mode
                bool is_exr_mode = video_player && video_player->IsInEXRMode();

                // Check if we're in image sequence mode (pipeline mode auto-detected from image bit depth)
                bool is_image_sequence = project_manager && project_manager->IsInImageSequenceMode();

                // Pipeline Mode Selection
                ImGui::TextDisabled("Video Processing Pipeline:");

                if (is_exr_mode) {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "EXR Mode - Fixed Float16 Pipeline");
                    ImGui::TextDisabled("Pipeline selection disabled for EXR sequences");
                } else if (is_image_sequence) {
                    // Get auto-detected pipeline mode from frame cache
                    PipelineMode seq_mode = project_manager->GetImageSequencePipelineMode();
                    std::string mode_str = PipelineModeToString(seq_mode);
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), ("Image Sequence - " + mode_str + " Pipeline (Auto-detected)").c_str());
                    ImGui::TextDisabled("Pipeline locked to image bit depth");

                    // Show bit depth info
                    const char* bit_info = "";
                    if (seq_mode == PipelineMode::NORMAL) {
                        bit_info = "(8-bit images: PNG/JPEG/TIFF)";
                    } else if (seq_mode == PipelineMode::HIGH_RES) {
                        bit_info = "(16-bit images: PNG16/TIFF16)";
                    } else if (seq_mode == PipelineMode::ULTRA_HIGH_RES) {
                        bit_info = "(Float images: 32-bit TIFF)";
                    }
                    ImGui::TextDisabled("%s", bit_info);
                } else {
                    const char* pipeline_modes[] = { "Normal (8-bit)", "High-Res (12-bit/16-bit)", "Ultra-High-Res (Float)", "HDR-Res (HDR Display)" };

                    PipelineMode current_mode = video_player ? video_player->GetPipelineMode() : PipelineMode::NORMAL;
                    int current_mode_index = static_cast<int>(current_mode);

                    for (int i = 0; i < 4; i++) {
                        bool is_selected = (i == current_mode_index);

                        // Disable Ultra-High-Res for regular video (no float video formats exist)
                        bool should_disable = (i == 2); // Ultra-High-Res index

                        if (should_disable) {
                            ImGui::BeginDisabled();
                        }

                        if (ImGui::MenuItem(pipeline_modes[i], nullptr, is_selected)) {
                            if (video_player && i != current_mode_index) {
                                PipelineMode new_mode = static_cast<PipelineMode>(i);
                                if (video_player->SupportsPipelineMode(new_mode)) {
                                    video_player->SetPipelineMode(new_mode);
                                    Debug::Log("Pipeline mode changed to: " + std::string(PipelineModeToString(new_mode)));

                                    // Reload current media to apply new pipeline settings
                                    if (!current_file_path.empty()) {
                                        Debug::Log("Reloading media for new pipeline mode: " + current_file_path);

                                        // Route through project manager for proper cache eviction
                                        if (project_manager) {
                                            project_manager->LoadSingleFileFromDrop(current_file_path);
                                        } else {
                                            // Fallback to direct loading if project manager unavailable
                                            video_player->LoadFile(current_file_path);
                                            if (timeline_manager) {
                                                timeline_manager->SetVideoFile(current_file_path);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (should_disable) {
                            ImGui::EndDisabled();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                ImGui::SetTooltip("Ultra-High-Res disabled for video\n(no float video formats exist)");
                            }
                        }
                    }
                }

                ImGui::Separator();
                ImGui::TextDisabled("Settings:");

                if (ImGui::MenuItem("Pipeline & Cache Settings...")) {
                    show_cache_settings = true;
                }

                ImGui::Separator();

                // MPV Status Info
                ImGui::TextDisabled("Current Pipeline:");
                if (is_exr_mode) {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "EXR Float16 Pipeline (Fixed)");
                } else if (is_image_sequence) {
                    // Show actual auto-detected pipeline mode for image sequences
                    PipelineMode seq_mode = project_manager->GetImageSequencePipelineMode();
                    auto it = PIPELINE_CONFIGS.find(seq_mode);
                    if (it != PIPELINE_CONFIGS.end()) {
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (Image Sequence)", it->second.description.c_str());
                    } else {
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "Image Sequence - %s", PipelineModeToString(seq_mode));
                    }
                } else {
                    PipelineMode current_mode = video_player ? video_player->GetPipelineMode() : PipelineMode::NORMAL;
                    auto it = PIPELINE_CONFIGS.find(current_mode);
                    if (it != PIPELINE_CONFIGS.end()) {
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", it->second.description.c_str());
                    }
                }


                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("About u.m.p.")) {
                    ShellExecuteA(NULL, "open", "https://github.com/cbkow/ump/blob/main/README.md", NULL, NULL, SW_SHOWNORMAL);
                }

                if (ImGui::MenuItem("License")) {
                    ShellExecuteA(NULL, "open", "https://github.com/cbkow/ump/blob/main/LICENSE", NULL, NULL, SW_SHOWNORMAL);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Delete All Preferences")) {
                    DeleteAllPreferences();
                }

                ImGui::EndMenu();
            }

            ImGui::PopStyleColor(); // Pop custom submenu background
            ImGui::EndMenuBar();
        }
    }

    void CreateCacheStatsWindow() {
        static bool show_cache_stats = false;

        // Toggle with Ctrl+Shift+C
        if (ImGui::IsKeyPressed(ImGuiKey_C) && ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift) {
            show_cache_stats = !show_cache_stats;
        }

        if (!show_cache_stats || !timeline_manager) return;

        if (ImGui::Begin("Frame Cache Statistics", &show_cache_stats)) {
            auto stats = timeline_manager->GetCacheStats();

            ImGui::Text("Cache Status:");
            ImGui::Separator();

            ImGui::Text("Frames Cached: %zu", stats.total_frames_cached);
            // Removed: Memory usage display (memory-based eviction removed)
            ImGui::Text("Cache Hits: %zu", stats.cache_hits);
            ImGui::Text("Cache Misses: %zu", stats.cache_misses);
            ImGui::Text("Hit Ratio: %.1f%%", stats.hit_ratio * 100.0f);

            ImGui::Spacing();

            if (stats.coverage_end > stats.coverage_start) {
                ImGui::Text("Coverage: %.2fs - %.2fs", stats.coverage_start, stats.coverage_end);
                ImGui::Text("Window: %.2fs", stats.coverage_end - stats.coverage_start);
            }
            else {
                ImGui::Text("Coverage: No cached frames");
            }

            ImGui::Spacing();
            ImGui::Separator();

            // Cache configuration
            if (ImGui::CollapsingHeader("Cache Configuration")) {
                static int cache_size_mb = 12288;
                static float window_seconds = -1.0f;  // Full video
                static int cache_width = 1920;

                // Old cache settings UI removed - now using settings panel below
            }

            ImGui::Spacing();
            ImGui::Separator();

            // Cache visualization legend
            if (ImGui::CollapsingHeader("Cache Visualization")) {
                ImGui::Text("Timeline Cache Bar Legend:");
                ImGui::Spacing();

                // Green bar sample
                ImDrawList* legend_draw_list = ImGui::GetWindowDrawList();
                ImVec2 cursor_pos = ImGui::GetCursorScreenPos();

                legend_draw_list->AddRectFilled(
                    cursor_pos,
                    ImVec2(cursor_pos.x + 20, cursor_pos.y + 6),
                    IM_COL32(50, 200, 50, 200)
                );
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 25);
                ImGui::Text("RAM Cache (Green) - Frames cached in memory");

                cursor_pos = ImGui::GetCursorScreenPos();
                legend_draw_list->AddRectFilled(
                    cursor_pos,
                    ImVec2(cursor_pos.x + 20, cursor_pos.y + 6),
                    IM_COL32(50, 120, 200, 150)
                );
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 25);
                ImGui::Text("Keyframe Cache (Blue) - Sparse coverage for seeking");

                ImGui::Spacing();

                // White window indicator sample
                cursor_pos = ImGui::GetCursorScreenPos();
                legend_draw_list->AddRect(
                    ImVec2(cursor_pos.x, cursor_pos.y + 1),
                    ImVec2(cursor_pos.x + 20, cursor_pos.y + 5),
                    IM_COL32(255, 255, 255, 80), 0.0f, 0, 1.0f
                );
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 25);
                ImGui::Text("Active Cache Window (White outline) - 60s centered focus");

                ImGui::Spacing();
                ImGui::TextWrapped("Segments closer to current position appear brighter. "
                    "RAM cache focuses around the current playhead position for optimal scrubbing performance. "
                    "The cache bar appears at the bottom of the timeline.");
            }

            ImGui::Spacing();
            ImGui::Text("Press Ctrl+Shift+C to toggle this window");
        }
        ImGui::End();
    }

    void CreateTranscodeProgressDialog() {
        // Open modal popup when transcode starts
        if (show_transcode_progress) {
            ImGui::OpenPopup("EXR Transcode Progress");
        }

        // Set popup to center on screen
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500, 200), ImGuiCond_Always);

        if (ImGui::BeginPopupModal("EXR Transcode Progress", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            // Check if transcode completed (progress flag was cleared by completion callback)
            if (!show_transcode_progress) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }

            ImGui::Text("Converting multilayer EXR to single-layer sequence...");
            ImGui::Spacing();

            // Get progress values (thread-safe)
            int current = transcode_current_frame.load();
            int total = transcode_total_frames.load();

            std::string status_msg;
            {
                std::lock_guard<std::mutex> lock(transcode_status_mutex);
                status_msg = transcode_status_message;
            }

            // Progress bar with system accent color
            float progress = (total > 0) ? static_cast<float>(current) / static_cast<float>(total) : 0.0f;
            char progress_text[64];
            snprintf(progress_text, sizeof(progress_text), "%d / %d frames (%.0f%%)", current, total, progress * 100.0f);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, GetWindowsAccentColor());
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), progress_text);
            ImGui::PopStyleColor();

            ImGui::Spacing();

            // Status message
            if (!status_msg.empty()) {
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextWrapped("%s", status_msg.c_str());
                if (font_mono) ImGui::PopFont();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Cancel button
            if (ImGui::Button("Cancel Transcode", ImVec2(150, 0))) {
                // Call cancel on project manager's transcoder
                if (project_manager) {
                    project_manager->CancelTranscode();
                    Debug::Log("User requested transcode cancel");
                }
                show_transcode_progress = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            // Estimated time (optional enhancement)
            if (total > 0 && current > 0) {
                static auto start_time = std::chrono::steady_clock::now();
                if (current == 1) {
                    start_time = std::chrono::steady_clock::now();
                }

                auto elapsed = std::chrono::steady_clock::now() - start_time;
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

                if (elapsed_sec > 0) {
                    float rate = static_cast<float>(current) / static_cast<float>(elapsed_sec);
                    int remaining_frames = total - current;
                    int eta_sec = (rate > 0) ? static_cast<int>(remaining_frames / rate) : 0;

                    ImGui::Text("ETA: %d:%02d", eta_sec / 60, eta_sec % 60);
                }
            }

            ImGui::EndPopup();
        }
    }

    void CreateCacheClearDialogs() {
        // Open error popup when flag is set
        if (show_cannot_clear_exr_cache_error) {
            ImGui::OpenPopup("Cannot Clear EXR Cache");
            show_cannot_clear_exr_cache_error = false;
        }

        // Open success popup when flag is set
        if (show_exr_cache_cleared_success) {
            ImGui::OpenPopup("EXR Cache Cleared");
            show_exr_cache_cleared_success = false;
        }

        // Error popup for clearing image sequence cache while sequences are loaded
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Cannot Clear EXR Cache", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Cannot clear image sequence cache while sequences are loaded.");
            ImGui::Spacing();
            ImGui::TextWrapped("Dummy videos and transcodes are used by loaded image sequences.\nClearing them would corrupt playback.");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Please remove all EXR sequences from the project first.");
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Success popup showing how much was deleted
        center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("EXR Cache Cleared", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            double mb_cleared = exr_cache_bytes_cleared / 1024.0 / 1024.0;

            ImGui::Text("EXR Cache Cleared Successfully");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(Bright(GetWindowsAccentColor()), "%.2f MB deleted", mb_cleared);
            ImGui::Spacing();
            ImGui::TextWrapped("Cleared from dummies and EXR transcodes (all locations)");

            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void HandleCriticalPressure() {
        Debug::Log("SYSTEM CRITICAL: " + std::to_string(last_pressure_status.ram_usage_percent * 100) + "% RAM");

        // Enter emergency mode
        in_emergency_mode = true;

        // CRITICAL: Check if EXR cache is active BEFORE clearing anything
        // (ClearAllCaches will clear EXR cache, setting initialized_=false)
        bool has_exr = video_player && video_player->HasEXRCache();
        Debug::Log("EXR check (before clear): video_player=" + std::string(video_player ? "yes" : "no") +
                   ", HasEXRCache=" + std::string(has_exr ? "yes" : "no"));

        // Save EXR state BEFORE clearing
        if (has_exr) {
            auto before_stats = video_player->GetEXRCacheStats();
            Debug::Log("EXR cache detected (" + std::to_string(before_stats.cachedFrames) +
                       " cached frames), saving state for recovery...");

            // Remember that EXR was active so we can restore it on recovery
            exr_cache_was_active = true;
            if (!current_file_path.empty()) {
                exr_video_path_before_shutdown = current_file_path;
                Debug::Log("Saved EXR path for recovery: " + exr_video_path_before_shutdown);
            }

            // Stop playback - without cache, EXR playback won't work
            bool is_playing = video_player->IsPlaying();
            Debug::Log("Playback state: " + std::string(is_playing ? "PLAYING" : "PAUSED"));
            if (is_playing) {
                video_player->Pause();
                Debug::Log("Paused playback (EXR requires cache)");
            }
        } else {
            Debug::Log("No EXR cache to save");
            exr_cache_was_active = false;
        }

        // Priority 1: Clear all caches (NOTE: ClearAllCaches also clears EXR!)
        if (project_manager) {
            size_t before_ram_mb = last_pressure_status.ram_used_mb;
            project_manager->ClearAllCaches();
            Debug::Log("Evicted seek caches + EXR (before: " + std::to_string(before_ram_mb) + " MB RAM)");
        }

        // Disable caching to prevent RAM refill
        cache_enabled = false;
        if (project_manager) {
            project_manager->SetCacheEnabled(false);
        }

        // Auto-open stats bar and show notification
        show_system_stats_bar = true;
        stats_bar_notification_message = "RAM exceeded 92% - caching paused";
        show_notification_permanent = true;  // Keep message visible until recovery
        notification_start_time = std::chrono::steady_clock::now();

        Debug::Log("Stats bar opened with critical notification");
    }

    void RenderPressureWarningBanner() {
        if (!show_pressure_warning_banner) return;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 50));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(200, 150, 0, 220));

        if (ImGui::Begin("System Pressure Warning", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {

            ImGui::Spacing();

            // Show warning icon with Material Icons
            if (font_icons) ImGui::PushFont(font_icons);
            ImGui::Text(ICON_WARNING);
            if (font_icons) ImGui::PopFont();

            ImGui::SameLine(0, 8);

            // Show relevant metrics
            std::string warning_text = "System Under Pressure: ";

            if (last_pressure_status.ram_usage_percent >= 0.80f) {
                warning_text += "RAM " + std::to_string(int(last_pressure_status.ram_usage_percent * 100)) + "%";
            }

            if (last_pressure_status.cpu_usage_percent >= 0.85f) {
                if (last_pressure_status.ram_usage_percent >= 0.80f) {
                    warning_text += " | ";
                }
                warning_text += "CPU " + std::to_string(int(last_pressure_status.cpu_usage_percent * 100)) + "%";
            }

            warning_text += " - Consider reducing cache window if playback stutters";

            ImGui::Text("%s", warning_text.c_str());

            ImGui::SameLine(ImGui::GetWindowWidth() - 200);
            if (ImGui::Button("View Details", ImVec2(100, 0))) {
                show_cache_settings = true;
            }

            ImGui::SameLine();
            if (ImGui::Button("Dismiss", ImVec2(80, 0))) {
                show_pressure_warning_banner = false;
            }

            ImGui::Spacing();
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    void RenderPressureCriticalDialog() {
        if (!show_pressure_critical_dialog) return;

        ImGui::OpenPopup("System Critical");

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_Always);

        if (ImGui::BeginPopupModal("System Critical", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {

            // Show critical warning with Material Icons
            if (font_icons) ImGui::PushFont(font_icons);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), ICON_WARNING);
            if (font_icons) ImGui::PopFont();

            ImGui::SameLine(0, 8);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "SYSTEM CRITICAL - EMERGENCY MODE");

            ImGui::Separator();
            ImGui::Spacing();

            // Live RAM status with color coding for recovery
            float ram_percent = last_pressure_status.ram_usage_percent * 100.0f;
            ImVec4 ram_color;
            if (ram_percent >= 90.0f) {
                ram_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red - critical
            } else if (ram_percent >= 70.0f) {
                ram_color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f); // Yellow - recovering
            } else {
                ram_color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); // Green - recovered
            }

            ImGui::TextColored(ram_color, "System RAM: %.0f%% (%.1f GB / %.1f GB)",
                              ram_percent,
                              last_pressure_status.ram_used_mb / 1024.0,
                              last_pressure_status.ram_total_mb / 1024.0);

            // Show recovery status
            ImGui::Spacing();
            if (ram_percent >= 70.0f) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                 "Waiting for recovery... (auto-resumes at < 70%% RAM)");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                 "System recovered! Resuming...");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextWrapped(
                "Emergency shutdown triggered at 90%% RAM. All caches evicted, "
                "playback paused, and caching disabled."
            );

            ImGui::Spacing();
            ImGui::TextWrapped(
                "This dialog will automatically dismiss when RAM drops below 70%% "
                "and caching will resume."
            );

            ImGui::Spacing();
            ImGui::Text("Recommendations:");
            ImGui::BulletText("Close unused applications to free RAM");
            ImGui::BulletText("Consider reducing cache window (%ds currently)",
                             cache_settings.max_cache_seconds);
            ImGui::BulletText("Work with lower resolution content");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Open Cache Settings", ImVec2(160, 0))) {
                show_cache_settings = true;
                show_pressure_critical_dialog = false;
                in_emergency_mode = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Dismiss", ImVec2(100, 0))) {
                show_pressure_critical_dialog = false;
                in_emergency_mode = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Close ump", ImVec2(100, 0))) {
                Debug::Log("User requested app shutdown from critical dialog");
                exit(0);
            }

            ImGui::EndPopup();
        }
    }

    void RenderSystemStatsPanel() {
        if (!show_system_stats_bar || !pressure_monitor) return;

        // Refresh status only every 500ms (2Hz instead of 60Hz) to reduce mutex locks
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - last_stats_bar_refresh).count();

        if (elapsed >= stats_bar_refresh_interval) {
            cached_stats_bar_status = pressure_monitor->GetStatus();
            last_stats_bar_refresh = now;
        }

        auto& status = cached_stats_bar_status;

        // Get accent color (no muted-bright variant)
        ImVec4 accent = GetWindowsAccentColor();

        // If window doesn't exist yet and we have a saved dock ID, dock it there
        if (!ImGui::FindWindowByName("System Stats") && saved_stats_dock_id != 0) {
            ImGui::SetNextWindowDockID(saved_stats_dock_id, ImGuiCond_Appearing);
        }

        if (ImGui::Begin("System Stats", &show_system_stats_bar, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse)) {
            // Check if notification should auto-dismiss
            if (!stats_bar_notification_message.empty() && !show_notification_permanent) {
                auto now = std::chrono::steady_clock::now();
                float elapsed = std::chrono::duration<float>(now - notification_start_time).count();
                if (elapsed >= notification_timeout_seconds) {
                    stats_bar_notification_message.clear();  // Auto-dismiss
                }
            }

            // Use monospace font for entire panel
            if (font_mono) ImGui::PushFont(font_mono);

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 4));

            // RAM metrics
            ImGui::Text("RAM:");
            ImGui::SameLine();

            // Use normal accent color for progress bars
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
            ImGui::PushItemWidth(150);
            ImGui::ProgressBar(status.ram_usage_percent, ImVec2(0, 0), "");
            ImGui::PopItemWidth();
            ImGui::PopStyleColor();

            ImGui::SameLine();
            char ram_text[64];
            snprintf(ram_text, sizeof(ram_text), "%.1f%% (%.1f / %.1f GB)",
                    status.ram_usage_percent * 100.0f,
                    status.ram_used_mb / 1024.0f,
                    status.ram_total_mb / 1024.0f);
            ImGui::Text("%s", ram_text);

            // VRAM metrics (if available)
            if (status.vram_total_mb > 0) {
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                ImGui::Text("VRAM:");
                ImGui::SameLine();

                // Calculate VRAM usage percentage
                float vram_percent = 0.0f;
                if (status.vram_total_mb > 0 && status.vram_used_mb > 0) {
                    vram_percent = static_cast<float>(status.vram_used_mb) / status.vram_total_mb;
                }

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
                ImGui::PushItemWidth(150);
                ImGui::ProgressBar(vram_percent, ImVec2(0, 0), "");
                ImGui::PopItemWidth();
                ImGui::PopStyleColor();

                ImGui::SameLine();
                char vram_text[64];
                if (status.vram_used_mb > 0) {
                    snprintf(vram_text, sizeof(vram_text), "%.1f%% (%.1f / %.1f GB)",
                            vram_percent * 100.0f,
                            status.vram_used_mb / 1024.0f,
                            status.vram_total_mb / 1024.0f);
                } else {
                    snprintf(vram_text, sizeof(vram_text), "%.1f GB total", status.vram_total_mb / 1024.0f);
                }
                ImGui::Text("%s", vram_text);
            }

            // CPU metrics
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            ImGui::Text("CPU:");
            ImGui::SameLine();

            // Use normal accent color for progress bars
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
            ImGui::PushItemWidth(150);
            ImGui::ProgressBar(status.cpu_usage_percent, ImVec2(0, 0), "");
            ImGui::PopItemWidth();
            ImGui::PopStyleColor();

            ImGui::SameLine();
            char cpu_text[32];
            snprintf(cpu_text, sizeof(cpu_text), "%.1f%%", status.cpu_usage_percent * 100.0f);
            ImGui::Text("%s", cpu_text);

            // Pressure status indicator (using accent color for warning states)
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();

            // Pop mono font before icon font
            if (font_mono) ImGui::PopFont();

            // Push icon font for status icons
            if (font_icons) ImGui::PushFont(font_icons);

            if (status.pressure_level == ump::SystemPressureStatus::CRITICAL) {
                ImGui::TextColored(accent, ICON_WARNING);
                if (font_icons) ImGui::PopFont();
                ImGui::SameLine(0, 4);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(accent, "CRITICAL");
            } else if (status.pressure_level == ump::SystemPressureStatus::WARNING) {
                ImGui::TextColored(accent, ICON_WARNING);
                if (font_icons) ImGui::PopFont();
                ImGui::SameLine(0, 4);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(accent, "WARNING");
            } else {
                ImGui::Text(ICON_DONE);
                if (font_icons) ImGui::PopFont();
                ImGui::SameLine(0, 4);
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Text("OK");
            }

            // Show notification message on same row if present
            if (!stats_bar_notification_message.empty()) {
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();

                // Pop mono font for icon
                if (font_mono) ImGui::PopFont();
                if (font_icons) ImGui::PushFont(font_icons);

                // Show icon
                if (show_notification_permanent) {
                    ImGui::TextColored(accent, ICON_WARNING);
                } else {
                    ImGui::TextColored(accent, ICON_DONE);
                }

                if (font_icons) ImGui::PopFont();
                ImGui::SameLine(0, 4);
                if (font_mono) ImGui::PushFont(font_mono);

                // Show message
                ImGui::TextColored(accent, "%s", stats_bar_notification_message.c_str());
            }

            // Close button (flush right)
            ImGui::SameLine();
            float available_width = ImGui::GetContentRegionAvail().x;
            float button_width = 80.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available_width - button_width);

            if (font_mono) ImGui::PopFont();
            if (font_icons) ImGui::PushFont(font_icons);

            ImGui::PushStyleColor(ImGuiCol_Button, accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x * 1.2f, accent.y * 1.2f, accent.z * 1.2f, accent.w));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, accent.w));

            if (ImGui::Button(ICON_CLOSE, ImVec2(button_width, 0))) {
                show_system_stats_bar = false;
            }

            ImGui::PopStyleColor(3);
            if (font_icons) ImGui::PopFont();

            ImGui::PopStyleVar(2);
        }
        ImGui::End();
    }

    void CreateCacheSettingsWindow() {
        // Open modal popup when flag is set
        if (show_cache_settings) {
            ImGui::OpenPopup("Pipeline & Cache Settings");
            show_cache_settings = false; // Reset flag, modal will handle its own state
        }

        static bool settings_changed = false;

        // Set popup to center on screen
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        bool open = true;

        // Set modal size BEFORE BeginPopupModal
        ImGui::SetNextWindowSize(ImVec2(900, 650), ImGuiCond_Always);

        if (ImGui::BeginPopupModal("Pipeline & Cache Settings", &open, ImGuiWindowFlags_NoResize)) {

            ImGui::Text("Video Pipeline & Cache Settings");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Configure video processing quality and memory usage");
            ImGui::Separator();

            // Update pipeline info from current media
            bool is_exr_mode = video_player && video_player->IsInEXRMode();
            bool is_image_sequence = project_manager && project_manager->IsInImageSequenceMode();

            if (is_exr_mode) {
                // EXR sequences always use Float16 pipeline
                cache_settings.current_pipeline_mode = PipelineMode::HDR_RES;  // HDR_RES = half-float for EXR
            } else if (is_image_sequence) {
                // Image sequences use auto-detected pipeline from first frame
                cache_settings.current_pipeline_mode = project_manager->GetImageSequencePipelineMode();
            } else if (video_player) {
                // Regular video uses MPV pipeline mode
                cache_settings.current_pipeline_mode = video_player->GetPipelineMode();
            }

            // Show current pipeline mode (read-only for EXR/image sequences, changeable for video)
            ImGui::Text("Current Pipeline Mode:");
            auto it = PIPELINE_CONFIGS.find(cache_settings.current_pipeline_mode);
            if (it != PIPELINE_CONFIGS.end()) {
                if (font_mono) ImGui::PushFont(font_mono);
                if (is_exr_mode) {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (EXR - Fixed)", it->second.description.c_str());
                } else if (is_image_sequence) {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (Image Sequence - Auto-detected)", it->second.description.c_str());
                } else {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (Video)", it->second.description.c_str());
                }
                if (font_mono) ImGui::PopFont();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Tabbed layout for organized settings
            if (ImGui::BeginChild("SettingsContent", ImVec2(0, -50), false)) {
                if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {

                    // === TAB 1: Video Settings ===
                    if (ImGui::BeginTabItem("Video Settings")) {

            // Master cache enable/disable toggle
            ImGui::TextColored(Bright(GetWindowsAccentColor()), "Video Seek Cache:");
            if (ImGui::Checkbox("Enable Video Seek Cache", &cache_enabled)) {
                settings_changed = true;
                // Apply immediately to project manager
                if (project_manager) {
                    project_manager->SetUserCachePreference(cache_enabled);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Enable/disable background video frame caching.\n\n"
                    "When enabled:\n"
                    "  - Frames are pre-cached around current position for smooth scrubbing\n"
                    "  - Uses RAM based on duration limit below\n\n"
                    "When disabled:\n"
                    "  - No caching, direct playback only\n"
                    "  - Lower memory usage but less responsive scrubbing\n\n"
                    "Note: Automatically disabled for H.264/H.265 codecs");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Disable cache settings when cache is disabled
            ImGui::BeginDisabled(!cache_enabled);

            // Cache duration limit (NEW: replaces memory limit)
            ImGui::Text("Cache Duration Limit:");
            int min_seconds = 1;    // 1 second minimum
            int max_seconds = 60;   // 60 seconds maximum

            // Slider for visual adjustment
            if (ImGui::SliderInt("##CacheSecondsSlider", &cache_settings.max_cache_seconds, min_seconds, max_seconds)) {
                settings_changed = true;
            }

            // Manual input field for precise entry
            ImGui::SameLine();
            if (ImGui::InputInt("##CacheSecondsInput", &cache_settings.max_cache_seconds, 0, 0)) {
                // Clamp to valid range
                if (cache_settings.max_cache_seconds < min_seconds) {
                    cache_settings.max_cache_seconds = min_seconds;
                } else if (cache_settings.max_cache_seconds > max_seconds) {
                    cache_settings.max_cache_seconds = max_seconds;
                }
                settings_changed = true;
            }

            ImGui::SameLine();
            if (font_mono) ImGui::PushFont(font_mono);
            ImGui::Text("seconds (%.1f minutes)", cache_settings.max_cache_seconds / 60.0f);
            if (font_mono) ImGui::PopFont();

            // Help text
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Maximum duration of video cached around current position");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Frames beyond this window are automatically evicted");

            // Memory Estimates Section
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(Bright(GetWindowsAccentColor()), "Memory Usage Estimates:");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Estimated RAM usage for %d seconds @ 24fps", cache_settings.max_cache_seconds);

            ImGui::Spacing();

            // Create table with pipeline modes and resolutions
            if (ImGui::BeginTable("MemoryEstimates", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Pipeline Mode", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("HD (1920x1080)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("UHD (3840x2160)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableHeadersRow();

                // Calculate frames for 24fps
                int frames_24fps = cache_settings.max_cache_seconds * 24;

                // HD and UHD resolutions
                size_t hd_pixels = 1920 * 1080;
                size_t uhd_pixels = 3840 * 2160;

                // Enumerate all pipeline modes
                const struct {
                    PipelineMode mode;
                    const char* name;
                    int bytes_per_pixel;
                } pipeline_info[] = {
                    {PipelineMode::NORMAL, "Normal (8-bit)", 4},
                    {PipelineMode::HIGH_RES, "High-Res (12-bit/16-bit)", 8},  // AV_PIX_FMT_RGBA64LE = 8 bytes
                    {PipelineMode::ULTRA_HIGH_RES, "Ultra-High-Res (Float)", 16},  // EXR only: AV_PIX_FMT_RGBAF32LE = 16 bytes
                    {PipelineMode::HDR_RES, "HDR-Res (HDR Display)", 8}  // Fixed: AV_PIX_FMT_RGBA64LE = 8 bytes (half-float)
                };

                for (const auto& info : pipeline_info) {
                    ImGui::TableNextRow();

                    // Pipeline mode name (highlight current mode)
                    ImGui::TableNextColumn();
                    if (font_mono) ImGui::PushFont(font_mono);
                    if (info.mode == cache_settings.current_pipeline_mode) {
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", info.name);
                    } else {
                        ImGui::Text("%s", info.name);
                    }
                    if (font_mono) ImGui::PopFont();

                    // HD memory usage
                    ImGui::TableNextColumn();
                    if (font_mono) ImGui::PushFont(font_mono);
                    float hd_mb = (frames_24fps * hd_pixels * info.bytes_per_pixel) / (1024.0f * 1024.0f);
                    if (hd_mb < 1000) {
                        ImGui::Text("%.0f MB", hd_mb);
                    } else {
                        ImGui::Text("%.1f GB", hd_mb / 1024.0f);
                    }
                    if (font_mono) ImGui::PopFont();

                    // UHD memory usage
                    ImGui::TableNextColumn();
                    if (font_mono) ImGui::PushFont(font_mono);
                    float uhd_mb = (frames_24fps * uhd_pixels * info.bytes_per_pixel) / (1024.0f * 1024.0f);
                    if (uhd_mb < 1000) {
                        ImGui::Text("%.0f MB", uhd_mb);
                    } else {
                        ImGui::Text("%.1f GB", uhd_mb / 1024.0f);
                    }
                    if (font_mono) ImGui::PopFont();
                }

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::TextColored(Bright(GetWindowsAccentColor()), "Memory usage automatically managed per video");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Hardware Decode Settings
            ImGui::Text("Hardware Decode:");
            if (ImGui::Checkbox("Enable NVIDIA Hardware Decode", &cache_settings.enable_nvidia_decode)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Use NVDEC for hardware-accelerated decoding on NVIDIA GPUs.\nFalls back to D3D11VA if unavailable.");
            }

            ImGui::Spacing();

            // Threading Settings
            ImGui::Text("Background Extraction Threading:");

            // Batch Size
            ImGui::Text("Frames per Batch:");
            if (ImGui::SliderInt("##BatchSize", &cache_settings.max_batch_size, 1, 25)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Number of frames processed together.\nSmaller batches = more responsive, larger batches = better throughput.");
            }

            // Thread Count
            ImGui::Text("Extraction Threads:");
            if (ImGui::SliderInt("##ThreadCount", &cache_settings.max_concurrent_batches, 1, 16)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Number of parallel extraction threads.\nMore threads = faster caching but higher CPU/memory usage.\nRecommended: 4-8 threads for best performance.");
            }

            ImGui::EndDisabled(); // End cache_enabled disable block

                        ImGui::EndTabItem();
                    } // End Video Cache tab

                    // === TAB 2: Image Sequence Settings ===
                    if (ImGui::BeginTabItem("Image Sequence")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Image sequence cache (EXR/TIFF/PNG/JPEG) with background loading");
                    ImGui::Spacing();

                    // Image Sequence Cache Size (RAM)
                    ImGui::Text("Image Sequence Cache Size:");
                    float exr_cache_gb_float = static_cast<float>(g_exr_cache_gb);
                    if (ImGui::SliderFloat("##ImageSeqCacheSize", &exr_cache_gb_float, 2.0f, 96.0f, "%.1f GB")) {
                        g_exr_cache_gb = static_cast<double>(exr_cache_gb_float);
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "RAM cache for image sequence frames (EXR/TIFF/PNG/JPEG).\n\n"
                            "Higher = more frames cached = smoother scrubbing\n\n"
                            "Memory per frame (4K):\n"
                            "  - EXR (half-float): ~63 MB\n"
                            "  - TIFF 16-bit: ~33 MB\n"
                            "  - PNG 8-bit: ~17 MB\n\n"
                            "Example (4K EXR):\n"
                            "  - 18GB = ~290 frames (~12 seconds @ 24fps)\n"
                            "  - 36GB = ~580 frames (~24 seconds @ 24fps)\n\n"
                            "Recommended: 18-36GB for 4K EXR work");
                    }

                    // Calculate and display frame count
                    int estimated_4k_frames = static_cast<int>((g_exr_cache_gb * 1024.0) / 63.0);
                    double estimated_4k_seconds = estimated_4k_frames / 24.0;
                    ImGui::Spacing();
                    if (font_mono) ImGui::PushFont(font_mono);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "4K Cache: ~%d frames (~%.1f seconds @ 24fps)",
                        estimated_4k_frames, estimated_4k_seconds);
                    if (font_mono) ImGui::PopFont();

                    // Read-Behind Time
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Read-Behind Time:");
                    if (ImGui::SliderFloat("##ReadBehind", &g_read_behind_seconds, 0.0f, 5.0f, "%.1f sec")) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Seconds to keep cached behind current frame.\nUseful for smooth reverse scrubbing.");
                    }

                    // OpenEXR Threading Info
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "OpenEXR Multi-Threading");
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Automatic multi-threaded DWAB/ZIP decompression");
                    ImGui::Spacing();

                    int hw_threads = std::thread::hardware_concurrency();
                    if (font_mono) ImGui::PushFont(font_mono);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "OpenEXR Threads: %d (auto-detected from CPU)", hw_threads);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Background Loaders: 16 concurrent pixel loads");
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Thread Pool: std::async (reuses threads efficiently)");
                    if (font_mono) ImGui::PopFont();

                        ImGui::EndTabItem();
                    } // End Image Sequence tab

                    // === TAB 3: Thumbnails ===
                    if (ImGui::BeginTabItem("Thumbnails")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Timeline thumbnail preview settings");
                    ImGui::Spacing();

                    // Enable/Disable thumbnails
                    if (ImGui::Checkbox("Enable Timeline Thumbnails", &cache_settings.enable_thumbnails)) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Show thumbnail preview when hovering over the timeline.\n"
                            "Image sequences only (EXR/TIFF/PNG/JPEG).\n\n"
                            "Thumbnails are generated on-demand and cached in RAM.");
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Thumbnail dimensions
                    ImGui::BeginDisabled(!cache_settings.enable_thumbnails);

                    ImGui::Text("Thumbnail Size:");
                    int thumb_width = cache_settings.thumbnail_width;
                    int thumb_height = cache_settings.thumbnail_height;
                    if (ImGui::SliderInt("Width##ThumbWidth", &thumb_width, 160, 640, "%d px")) {
                        cache_settings.thumbnail_width = thumb_width;
                        settings_changed = true;
                    }
                    if (ImGui::SliderInt("Height##ThumbHeight", &thumb_height, 90, 360, "%d px")) {
                        cache_settings.thumbnail_height = thumb_height;
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Dimensions for timeline thumbnail previews.\n\n"
                            "Default: 320x180 (16:9 aspect ratio)\n"
                            "Range: 160x90 to 640x360\n\n"
                            "Larger = better quality, more memory");
                    }

                    // Thumbnail cache size
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Thumbnail Cache Size:");
                    int thumb_cache_size = cache_settings.thumbnail_cache_size;
                    if (ImGui::SliderInt("##ThumbCacheSize", &thumb_cache_size, 50, 500, "%d thumbnails")) {
                        cache_settings.thumbnail_cache_size = thumb_cache_size;
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Number of thumbnails to keep in RAM cache (LRU eviction).\n\n"
                            "Memory usage (320x180): ~225 KB per thumbnail\n"
                            "  - 100 thumbnails = ~22 MB\n"
                            "  - 200 thumbnails = ~45 MB\n"
                            "  - 500 thumbnails = ~110 MB\n\n"
                            "Thumbnails are generated on-demand as you hover.");
                    }

                    // Display memory estimate
                    ImGui::Spacing();
                    if (font_mono) ImGui::PushFont(font_mono);
                    double thumb_memory_mb = (cache_settings.thumbnail_width * cache_settings.thumbnail_height * 4.0 * cache_settings.thumbnail_cache_size) / (1024.0 * 1024.0);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Thumbnail Cache: ~%.1f MB (%d thumbnails @ %dx%d)",
                        thumb_memory_mb, cache_settings.thumbnail_cache_size,
                        cache_settings.thumbnail_width, cache_settings.thumbnail_height);
                    if (font_mono) ImGui::PopFont();

                    ImGui::EndDisabled();

                        ImGui::EndTabItem();
                    } // End Thumbnails tab

                    // === TAB 4: Performance ===
                    if (ImGui::BeginTabItem("Performance")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Threading and parallel processing settings");
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Image Sequence I/O Threading Settings
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "Image Sequence I/O Threading");
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Parallel loading for image sequences (EXR/TIFF/PNG/JPEG)");
                    ImGui::Spacing();

                    // Image Sequence Thread Count
                    ImGui::Text("Parallel I/O Threads:");
                    if (ImGui::SliderInt("##ImageSeqThreadCount", &g_exr_thread_count, 1, 32)) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Number of parallel threads for image sequence loading.\n\n"
                            "More threads = faster cache filling\n"
                            "Works for all formats: EXR, TIFF, PNG, JPEG\n\n"
                            "Recommended:\n"
                            "  - 8-16 threads for fast SSDs\n"
                            "  - 4-8 threads for HDDs\n"
                            "  - 16-32 threads for network storage\n\n"
                            "Note: EXR files also use OpenEXR's internal multi-threading\n"
                            "for DWAB/ZIP decompression automatically");
                    }

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Current: %d parallel loaders", g_exr_thread_count);

                    // Image Sequence Transcode Threading Settings
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "Image Sequence Transcode Threading");
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Parallel transcoding for EXR single-layer conversion");
                    ImGui::Spacing();

                    // Transcode Thread Count
                    ImGui::Text("Parallel Transcode Threads:");
                    if (ImGui::SliderInt("##TranscodeThreadCount", &g_exr_transcode_threads, 1, 16)) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Number of frames transcoded in parallel.\n\n"
                            "When converting multilayer EXR to single-layer:\n"
                            "  - Read EXR: Parallel\n"
                            "  - Resize (if enabled): Parallel using stb_image_resize2\n"
                            "  - Write EXR: Parallel\n\n"
                            "More threads = faster transcode but more RAM usage\n"
                            "Each thread holds one frame in memory during transcode\n\n"
                            "Recommended: 4-8 threads for balanced performance");
                    }

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Current: %d parallel transcoders", g_exr_transcode_threads);

                        ImGui::EndTabItem();
                    } // End Performance tab

                    // === TAB 5: Disk Cache ===
                    if (ImGui::BeginTabItem("Disk Cache")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Auto-cleanup and storage management for cached files");
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Custom Cache Path
                    ImGui::Text("Cache Location:");
                    ImGui::PushItemWidth(-1);
                    if (font_mono) ImGui::PushFont(font_mono);
                    if (g_custom_cache_path.empty()) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Default: %%LOCALAPPDATA%%\\ump\\");
                    } else {
                        ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "%s", g_custom_cache_path.c_str());
                    }
                    if (font_mono) ImGui::PopFont();
                    ImGui::PopItemWidth();

                    if (ImGui::Button("Change Cache Folder...", ImVec2(-1, 0))) {
                        nfdchar_t* out_path = nullptr;
                        nfdresult_t result = NFD_PickFolder(&out_path, nullptr);
                        if (result == NFD_OKAY) {
                            g_custom_cache_path = out_path;
                            settings_changed = true;
                            NFD_FreePath(out_path);
                        }
                    }
                    if (ImGui::Button("Reset to Default", ImVec2(-1, 0))) {
                        g_custom_cache_path = "";
                        settings_changed = true;
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Cache Retention Days
                    ImGui::Text("Cache Retention:");
                    if (ImGui::SliderInt("##CacheRetentionDays", &g_cache_retention_days, 1, 30, "%d days")) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Auto-delete cache files older than N days on startup.\n\n"
                            "Files are only deleted when app launches.\n"
                            "Default: 7 days");
                    }

                    // Dummy Cache Size Limit
                    ImGui::Spacing();
                    ImGui::Text("Dummy Video Cache Limit:");
                    if (ImGui::SliderInt("##DummyCacheMaxGB", &g_dummy_cache_max_gb, 1, 10, "%d GB")) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Maximum disk space for dummy video cache.\n\n"
                            "When exceeded on startup, all dummy videos are deleted.\n"
                            "Dummy videos are small black videos used as placeholders.\n"
                            "Default: 1 GB");
                    }

                    // Transcode Cache Size Limit
                    ImGui::Spacing();
                    ImGui::Text("EXR Transcode Cache Limit:");
                    if (ImGui::SliderInt("##TranscodeCacheMaxGB", &g_transcode_cache_max_gb, 5, 100, "%d GB")) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Maximum disk space for EXR transcode cache.\n\n"
                            "When exceeded on startup, oldest 50%% of transcodes are deleted.\n"
                            "Transcodes are converted single-layer EXR sequences.\n"
                            "Default: 10 GB");
                    }

                    // Clear Cache on Exit
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Checkbox("Clear all cache on exit", &g_clear_cache_on_exit)) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Delete all dummy videos and EXR transcodes on app close.\n\n"
                            "WARNING: This will block app shutdown until cleanup completes.\n"
                            "May take several seconds if you have large caches.\n\n"
                            "Default: OFF (lazy cleanup on next launch is recommended)");
                    }

                    // Note about cache clearing
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Manual Cache Clearing:");
                    ImGui::Spacing();
                    ImGui::TextWrapped("To clear image sequence cache (dummies + transcodes), use Cache > Clear Image Sequence Disk Cache from the main menu.");

                        ImGui::EndTabItem();
                    } // End Disk Cache tab

                    ImGui::EndTabBar();
                } // End tab bar
                ImGui::EndChild();
            }

            ImGui::Spacing();
            ImGui::Separator();

            // Buttons
            if (ImGui::Button("Apply Settings") && settings_changed) {
                // Apply pipeline mode change (only for regular video, not EXR/image sequences)
                bool is_exr_mode = video_player && video_player->IsInEXRMode();
                bool is_image_sequence = project_manager && project_manager->IsInImageSequenceMode();

                if (!is_exr_mode && !is_image_sequence) {
                    // Regular video: allow pipeline mode changes
                    if (video_player && cache_settings.current_pipeline_mode != video_player->GetPipelineMode()) {
                        video_player->SetPipelineMode(cache_settings.current_pipeline_mode);
                        Debug::Log("Applied pipeline mode: " + std::string(PipelineModeToString(cache_settings.current_pipeline_mode)));

                        // Reload current media to apply new pipeline settings
                        if (!current_file_path.empty()) {
                            Debug::Log("Reloading media for new pipeline mode: " + current_file_path);

                            // Route through project manager for proper cache eviction
                            if (project_manager) {
                                project_manager->LoadSingleFileFromDrop(current_file_path);
                            } else {
                                // Fallback to direct loading if project manager unavailable
                                video_player->LoadFile(current_file_path);
                                if (timeline_manager) {
                                    timeline_manager->SetVideoFile(current_file_path);
                                }
                            }
                        }
                    }
                } else {
                    // EXR/image sequences: pipeline mode is locked, just log
                    if (is_exr_mode) {
                        Debug::Log("Pipeline mode locked for EXR sequences (Float16 fixed)");
                    } else if (is_image_sequence) {
                        Debug::Log("Pipeline mode locked for image sequences (auto-detected from bit depth)");
                    }
                }

                // Apply cache configuration
                if (project_manager) {
                    // Create FrameCache::CacheConfig for seconds-based cache
                    FrameCache::CacheConfig config;
                    config.max_cache_seconds = cache_settings.max_cache_seconds; // NEW: Seconds-based limit

                    // Set pipeline mode for texture format matching
                    config.pipeline_mode = cache_settings.current_pipeline_mode;

                    // Set RAM cache defaults
                    config.use_centered_caching = true;         // Focus on current position
                    config.cache_width = 1920;                  // Standard resolution for consistent quality
                    config.cache_height = -1;                   // Calculate from aspect ratio
                    // Keyframe cache removed - background extractor handles all caching
                    config.adaptive_threading = true;           // Slow down during playback
                    config.max_extractions_per_second = 100;    // Reasonable limit for background caching
                    config.enable_nvidia_decode = cache_settings.enable_nvidia_decode;  // Hardware decode setting

                    // Apply threading settings
                    config.max_batch_size = cache_settings.max_batch_size;
                    config.max_concurrent_batches = cache_settings.max_concurrent_batches;

                    project_manager->SetCacheConfig(config);

                    Debug::Log("Applied cache settings: " + std::to_string(cache_settings.max_cache_seconds) + "s cache window for " +
                               std::string(PipelineModeToString(cache_settings.current_pipeline_mode)) + " mode");
                }

                // Apply EXR-specific settings (NEW: DirectEXRCacheConfig)
                if (video_player) {
                    ump::DirectEXRCacheConfig exr_config = GetCurrentEXRCacheConfig();

                    // Apply config through VideoPlayer
                    if (video_player->HasEXRCache()) {
                        video_player->SetEXRCacheConfig(exr_config);

                        Debug::Log("Applied image sequence cache settings: " +
                                   std::to_string(exr_config.video_cache_gb) + "GB cache, " +
                                   std::to_string(exr_config.read_behind_seconds) + "s read behind, " +
                                   std::to_string(exr_config.gpu_memory_pool_mb) + "MB GPU pool");
                    } else {
                        Debug::Log("Image sequence cache settings configured (will apply when sequence is loaded)");
                    }

                    // Apply disk cache settings to DummyVideoGenerator and EXRTranscoder
                    video_player->SetCacheSettings(g_custom_cache_path, g_cache_retention_days,
                                                   g_dummy_cache_max_gb, g_transcode_cache_max_gb,
                                                   g_clear_cache_on_exit);
                    Debug::Log("Applied disk cache settings: retention=" + std::to_string(g_cache_retention_days) +
                              " days, dummy limit=" + std::to_string(g_dummy_cache_max_gb) +
                              " GB, transcode limit=" + std::to_string(g_transcode_cache_max_gb) + " GB" +
                              ", clear on exit=" + std::string(g_clear_cache_on_exit ? "ON" : "OFF"));

                    // Disk cache settings applied above
                }

                // Save settings to disk
                SaveSettings();

                settings_changed = false;

                // Close the settings window
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Reset to Defaults")) {
                // Pipeline and cache settings
                cache_settings.current_pipeline_mode = PipelineMode::NORMAL;
                cache_settings.max_cache_size_mb = 12288;
                cache_settings.max_cache_seconds = 20;

                // Video settings
                cache_settings.enable_nvidia_decode = false;
                cache_settings.max_batch_size = 8;
                cache_settings.max_concurrent_batches = 8;

                // EXR settings - Auto-configure based on CPU
                AutoConfigureEXRThreading(cache_settings);

                // GPU memory settings
                cache_settings.gpu_memory_pool_mb = 2048;

                // Thumbnail settings
                cache_settings.enable_thumbnails = true;
                cache_settings.thumbnail_width = 160;
                cache_settings.thumbnail_height = 90;
                cache_settings.thumbnail_cache_size = 50;

                // Disk cache settings
                g_custom_cache_path = "";
                g_cache_retention_days = 7;
                g_dummy_cache_max_gb = 1;
                g_transcode_cache_max_gb = 10;
                g_clear_cache_on_exit = false;

                settings_changed = true;

                // Save the defaults immediately
                SaveSettings();
                Debug::Log("Settings reset to defaults and saved");
            }

            ImGui::SameLine();

            if (ImGui::Button("Close")) {
                ImGui::CloseCurrentPopup();
            }

            // Handle X button click
            if (!open) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void CreateVideoViewport() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin("Video Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImVec2 content_region = ImGui::GetContentRegionAvail();
            ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
            ImVec2 canvas_size = ImGui::GetContentRegionAvail();

            DrawVideoBackground(canvas_pos, canvas_size, 20.0f);

            // Process annotation drawing input if in annotation mode
            if (viewport_annotator && viewport_annotator->IsAnnotationMode() && video_player) {
                int video_width = video_player->GetVideoWidth();
                int video_height = video_player->GetVideoHeight();

                if (video_width > 0 && video_height > 0) {
                    // Calculate display bounds (same logic as video rendering)
                    float aspect_ratio = (float)video_width / video_height;
                    ImVec2 display_size = canvas_size;

                    if (canvas_size.x / aspect_ratio <= canvas_size.y) {
                        display_size.y = canvas_size.x / aspect_ratio;
                    } else {
                        display_size.x = canvas_size.y * aspect_ratio;
                    }

                    ImVec2 display_pos = ImVec2(
                        canvas_pos.x + (canvas_size.x - display_size.x) * 0.5f,
                        canvas_pos.y + (canvas_size.y - display_size.y) * 0.5f
                    );

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
            bool used_cached_frame = false;
            if (timeline_manager && (timeline_manager->IsScrubbing() || timeline_manager->IsHoldingCachedFrame())) {
                GLuint cached_texture_id = 0;
                int cached_width = 0, cached_height = 0;
                double current_time = timeline_manager->GetUIPosition();

                if (timeline_manager->GetCachedFrameForScrubbing(current_time, cached_texture_id, cached_width, cached_height)) {
                    // Use cached frame for instant scrubbing feedback
                    // Debug::Log("Using cached frame for scrubbing at " + std::to_string(current_time) + 
                    //           "s (texture_id: " + std::to_string(cached_texture_id) + 
                    //           ", size: " + std::to_string(cached_width) + "x" + std::to_string(cached_height) + ")");

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
                    GLuint display_texture = cached_texture_id;  // Default to original texture

                    if (video_player && video_player->HasColorPipeline()) {
                        // Create color-corrected version of cached frame
                        GLuint color_corrected_texture = video_player->CreateColorCorrectedTexture(
                            cached_texture_id, cached_width, cached_height,
                            (int)display_size.x, (int)display_size.y
                        );

                        if (color_corrected_texture != 0) {
                            display_texture = color_corrected_texture;

                            // Store for cleanup - we need to delete this texture after rendering
                            // For simplicity, we'll delete it immediately after ImGui call
                            static std::vector<GLuint> textures_to_cleanup;
                            textures_to_cleanup.push_back(color_corrected_texture);

                            // Clean up old textures (from previous frames)
                            for (size_t i = 0; i < textures_to_cleanup.size() - 1; i++) {
                                glDeleteTextures(1, &textures_to_cleanup[i]);
                            }
                            if (textures_to_cleanup.size() > 1) {
                                textures_to_cleanup.erase(textures_to_cleanup.begin(), textures_to_cleanup.end() - 1);
                            }
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
                if (timeline_manager && timeline_manager->IsHoldingCachedFrame()) {
                    // Don't render video frame while holding cached frame to avoid flashing old frame
                    // Draw solid black to prevent OpenGL texture flash
                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    draw_list->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                        IM_COL32(0, 0, 0, 255));
                }
                else {
                    video_player->RenderVideoFrame();

                    // NOTE: Global memory limit enforcement removed - now using seconds-based cache windows

                    // Render annotation overlays (MIDDLE LAYER - after video, before safety overlays)
                    if (annotation_renderer && viewport_annotator) {
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();

                        // Get display size/position for coordinate conversion
                        // Calculate display size maintaining aspect ratio (same logic as video rendering)
                        if (video_player) {
                            int video_width = video_player->GetVideoWidth();
                            int video_height = video_player->GetVideoHeight();

                            if (video_width > 0 && video_height > 0) {
                                float aspect_ratio = (float)video_width / video_height;
                                ImVec2 display_size = canvas_size;

                                if (canvas_size.x / aspect_ratio <= canvas_size.y) {
                                    display_size.y = canvas_size.x / aspect_ratio;
                                } else {
                                    display_size.x = canvas_size.y * aspect_ratio;
                                }

                                // Center the display area
                                ImVec2 display_pos = ImVec2(
                                    canvas_pos.x + (canvas_size.x - display_size.x) * 0.5f,
                                    canvas_pos.y + (canvas_size.y - display_size.y) * 0.5f
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

                                if (viewport_annotator->IsAnnotationMode()) {
                                    // Always render in annotation mode
                                    should_render_annotations = true;
                                } else if (annotations_enabled && annotation_manager && video_player) {
                                    // Check if current frame has an annotation to display (only if annotations enabled)
                                    double current_time = video_player->GetPosition();
                                    int current_frame = video_player->GetCurrentFrame();

                                    // Look for annotation at current frame
                                    const auto& notes = annotation_manager->GetNotes();
                                    for (const auto& note : notes) {
                                        // Match by frame number for precision
                                        if (note.frame == current_frame && !note.annotation_data.empty()) {
                                            // Load and render this annotation's strokes
                                            auto strokes = ump::Annotations::AnnotationSerializer::JsonStringToStrokes(note.annotation_data);
                                            // Render strokes (no debug log - this runs every frame)
                                            for (const auto& stroke : strokes) {
                                                annotation_renderer->RenderActiveStroke(
                                                    draw_list, stroke, display_pos, display_size, true  // Apply smoothing to loaded strokes
                                                );
                                            }
                                            break; // Only one annotation per frame
                                        }
                                    }
                                }

                                // Render editing strokes (only in annotation mode)
                                if (viewport_annotator->IsAnnotationMode()) {
                                    // Render all stored strokes for the current annotation being edited
                                    for (const auto& stroke : current_annotation_strokes_) {
                                        annotation_renderer->RenderActiveStroke(
                                            draw_list, stroke, display_pos, display_size, true  // Apply smoothing to completed strokes
                                        );
                                    }

                                    // Render active stroke being drawn (if any)
                                    const auto* active_stroke = viewport_annotator->GetActiveStroke();
                                    if (active_stroke) {
                                        annotation_renderer->RenderActiveStroke(
                                            draw_list, *active_stroke, display_pos, display_size, true
                                        );
                                    }
                                }

                                // Render pending capture strokes (for export)
                                if (pending_capture.pending && !pending_capture.strokes.empty()) {
                                    // Render annotation strokes
                                    for (const auto& stroke : pending_capture.strokes) {
                                        annotation_renderer->RenderActiveStroke(
                                            draw_list, stroke, display_pos, display_size, true
                                        );
                                    }
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

                        // Debug: Log the actual color values being used for rendering
                        static int debug_counter = 0;
                        if (debug_counter++ % 120 == 0) { // Log every 2 seconds at 60 FPS
                            Debug::Log("Safety overlay rendering with color: R=" + std::to_string((int)(safety_settings.color[0] * 255)) +
                                      " G=" + std::to_string((int)(safety_settings.color[1] * 255)) +
                                      " B=" + std::to_string((int)(safety_settings.color[2] * 255)) +
                                      " (hex: " + std::to_string(overlay_color) + ")");
                        }

                        video_player->RenderSVGOverlays(draw_list, canvas_pos, canvas_size,
                            safety_settings.opacity, overlay_color, safety_settings.line_width);
                    }
                }
            }

            // Audio waveform visualization for audio-only files
            if (video_player && video_player->IsAudioOnly()) {
                DrawAudioWaveform(canvas_pos, canvas_size);
            }

            // TEMPORARY: Always show waveform for testing
            // TODO: Remove this test code once audio detection works
            if (video_player && !current_file_path.empty()) {
                // Check if current file looks like audio based on extension
                std::string ext = current_file_path.substr(current_file_path.find_last_of("."));
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".mp3" || ext == ".wav" || ext == ".aac" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
                    DrawAudioWaveform(canvas_pos, canvas_size);
                }
            }

            // ImGui requires a Dummy() item after using GetCursorScreenPos() to properly size the window
            ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y));
            ImGui::Dummy(ImVec2(0, 0));
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void RenderBackgroundSelectionPanel(VideoBackgroundType& bg_type, bool& show_panel) {
        if (!show_panel) return;

        ImGuiWindow* video_window = ImGui::FindWindowByName("Video Viewport");
        if (!video_window) return;

        ImVec2 video_pos = video_window->Pos;
        ImVec2 video_size = video_window->Size;

        const float panel_width = 200.0f;
        const float panel_height = 116.0f;
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

    void RenderFrameioImportDialog() {
        if (!frameio_import_state.show_dialog) return;

        // Token is automatically loaded from settings on app start via LoadSettings()
        // No need to manually load it here

        ImGui::OpenPopup("Import from Frame.io");

        ImGui::SetNextWindowSize(ImVec2(500, 350), ImGuiCond_FirstUseEver);

        if (ImGui::BeginPopupModal("Import from Frame.io", &frameio_import_state.show_dialog, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Import annotations from Frame.io review links");
            ImGui::Separator();
            ImGui::Spacing();

            // API Token input (password style)
            ImGui::Text("API Token:");
            ImGui::PushItemWidth(-1);
            ImGui::InputText("##token", frameio_import_state.token_buffer, 256, ImGuiInputTextFlags_Password);
            ImGui::PopItemWidth();

            // Token management buttons (side by side)
            float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            // Save Token button
            bool token_not_empty = strlen(frameio_import_state.token_buffer) > 0;
            if (!token_not_empty) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Save Token", ImVec2(button_width, 0))) {
                SaveSettings();
                frameio_import_state.status_message = "Token saved successfully";
                frameio_import_state.import_success = true;
            }
            if (!token_not_empty) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();

            // Clear Saved Token button
            bool has_saved_token = HasSavedFrameioToken();
            if (!has_saved_token) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Clear Saved Token", ImVec2(button_width, 0))) {
                ClearSavedToken();
                frameio_import_state.token_buffer[0] = '\0';
                frameio_import_state.status_message = "Saved token cleared";
                frameio_import_state.import_success = true;
            }
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

            // Buttons
            if (frameio_import_state.importing) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button("Import", ImVec2(120, 0))) {
                // Start import
                frameio_import_state.importing = true;
                frameio_import_state.status_message = "Importing...";

                std::string token = frameio_import_state.token_buffer;
                std::string url = frameio_import_state.url_buffer;

                // Parse URL to get asset_id
                auto parse_result = ump::Integrations::FrameioUrlParser::Parse(url);

                if (!parse_result.success) {
                    frameio_import_state.status_message = "Error: " + parse_result.error_message;
                    frameio_import_state.importing = false;
                    frameio_import_state.import_success = false;
                } else {
                    // Fetch comments from Frame.io
                    auto fetch_result = ump::Integrations::FrameioClient::GetAssetComments(
                        parse_result.asset_id,
                        token
                    );

                    if (!fetch_result.success) {
                        frameio_import_state.status_message = "Error: " + fetch_result.error_message;
                        frameio_import_state.importing = false;
                        frameio_import_state.import_success = false;
                    } else {
                        // Convert Frame.io comments to ump annotations
                        double framerate = video_player ? video_player->GetFrameRate() : 24.0;
                        auto convert_result = ump::Integrations::FrameioConverter::ConvertComments(
                            fetch_result.comments,
                            framerate
                        );

                        if (!convert_result.success) {
                            frameio_import_state.status_message = "Error: " + convert_result.error_message;
                            frameio_import_state.importing = false;
                            frameio_import_state.import_success = false;
                        } else {
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

            ImGui::SameLine();

            if (ImGui::Button("Close", ImVec2(120, 0))) {
                frameio_import_state.show_dialog = false;
            }

            if (frameio_import_state.importing) {
                ImGui::EndDisabled();
            }

            ImGui::EndPopup();
        }
    }


    void RenderShutdownModal() {
        if (!is_shutting_down_) return;

        // Update animation time (increment for smooth animation)
        shutdown_animation_time_ += 0.016f; // ~60fps increment

        // Get main viewport for fullscreen overlay and centering
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = viewport->GetCenter();
        ImVec2 display_size = viewport->Size;

        // Draw fullscreen dimmed overlay
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
        draw_list->AddRectFilled(
            ImVec2(0, 0),
            display_size,
            IM_COL32(0, 0, 0, 185)  // Semi-transparent black (65% opacity)
        );

        // Now draw the modal window on top
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(450, 220), ImGuiCond_Always);

        ImGuiWindowFlags modal_flags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.128f, 0.128f, 0.128f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 2.0f));

        if (ImGui::Begin("##ShutdownModal", nullptr, modal_flags)) {
            ImGui::Dummy(ImVec2(0, 20));

            // Hourglass icon (centered, larger)
            ImGui::PushFont(font_icons);
            const char* icon = ICON_HOURGLASS_EMPTY;
            float icon_size = ImGui::CalcTextSize(icon).x * 2.0f; // Make it bigger
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - icon_size) * 0.5f);
            ImGui::SetWindowFontScale(2.0f);
            ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "%s", icon);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();

            ImGui::Dummy(ImVec2(0, 10));

            // Title (centered)
            const char* title = "Shutting Down ump...";
            float title_width = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - title_width) * 0.5f);
            ImGui::Text("%s", title);

            ImGui::Dummy(ImVec2(0, 4));

            // Status text (centered)
            const char* status = "Cleaning up GPU resources";
            float status_width = ImGui::CalcTextSize(status).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - status_width) * 0.5f);
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", status);

            ImGui::Dummy(ImVec2(0, 4));

            // Please wait text (centered)
            const char* wait_text = "Please wait...";
            float wait_width = ImGui::CalcTextSize(wait_text).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - wait_width) * 0.5f);
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", wait_text);

            ImGui::Dummy(ImVec2(0, 20));
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void RenderSafetyOverlayPanel(bool& show_panel) {
        if (!show_panel) return;

        ImGuiWindow* video_window = ImGui::FindWindowByName("Video Viewport");
        if (!video_window) return;

        ImVec2 video_pos = video_window->Pos;
        ImVec2 video_size = video_window->Size;

        const float panel_width = 315.0f;
        const float panel_height = 695.0f;
        const float margin = 10.0f;

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
                        bool is_16x9_active = (current_svg == "assets/safety/16x9.svg");
                        if (is_16x9_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("16:9 Broadcast")) {
                            Debug::Log("*** BUTTON CLICKED: 16:9 Standard ***");
                            Debug::Log("Loading 16:9 standard SVG");
                            bool result = svg_renderer->LoadSafetyOverlaySVG("assets/safety/16x9.svg");
                            Debug::Log("LoadSafetyOverlaySVG result: " + std::string(result ? "SUCCESS" : "FAILED"));
                            video_player->EnableSafetyOverlays(true);
                            Debug::Log("EnableSafetyOverlays(true) called");
                        }

                        if (is_16x9_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_youtube_masthead_active = (current_svg == "assets/safety/Youtube_16x9_Masthead.svg");
                        if (is_youtube_masthead_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("YouTube Masthead 16:9")) {
                            Debug::Log("Loading YouTube Masthead SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Youtube_16x9_Masthead.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_masthead_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();

                        bool is_youtube_16x9_active = (current_svg == "assets/safety/Youtube_16x9.svg");
                        if (is_youtube_16x9_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("YouTube 16:9")) {
                            Debug::Log("Loading YouTube 16:9 SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Youtube_16x9.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_16x9_active) {
                            ImGui::PopStyleColor();
                        }

                        ImGui::Spacing();
                        ImGui::TextDisabled("9:16 Vertical:");

                        if (ImGui::Button("TikTok 9:16")) {
                            Debug::Log("*** BUTTON CLICKED: TikTok 9:16 ***");
                            Debug::Log("Loading TikTok SVG");
                            bool result = svg_renderer->LoadSafetyOverlaySVG("assets/safety/TikTok_9x16.svg");
                            Debug::Log("LoadSafetyOverlaySVG result: " + std::string(result ? "SUCCESS" : "FAILED"));
                            video_player->EnableSafetyOverlays(true);
                            Debug::Log("EnableSafetyOverlays(true) called");
                        }
                        ImGui::SameLine();

                        bool is_youtube_9x16_active = (current_svg == "assets/safety/Youtube_9x16.svg");
                        if (is_youtube_9x16_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("YouTube 9:16")) {
                            Debug::Log("Loading YouTube 9:16 SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Youtube_9x16.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_9x16_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_meta_reels_active = (current_svg == "assets/safety/Meta_Reels_9x16.svg");
                        if (is_meta_reels_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("Meta Reels 9:16")) {
                            Debug::Log("Loading Meta Reels SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Meta_Reels_9x16.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_meta_reels_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();

                        bool is_meta_stories_active = (current_svg == "assets/safety/Meta_Stories_9x16.svg");
                        if (is_meta_stories_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("Meta Stories 9x16")) {
                            Debug::Log("Loading Meta Stories SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Meta_Stories_9x16.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_meta_stories_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_pinterest_9x16_active = (current_svg == "assets/safety/Pinterest_9x16.svg");
                        if (is_pinterest_9x16_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("Pinterest 9:16")) {
                            Debug::Log("Loading Pinterest 9:16 SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Pinterest_9x16.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_pinterest_9x16_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();
                        bool is_samsung_active = (current_svg == "assets/safety/Samsung_9x16_Safety.svg");
                        if (is_samsung_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("Samsung Safety 9:16")) {
                            Debug::Log("Loading Samsung Safety SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Samsung_9x16_Safety.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_samsung_active) {
                            ImGui::PopStyleColor();
                        }

                        bool is_snapchat_active = (current_svg == "assets/safety/Snapchat_9x16_unofficial.svg");
                        if (is_snapchat_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("Snapchat 9:16")) {
                            Debug::Log("Loading Snapchat SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Snapchat_9x16_unofficial.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_snapchat_active) {
                            ImGui::PopStyleColor();
                        }

                        ImGui::Spacing();
                        ImGui::TextDisabled("1x1 Square:");

                        bool is_youtube_1x1_active = (current_svg == "assets/safety/Youtube_1x1.svg");
                        if (is_youtube_1x1_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("YouTube 1:1")) {
                            Debug::Log("Loading YouTube 1:1 SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Youtube_1x1.svg");
                            video_player->EnableSafetyOverlays(true);
                        }

                        if (is_youtube_1x1_active) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::SameLine();
                        bool is_pinterest_1x1_active = (current_svg == "assets/safety/Pinterest_PremiumSpotlight_1x1.svg");
                        if (is_pinterest_1x1_active) {
                            ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                        }

                        if (ImGui::Button("Pinterest Spotlight 1:1")) {
                            Debug::Log("Loading Pinterest Premium Spotlight SVG");
                            svg_renderer->LoadSafetyOverlaySVG("assets/safety/Pinterest_PremiumSpotlight_1x1.svg");
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

                        if (ImGui::Button("None / Disable")) {
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


    void RenderColorspacePresetsPanel(bool& show_panel) {
        if (!show_panel) return;

        ImGuiWindow* video_window = ImGui::FindWindowByName("Video Viewport");
        if (!video_window) return;

        ImVec2 video_pos = video_window->Pos;
        ImVec2 video_size = video_window->Size;

        const float panel_width = 280.0f;
        const float panel_height = 430.0f;
        const float margin = 10.0f;

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

            // Standard Presets Section (using Blender config internally)
            ImGui::PushStyleColor(ImGuiCol_Text, Bright(GetWindowsAccentColor()));
            if (ImGui::CollapsingHeader("Standard Workflows", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PopStyleColor();
                ImGui::Spacing();
                CreateStandardPresets();
                ImGui::Spacing();
            }
            else {
                ImGui::PopStyleColor();
            }

            // ACES Presets Section
            ImGui::PushStyleColor(ImGuiCol_Text, Bright(GetWindowsAccentColor()));
            if (ImGui::CollapsingHeader("ACES Workflows", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PopStyleColor();
                ImGui::Spacing();
                CreateACESPresets();
                ImGui::Spacing();
            }
            else {
                ImGui::PopStyleColor();
            }

            // Blender Presets Section
            ImGui::PushStyleColor(ImGuiCol_Text, Bright(GetWindowsAccentColor()));
            if (ImGui::CollapsingHeader("Blender Workflows", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PopStyleColor();
                ImGui::Spacing();
                CreateBlenderPresets();
                ImGui::Spacing();
            }
            else {
                ImGui::PopStyleColor();
            }

            // Reset Section at Bottom
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextDisabled("Reset");

            if (ImGui::Button("Remove All Color Profiles", ImVec2(-1, 0))) {
                Debug::Log("User requested OCIO pipeline removal from preset panel");
                video_player->ClearColorPipeline();
                show_panel = false; // Close panel after reset
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Clear all color processing and return to original video");
            }
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

    void DrawVideoBackground(ImVec2 canvas_pos, ImVec2 canvas_size, float tile_size = 20.0f) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        switch (video_background_type) {
        case VideoBackgroundType::DEFAULT:
            // Don't draw anything - transparent/default
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
            const ImU32 col_lighter = IM_COL32(220, 220, 220, 255);

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

    void DrawAudioWaveform(ImVec2 canvas_pos, ImVec2 canvas_size) {
        // Get current playback info
        double position = 0.0;
        double duration = 1.0;
        float current_audio_level = 0.0f;
        bool is_playing = false;

        if (video_player) {
            position = video_player->GetPosition();
            duration = video_player->GetDuration();
            current_audio_level = video_player->GetAudioLevel();
            is_playing = video_player->IsPlaying();
        }

        // Set up ImPlot plotting area with proper positioning
        ImGui::SetCursorScreenPos(canvas_pos);
        ImGui::BeginChild("AudioVisualization", canvas_size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Simple minimal audio visualization - vertical white lines on pure black background
        if (ImPlot::BeginPlot("##AudioLines", ImVec2(-1, -1), ImPlotFlags_NoFrame | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText)) {
            // Configure minimal plot appearance - no decorations, transparent background
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, duration > 0 ? duration : 60, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0f, 1.0f, ImGuiCond_Always);

            // Transparent background - let the regular window show through
            ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

            // Rolling audio data - store positions where we have audio hits
            static std::vector<float> audio_positions;
            static std::vector<float> audio_levels;
            static bool is_looping = false;

            // Check if we're looping to clear old data
            bool current_loop_state = video_player ? video_player->IsLooping() : false;
            if (current_loop_state != is_looping) {
                is_looping = current_loop_state;
                if (is_looping) {
                    // Clear all existing lines when loop starts
                    audio_positions.clear();
                    audio_levels.clear();
                }
            }

            // Clear data if we've looped back (position jumped backwards significantly)
            static double last_position = 0.0;
            if (position < last_position - 1.0) { // Detected loop or seek backwards
                audio_positions.clear();
                audio_levels.clear();
            }
            last_position = position;

            // Add new vertical line if playing and audio level is significant
            if (is_playing && current_audio_level > 0.1f) {
                // Check if we already have a line at this position (avoid duplicates)
                bool position_exists = false;
                for (size_t i = 0; i < audio_positions.size(); i++) {
                    if (abs(audio_positions[i] - position) < 0.1) { // Within 0.1 second
                        position_exists = true;
                        break;
                    }
                }

                if (!position_exists) {
                    audio_positions.push_back(position);
                    audio_levels.push_back(current_audio_level);

                    // Limit to 500 lines for performance
                    if (audio_positions.size() > 500) {
                        audio_positions.erase(audio_positions.begin());
                        audio_levels.erase(audio_levels.begin());
                    }
                }
            }

            // Draw vertical white lines
            for (size_t i = 0; i < audio_positions.size(); i++) {
                float pos = audio_positions[i];
                float level = audio_levels[i];

                // Create vertical line data
                float line_x[2] = { pos, pos };
                float line_y[2] = { -level, level }; // Vertical line centered at 0

                ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), 1.0f);
                ImPlot::PlotLine("##line", line_x, line_y, 2);
            }

            ImPlot::PopStyleColor();
            ImPlot::EndPlot();
        }

        ImGui::EndChild();
    }


    void CreateTimelineTransportPanel() {
        if (!show_timeline_panel) return;

        if (ImGui::Begin("Timeline & Transport", &show_timeline_panel,
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoCollapse)) {

            // Get timeline info from UI state (smooth and responsive)
            double duration = timeline_manager->GetUIDuration();
            double position = timeline_manager->GetUIPosition();

            // Fallback to project manager if timeline manager doesn't have valid data yet
            if (duration <= 0) {
                duration = project_manager->GetTimelineDuration();
            }
            if (position < 0) {
                position = project_manager->GetTimelinePosition();
            }

            float button_size = 37.0f;
            float small_button = 37.0f;

            int button_count = 21;  // Added 5 view toggle buttons
            int spacer_count = 10;   // Added 1 spacer before view toggles
            float spacer_width = 15.0f;

            float item_spacing = ImGui::GetStyle().ItemSpacing.x;

            float total_width = (button_count * small_button) +
                (spacer_count * spacer_width) +
                ((button_count + spacer_count - 1) * item_spacing);

            float available_width = ImGui::GetContentRegionAvail().x;
            float center_offset = (available_width - total_width) * 0.5f;

            // Store draw list for later use
            ImDrawList* transport_draw_list = ImGui::GetWindowDrawList();

            // Apply the centering
            if (center_offset > 0.0f) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_offset);
            }

            bool all_panels_shown = show_project_panel && show_inspector_panel && show_color_panels && !minimal_view_mode;

            // === TRANSPORT ROW BUTTON STYLING ===
            // Adjust these values to customize appearance:
            float icon_scale = 1.6f;  // Transport button icon size (1.0 = normal, 1.3 = 30% larger, 1.5 = 50% larger, etc.)
            float utility_icon_scale = 1.3f;  // Utility/view toggle button icon size
            ImVec4 disabled_color = ImVec4(0.29f, 0.29f, 0.29f, 1.0f);  // Color for disabled buttons (RGB: 0.0-1.0)

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));  // Transparent background
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));  // Subtle hover
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));  // Subtle active

            // Toggle Timecode
            if (font_icons) {
                ImGui::PushFont(font_icons);

                // Check timecode availability when needed
                if (timecode_state == NOT_CHECKED) {
                    CheckStartTimecodeAvailability();
                }

                // Only change button color when we're actually in timecode mode
                bool use_green_color = (timecode_mode_enabled && timecode_state == AVAILABLE);

                if (use_green_color) {
                    ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor()); // Accent color when active
                }

                // ALWAYS ENABLED - no more disabled state
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool clicked = ImGui::Button(ICON_TIMECODE, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);

                if (use_green_color) {
                    ImGui::PopStyleColor();
                }

                ImGui::PopFont();

                if (clicked) {
                    ToggleTimecodeMode();
                }

                // Simplified tooltip that shows the actual state
                if (ImGui::IsItemHovered()) {
                    const char* tooltip = "";
                    switch (timecode_state) {
                    case NOT_CHECKED:
                    case CHECKING:
                        tooltip = "Checking for embedded timecode...";
                        break;
                    case AVAILABLE:
                        tooltip = timecode_mode_enabled ?
                            "Timecode Mode ON - showing offset timecode" :
                            "Timecode Mode OFF - click to enable";
                        break;
                    case NOT_AVAILABLE:
                        tooltip = "Timecode Mode - no embedded timecode in this file";
                        break;
                    }
                    ImGui::SetTooltip("%s", tooltip);

                    // Show the actual timecode when available
                    if (timecode_state == AVAILABLE && !cached_start_timecode.empty()) {
                        ImGui::SetTooltip("%s\nStart Timecode: %s", tooltip, cached_start_timecode.c_str());
                    }
                }
            }
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();

            // === DRAW PLAYBACK CONTROLS BACKGROUND BOX ===
            // Calculate width: 15px dummy + Playlist Prev + 8 playback buttons + Playlist Next + spacings + 15px dummy
            int playback_button_count = 9; // Prev playlist, skip prev, RW, prev frame, play, next frame, FF, skip next, next playlist
            int playback_spacer_count = playback_button_count - 1;
            float playback_box_padding = 5.0f; // Padding on each side for rounded corners
            float playback_box_width = (playback_button_count * small_button) + (playback_spacer_count * item_spacing) + (playback_box_padding * 2 + 12);
            float playback_box_height = button_size;  // Just button height, no padding
            ImVec2 playback_box_start = ImGui::GetCursorScreenPos();

            // Draw background box with rounded corners
            transport_draw_list->AddRectFilled(
                playback_box_start,
                ImVec2(playback_box_start.x + playback_box_width, playback_box_start.y + playback_box_height),
                IM_COL32(16, 16, 16, 60),
                9.0f); // Rounded corners

            // Draw border (20% opacity = ~51/255) with rounded corners
            transport_draw_list->AddRect(
                playback_box_start,
                ImVec2(playback_box_start.x + playback_box_width, playback_box_start.y + playback_box_height),
                IM_COL32(255, 255, 255, 15),
                9.0f, 0, 1.0f); // Rounded corners

            // Add 15px dummy for padding before buttons
            ImGui::Dummy(ImVec2(playback_box_padding, 0));
            ImGui::SameLine();

            // Playlist Previous
            if (font_icons) {
                ImGui::PushFont(font_icons);
                bool should_disable = !project_manager->IsInSequenceMode() || project_manager->GetPlaylistLength() <= 1;
                if (should_disable) {
                    ImGui::PushStyleColor(ImGuiCol_Text, disabled_color);
                }
                ImGui::SetWindowFontScale(icon_scale);
                ImGui::BeginDisabled(should_disable);
                bool clicked = ImGui::Button(ICON_BACK, ImVec2(small_button, button_size));
                ImGui::EndDisabled();
                ImGui::SetWindowFontScale(1.0f);
                if (should_disable) {
                    ImGui::PopStyleColor();
                }
                ImGui::PopFont();
                if (clicked && !should_disable) {
                    project_manager->GoToPreviousInPlaylist();
                }
                if (ImGui::IsItemHovered()) {
                    if (should_disable) {
                        ImGui::SetTooltip("Previous media (playlist) - No playlist active");
                    }
                    else {
                        ImGui::SetTooltip("Previous media (playlist)");
                    }
                }
            }
            ImGui::SameLine();

            // Go to start
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(icon_scale);
                bool clicked = ImGui::Button(ICON_SKIP_PREVIOUS, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    video_player->GoToStart();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Go to beginning");
                }
            }
            ImGui::SameLine();

            // Rewind button
            static bool rw_active = false;
            bool rw_clicked = false;
            bool rw_held = false;

            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(icon_scale);
                rw_clicked = ImGui::Button(ICON_FAST_REWIND, ImVec2(small_button, button_size));
                rw_held = ImGui::IsMouseDown(0) && ImGui::IsItemHovered();
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click: Step back | Hold: Rewind");
                }
            }

            if (rw_held && !rw_active) {
                rw_active = true;
                video_player->StartRewind();
            }
            else if (!rw_held && rw_active) {
                rw_active = false;
                video_player->StopFastSeek();
            }
            else if (rw_clicked && !rw_active) {
                video_player->StepFrame(-1);
            }
            ImGui::SameLine();

            // Previous frame
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(icon_scale);
                bool clicked = ImGui::Button(ICON_ARROW_LEFT, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    video_player->StepFrame(-1);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Previous frame");
                }
            }
            ImGui::SameLine();

            // Play/Pause button - larger, centered
            bool is_playing = video_player->IsPlaying();
            const char* play_pause_icon = is_playing ? ICON_PAUSE : ICON_PLAY_ARROW;

            if (font_icons) {
                ImGui::PushFont(font_icons);

                // Highlight play button when playing (like timecode button)
                if (is_playing) {
                    ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor()); // Accent color when playing
                }

                ImGui::SetWindowFontScale(icon_scale);
                bool clicked = ImGui::Button(play_pause_icon, ImVec2(button_size, button_size));
                ImGui::SetWindowFontScale(1.0f);

                if (is_playing) {
                    ImGui::PopStyleColor();
                }

                ImGui::PopFont();

                if (clicked) {
                    if (is_playing) {
                        video_player->Pause();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(false);
                        }
                    }
                    else {
                        video_player->Play();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(true);
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(is_playing ? "Pause" : "Play");
                }
            }
            ImGui::SameLine();

            // Next frame
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(icon_scale);
                bool clicked = ImGui::Button(ICON_ARROW_RIGHT, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    video_player->StepFrame(1);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Next frame");
                }
            }
            ImGui::SameLine();

            // Fast forward
            static bool ff_active = false;
            bool ff_clicked = false;
            bool ff_held = false;

            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(icon_scale);
                ff_clicked = ImGui::Button(ICON_FAST_FORWARD, ImVec2(small_button, button_size));
                ff_held = ImGui::IsMouseDown(0) && ImGui::IsItemHovered();
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click: Step forward | Hold: Fast forward");
                }
            }

            if (ff_held && !ff_active) {
                ff_active = true;
                video_player->StartFastForward();
            }
            else if (!ff_held && ff_active) {
                ff_active = false;
                video_player->StopFastSeek();
            }
            else if (ff_clicked && !ff_active) {
                video_player->StepFrame(1);
            }
            ImGui::SameLine();

            // Go to end
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(icon_scale);
                bool clicked = ImGui::Button(ICON_SKIP_NEXT, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    video_player->GoToEnd();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Go to end");
                }
            }
            ImGui::SameLine();

            // Playlist Next
            if (font_icons) {
                ImGui::PushFont(font_icons);
                bool should_disable = !project_manager->IsInSequenceMode() || project_manager->GetPlaylistLength() <= 1;
                if (should_disable) {
                    ImGui::PushStyleColor(ImGuiCol_Text, disabled_color);
                }
                ImGui::SetWindowFontScale(icon_scale);
                ImGui::BeginDisabled(should_disable);
                bool clicked = ImGui::Button(ICON_FORWARD, ImVec2(small_button, button_size));
                ImGui::EndDisabled();
                ImGui::SetWindowFontScale(1.0f);
                if (should_disable) {
                    ImGui::PopStyleColor();
                }
                ImGui::PopFont();
                if (clicked && !should_disable) {
                    project_manager->GoToNextInPlaylist();
                }
                if (ImGui::IsItemHovered()) {
                    if (should_disable) {
                        ImGui::SetTooltip("Next media (playlist) - No playlist active");
                    }
                    else {
                        ImGui::SetTooltip("Next media (playlist)");
                    }
                }
            }

            // Add 15px dummy for padding after buttons
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(playback_box_padding, 0));

            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();

            // Toggle Colorspace
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool clicked = ImGui::Button(ICON_TONALITY, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    show_colorspace_panel = !show_colorspace_panel;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Toggle colorspace presets");
                }
            }
            ImGui::SameLine();

            // Safety Guides
            if (font_icons) {
                ImGui::PushFont(font_icons);

                // Highlight button if safety overlays are enabled
                bool overlays_enabled = video_player && video_player->IsSafetyOverlaysEnabled();
                if (overlays_enabled) {
                    ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor()); // Accent color when active
                }

                ImGui::SetWindowFontScale(utility_icon_scale);
                bool clicked = ImGui::Button(ICON_MASK, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);

                if (overlays_enabled) {
                    ImGui::PopStyleColor();
                }

                ImGui::PopFont();

                if (clicked) {
                    show_safety_overlay_panel = !show_safety_overlay_panel;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Toggle safety overlay guides");
                }
            }
            ImGui::SameLine();

            // Background
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool clicked = ImGui::Button(ICON_BACKGROUND, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    show_background_panel = !show_background_panel;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Change video panel background");
                }
            }
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();

            // Screenshot to clipboard
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool clicked = ImGui::Button(ICON_SCREENSHOT_CLIPBOARD, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    if (video_player && video_player->HasValidTexture()) {
                        video_player->CaptureScreenshotToClipboard();
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Screenshot to clipboard");
                }
            }

            ImGui::SameLine();

            // Screenshot to desktop
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool clicked = ImGui::Button(ICON_SCREENSHOT_DESKTOP, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    if (video_player && video_player->HasValidTexture()) {
                        video_player->CaptureScreenshotToDesktop();
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Screenshot to desktop");
                }
            }
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();

            // Fullscreen
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool clicked = ImGui::Button(ICON_FULLSCREEN, ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopFont();

                if (clicked) {
                    ToggleFullscreen();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Fullscreen");
                }
            }
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(10.0f, 0));
            ImGui::SameLine();

            // === VIEW TOGGLE BUTTONS ===
            // Inspector icon button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::PushStyleColor(ImGuiCol_Text, show_inspector_panel ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool inspector_clicked = ImGui::Button(ICON_ARTICLE "##inspector_view", ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (inspector_clicked) {
                    show_inspector_panel = !show_inspector_panel;
                    if (show_inspector_panel) minimal_view_mode = false;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(show_inspector_panel ? "Hide Inspector (Ctrl+2)" : "Show Inspector (Ctrl+2)");
                }
            }

            ImGui::SameLine();

            // Project icon button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::PushStyleColor(ImGuiCol_Text, show_project_panel ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool project_clicked = ImGui::Button(ICON_VIEW_TIMELINE "##project_view", ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (project_clicked) {
                    show_project_panel = !show_project_panel;
                    if (show_project_panel) minimal_view_mode = false;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(show_project_panel ? "Hide Project (Ctrl+1)" : "Show Project (Ctrl+1)");
                }
            }

            ImGui::SameLine();

            // Color icon button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::PushStyleColor(ImGuiCol_Text, show_color_panels ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool color_clicked = ImGui::Button(ICON_FLOWCHART "##color_view", ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (color_clicked) {
                    show_color_panels = !show_color_panels;
                    first_time_setup = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(show_color_panels ? "Hide Color (Ctrl+4)" : "Show Color (Ctrl+4)");
                }
            }

            ImGui::SameLine();

            // Annotations icon button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::PushStyleColor(ImGuiCol_Text, show_annotation_panel ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool annotations_clicked = ImGui::Button(ICON_NOTE_STACK "##annotations_view", ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (annotations_clicked) {
                    show_annotation_panel = !show_annotation_panel;
                    if (show_annotation_panel) minimal_view_mode = false;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(show_annotation_panel ? "Hide Annotations (Ctrl+5)" : "Show Annotations (Ctrl+5)");
                }
            }

            ImGui::SameLine();

            // All panels icon button
            if (font_icons) {
                bool all_panels_shown_local = show_project_panel && show_inspector_panel && show_color_panels && !minimal_view_mode;
                ImGui::PushFont(font_icons);
                ImGui::PushStyleColor(ImGuiCol_Text, all_panels_shown_local ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool all_clicked = ImGui::Button(ICON_GRID_VIEW "##all_panels_view", ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (all_clicked) {
                    minimal_view_mode = false;
                    ShowAllPanels();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Show All Panels (Ctrl+9)");
                }
            }

            ImGui::SameLine();

            // Minimal view icon button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::PushStyleColor(ImGuiCol_Text, minimal_view_mode ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::SetWindowFontScale(utility_icon_scale);
                bool minimal_clicked = ImGui::Button(ICON_HOME_MAX "##minimal_view", ImVec2(small_button, button_size));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (minimal_clicked) {
                    minimal_view_mode = !minimal_view_mode;
                    if (minimal_view_mode) {
                        saved_show_project_panel = show_project_panel;
                        saved_show_inspector_panel = show_inspector_panel;
                        saved_show_color_panels = show_color_panels;
                        saved_show_annotation_panel = show_annotation_panel;
                        saved_show_annotation_toolbar = show_annotation_toolbar;

                        show_project_panel = false;
                        show_inspector_panel = false;
                        show_timeline_panel = true;
                        show_color_panels = false;
                        show_annotation_panel = false;
                        show_annotation_toolbar = false;
                        if (annotation_toolbar) annotation_toolbar->SetVisible(false);

                        first_time_setup = true;
                    }
                    else {
                        show_project_panel = saved_show_project_panel;
                        show_inspector_panel = saved_show_inspector_panel;
                        show_timeline_panel = true;
                        show_color_panels = saved_show_color_panels;
                        show_annotation_panel = saved_show_annotation_panel;
                        show_annotation_toolbar = saved_show_annotation_toolbar;
                        if (annotation_toolbar) annotation_toolbar->SetVisible(saved_show_annotation_toolbar);

                        first_time_setup = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(minimal_view_mode ? "Exit Minimal View (Ctrl+-)" : "Minimal View (Ctrl+-)");
                }
            }

            // Pop transport row button styling
            ImGui::PopStyleColor(3);  // Pop Button, ButtonHovered, ButtonActive

            ImGui::Spacing();

            // === TIMELINE SECTION ===
            ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
            ImVec2 canvas_size = ImGui::GetContentRegionAvail();
            canvas_size.y = (canvas_size.y - 30.0f > 50.0f) ? (canvas_size.y - 30.0f) : 50.0f;

            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            draw_list->AddRectFilled(canvas_pos,
                ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                IM_COL32(16, 16, 16, 60));

            if (duration <= 0) {
                duration = 60.0;
            }
            double fps = video_player->GetFrameRate();
            int total_frames = video_player->GetTotalFrames();
            int current_frame = video_player->GetCurrentFrame();

            // Draw thin lines across top and bottom of timeline (matches frame ticker color state)
            ImU32 timeline_border_color = (duration > 0 && total_frames > 0)
                ? IM_COL32(160, 160, 160, 50)   // Match major tick color when media loaded
                : IM_COL32(100, 100, 100, 70);  // Match major tick color when no media

            // Top line
            draw_list->AddLine(
                ImVec2(canvas_pos.x, canvas_pos.y),
                ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y),
                timeline_border_color, 1.0f);

            // Bottom line
            draw_list->AddLine(
                ImVec2(canvas_pos.x, canvas_pos.y + canvas_size.y),
                ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                timeline_border_color, 1.0f);

            if (duration > 0 && total_frames > 0) {
                // Calculate frame markers
                int visible_frame_count = 20;
                int frame_step = (total_frames / visible_frame_count > 1) ? (total_frames / visible_frame_count) : 1;

                // Draw frame markers
                for (int i = 0; i <= visible_frame_count; ++i) {
                    int frame_num = i * frame_step;
                    if (frame_num > total_frames) frame_num = total_frames;
                    float x = canvas_pos.x + (canvas_size.x * frame_num / (float)total_frames);

                    bool is_major = (i % 5 == 0);
                    float tick_height = is_major ? 20.0f : 12.0f;
                    ImU32 tick_color = is_major ? IM_COL32(160, 160, 160, 255) : IM_COL32(120, 120, 120, 255);

                    draw_list->AddLine(ImVec2(x, canvas_pos.y),
                        ImVec2(x, canvas_pos.y + tick_height),
                        tick_color, is_major ? 1.5f : 1.0f);

                    if (is_major && frame_num > 0) {
                        char frame_label[16];
                        // Use file frame numbers for sequences, 1-based for regular videos
                        int display_frame_num;
                        if (video_player->IsInEXRMode() || video_player->IsImageSequence()) {
                            int start_frame = video_player->IsInEXRMode()
                                ? video_player->GetEXRSequenceStartFrame()
                                : video_player->GetImageSequenceStartFrame();
                            display_frame_num = ump::FrameIndexing::InternalToSequenceDisplay(frame_num, start_frame);
                        } else {
                            display_frame_num = ump::FrameIndexing::InternalToDisplay(frame_num);
                        }
                        snprintf(frame_label, sizeof(frame_label), "%d", display_frame_num);
                        draw_list->AddText(font_mono, 12.0f,
                            ImVec2(x - ImGui::CalcTextSize(frame_label).x * 0.5f, canvas_pos.y + 22),
                            IM_COL32(180, 180, 180, 255), frame_label);
                    }
                }

                // Draw progress fill (ONLY ONCE)
                float progress = (float)(position / duration);
                float progress_width = canvas_size.x * progress;
                if (progress_width > 0) {
                    draw_list->AddRectFilled(canvas_pos,
                        ImVec2(canvas_pos.x + progress_width, canvas_pos.y + canvas_size.y),
                        IM_COL32(80, 80, 80, 180));
                }

                // Draw fresh cache visualization bar
                if (timeline_manager && duration > 0) {
                    // Process completed frames from background extraction (no opportunistic caching)
                    if (project_manager && project_manager->GetCurrentVideoCache() && video_player) {
                        // TryCacheCurrentFrame now only processes completed frames, no new requests
                        project_manager->GetCurrentVideoCache()->TryCacheCurrentFrame(video_player.get());
                    }

                    // Get cache segments from timeline manager (throttled for performance)
                    static std::vector<FrameCache::CacheSegment> cached_segments;
                    static int cache_update_counter = 0;

                    // Update cache segments every 30 frames (~0.5 second at 60fps) for responsive feedback
                    if (++cache_update_counter >= 30) {
                        cached_segments = timeline_manager->GetCacheSegments();
                        cache_update_counter = 0;

                        // Debug: Log cache segments to see if we're getting data
                        static int last_segment_count = -1;
                        if (cached_segments.size() != last_segment_count) {
                            //Debug::Log("Timeline: Got " + std::to_string(cached_segments.size()) + " cache segments");
                            last_segment_count = cached_segments.size();
                        }
                    }

                    // Draw cache progress bar at bottom of timeline
                    const float cache_bar_height = 4.0f;
                    const float cache_bar_y = canvas_pos.y + canvas_size.y - cache_bar_height - 1.0f;

                    for (const auto& segment : cached_segments) {
                        // Convert timestamps to pixel positions
                        float start_x = canvas_pos.x + (float)(segment.start_time / duration) * canvas_size.x;
                        float end_x = canvas_pos.x + (float)(segment.end_time / duration) * canvas_size.x;

                        // Ensure minimum visibility
                        if (end_x - start_x < 2.0f) {
                            end_x = start_x + 2.0f;
                        }

                        // Clamp to timeline bounds
                        if (start_x > canvas_pos.x + canvas_size.x || end_x < canvas_pos.x) continue;
                        start_x = std::max(start_x, canvas_pos.x);
                        end_x = std::min(end_x, canvas_pos.x + canvas_size.x);

                        // Accent color for cached segments
                        ImU32 cache_color = ToImU32(GetWindowsAccentColor());

                        // Draw cache segment
                        draw_list->AddRectFilled(
                            ImVec2(start_x, cache_bar_y),
                            ImVec2(end_x, cache_bar_y + cache_bar_height),
                            cache_color
                        );
                    }
                }

                // Draw annotation markers (diamond shapes)
                if (annotation_manager && annotation_manager->HasNotes()) {
                    const auto& notes = annotation_manager->GetNotes();
                    ImU32 marker_color = ToImU32(GetWindowsAccentColor());

                    for (const auto& note : notes) {
                        // Calculate marker position
                        float marker_x = canvas_pos.x + (float)(note.timestamp_seconds / duration) * canvas_size.x;

                        // Diamond dimensions
                        float diamond_size = 8.0f;
                        float diamond_y = canvas_pos.y + canvas_size.y - 18.0f; // Position lower to avoid tickers

                        // Diamond points (center, top, right, bottom, left)
                        ImVec2 top(marker_x, diamond_y - diamond_size);
                        ImVec2 right(marker_x + diamond_size, diamond_y);
                        ImVec2 bottom(marker_x, diamond_y + diamond_size);
                        ImVec2 left(marker_x - diamond_size, diamond_y);

                        // Draw filled diamond
                        draw_list->AddQuadFilled(top, right, bottom, left, marker_color);
                    }
                }

                // Draw playhead (ONLY ONCE)
                float playhead_x = canvas_pos.x + progress_width;
                draw_list->AddLine(ImVec2(playhead_x, canvas_pos.y),
                    ImVec2(playhead_x, canvas_pos.y + canvas_size.y),
                    IM_COL32(255, 255, 255, 255), 2.0f);

                // Handle timeline interaction with smooth UI
                ImGui::SetCursorScreenPos(canvas_pos);
                ImGui::InvisibleButton("smooth_timeline", canvas_size);

                bool is_mouse_down = ImGui::IsItemActive();
                bool currently_scrubbing = timeline_manager->IsScrubbing();
                bool timeline_clicked = ImGui::IsItemClicked();
                bool marker_was_clicked = false;

                // NEW: Thumbnail tooltip on timeline hover (for all media with thumbnail cache)
                if (ImGui::IsItemHovered() && !is_mouse_down && cache_settings.enable_thumbnails &&
                    video_player->HasThumbnailCache()) {

                    // Throttle hover requests to avoid queuing every frame during fast drags
                    static int last_hover_frame = -1;
                    static int last_requested_frame = -1;
                    static auto last_request_time = std::chrono::steady_clock::now();

                    // Calculate frame at hover position
                    ImVec2 mouse_pos = ImGui::GetMousePos();
                    float relative_x = (mouse_pos.x - canvas_pos.x) / canvas_size.x;
                    if (relative_x >= 0.0f && relative_x <= 1.0f && duration > 0) {
                        double hover_time = relative_x * duration;
                        double fps = video_player->GetFrameRate();
                        if (fps > 0) {
                            int hover_frame = static_cast<int>(std::round(hover_time * fps));

                            // Time-based throttling: Only request new thumbnails every 100ms during fast hover
                            // This prevents queuing hundreds of frames when dragging across timeline
                            auto now = std::chrono::steady_clock::now();
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request_time).count();

                            // Try to get thumbnail with nearest-neighbor fallback enabled
                            // This shows the closest prefetched frame as a preview while the exact frame loads
                            GLuint thumbnail_texture = video_player->GetThumbnailForFrame(hover_frame, true);

                            // Update request timing for throttling
                            if (hover_frame != last_requested_frame && (elapsed >= 100 || last_requested_frame == -1)) {
                                last_requested_frame = hover_frame;
                                last_request_time = now;
                            }

                            // Update last hover frame for tooltip display (always)
                            last_hover_frame = hover_frame;

                            // ALWAYS show tooltip (even if thumbnail not ready yet)
                            ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(50, 50, 50, 255));
                            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(100, 100, 100, 51)); 
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

                            ImGui::BeginTooltip();

                            if (thumbnail_texture != 0) {
                                // Display thumbnail with fixed size
                                ImVec2 thumb_size(cache_settings.thumbnail_width, cache_settings.thumbnail_height);
                                ImGui::Image((void*)(intptr_t)thumbnail_texture, thumb_size);
                            } else {
                                // Show placeholder while loading
                                ImVec2 thumb_size(cache_settings.thumbnail_width, cache_settings.thumbnail_height);
                                ImVec2 cursor_pos = ImGui::GetCursorScreenPos();

                                // Draw dark placeholder rectangle
                                ImDrawList* tooltip_draw_list = ImGui::GetWindowDrawList();
                                tooltip_draw_list->AddRectFilled(
                                    cursor_pos,
                                    ImVec2(cursor_pos.x + thumb_size.x, cursor_pos.y + thumb_size.y),
                                    IM_COL32(40, 40, 40, 255)
                                );

                                // Draw loading text centered
                                const char* loading_text = "Loading...";
                                ImVec2 text_size = ImGui::CalcTextSize(loading_text);
                                ImVec2 text_pos(
                                    cursor_pos.x + (thumb_size.x - text_size.x) * 0.5f,
                                    cursor_pos.y + (thumb_size.y - text_size.y) * 0.5f
                                );
                                tooltip_draw_list->AddText(text_pos, IM_COL32(180, 180, 180, 255), loading_text);

                                // Advance cursor
                                ImGui::Dummy(thumb_size);
                            }

                            // Show frame number and timecode
                            ImGui::Separator();

                            // Display frame numbers correctly for different media types
                            int display_frame;
                            if (video_player->IsInEXRMode() || video_player->IsImageSequence()) {
                                // Image sequences: Show file frame numbers (e.g., 1001, 1002...)
                                int start_frame = video_player->IsInEXRMode()
                                    ? video_player->GetEXRSequenceStartFrame()
                                    : video_player->GetImageSequenceStartFrame();
                                display_frame = ump::FrameIndexing::InternalToSequenceDisplay(hover_frame, start_frame);
                            } else {
                                // Regular videos: Show 1-based frame numbers (Frame 1, 2, 3...)
                                display_frame = ump::FrameIndexing::InternalToDisplay(hover_frame);
                            }

                            std::string frame_info = "Frame " + std::to_string(display_frame);
                            std::string timecode = video_player->FormatTimecode(hover_time, fps);
                            ImGui::Text("%s - %s", frame_info.c_str(), timecode.c_str());

                            ImGui::EndTooltip();

                            // Pop custom tooltip styles
                            ImGui::PopStyleVar(2);  // WindowRounding, WindowBorderSize
                            ImGui::PopStyleColor(2);  // PopupBg, Border
                        }
                    }
                }

                // Check for annotation marker click FIRST (before scrubbing logic)
                if (timeline_clicked && annotation_manager && annotation_manager->HasNotes()) {
                    ImVec2 mouse_pos = ImGui::GetMousePos();
                    const auto& notes = annotation_manager->GetNotes();
                    float diamond_size = 8.0f;
                    float click_threshold = diamond_size + 4.0f; // Extra padding for easier clicking

                    for (const auto& note : notes) {
                        float marker_x = canvas_pos.x + (float)(note.timestamp_seconds / duration) * canvas_size.x;
                        float diamond_y = canvas_pos.y + canvas_size.y - 18.0f;

                        // Check if click is near this marker
                        float dx = mouse_pos.x - marker_x;
                        float dy = mouse_pos.y - diamond_y;
                        float distance = std::sqrt(dx * dx + dy * dy);

                        if (distance <= click_threshold) {
                            // Navigate to this annotation
                            video_player->Seek(note.timestamp_seconds);
                            if (annotation_panel) {
                                annotation_panel->SetSelectedNote(note.timecode);
                            }
                            Debug::Log("Clicked annotation marker at " + note.timecode);
                            marker_was_clicked = true;
                            break;
                        }
                    }
                }

                // Start scrubbing (only if didn't click a marker)
                if (!currently_scrubbing && is_mouse_down && !marker_was_clicked) {
                    timeline_manager->StartScrubbing(video_player.get());
                }

                // Update scrubbing position (UI updates immediately, MPV seeks are throttled)
                // Skip scrubbing if we clicked on a marker
                if (is_mouse_down && duration > 0 && !marker_was_clicked) {
                    ImVec2 mouse_pos = ImGui::GetMousePos();
                    float relative_x = (mouse_pos.x - canvas_pos.x) / canvas_size.x;
                    if (relative_x < 0.0f) relative_x = 0.0f;
                    if (relative_x > 1.0f) relative_x = 1.0f;

                    double seek_time = relative_x * duration;

                    // Snap to nearest frame boundary for precise cache alignment
                    if (video_player && video_player->GetFrameRate() > 0) {
                        double fps = video_player->GetFrameRate();
                        int target_frame = static_cast<int>(std::round(seek_time * fps));
                        seek_time = target_frame / fps; // Snap to exact frame timestamp
                    }

                    timeline_manager->UpdateScrubbing(seek_time);
                }

                // Stop scrubbing
                if (currently_scrubbing && !is_mouse_down) {
                    timeline_manager->StopScrubbing(video_player.get());
                }
            }
            else {
                // Render empty timeline with 96 frames as default
                const int default_frames = 96;
                const int visible_frame_count = 20;
                const int frame_step = 5; // Show every 5th frame (0, 5, 10, 15, etc.)

                // Draw frame markers for empty timeline
                for (int i = 0; i <= visible_frame_count; ++i) {
                    int frame_num = i * frame_step;
                    if (frame_num > default_frames) frame_num = default_frames;

                    float x = canvas_pos.x + (canvas_size.x * frame_num / (float)default_frames);

                    // Major tick every 5th marker (frames 0, 25, 50, 75)
                    bool is_major = (i % 5 == 0);
                    float tick_height = is_major ? 20.0f : 12.0f;
                    ImU32 tick_color = is_major ? IM_COL32(100, 100, 100, 255) : IM_COL32(70, 70, 70, 255);

                    draw_list->AddLine(
                        ImVec2(x, canvas_pos.y),
                        ImVec2(x, canvas_pos.y + tick_height),
                        tick_color, is_major ? 1.5f : 1.0f
                    );

                    // Frame number labels for major ticks
                    if (is_major) {
                        char frame_label[16];
                        snprintf(frame_label, sizeof(frame_label), "%d", ump::FrameIndexing::InternalToSequenceDisplay(frame_num, video_player->GetImageSequenceStartFrame()));
                        draw_list->AddText(font_mono, 12.0f,
                            ImVec2(x - ImGui::CalcTextSize(frame_label).x * 0.5f, canvas_pos.y + 22),
                            IM_COL32(120, 120, 120, 255), frame_label);
                    }
                }

                // Draw playhead at frame 0 for empty timeline
                float playhead_x = canvas_pos.x;
                draw_list->AddLine(
                    ImVec2(playhead_x, canvas_pos.y),
                    ImVec2(playhead_x, canvas_pos.y + canvas_size.y),
                    IM_COL32(150, 150, 150, 255), 2.0f
                );

                // Optional: Add text indicating no video loaded
                const char* empty_text = "";
                ImVec2 text_size = ImGui::CalcTextSize(empty_text);
                draw_list->AddText(
                    ImVec2(canvas_pos.x + (canvas_size.x - text_size.x) * 0.5f,
                        canvas_pos.y + (canvas_size.y - text_size.y) * 0.5f),
                    IM_COL32(100, 100, 100, 128),
                    empty_text
                );

                // Still create the invisible button for consistency
                ImGui::SetCursorScreenPos(canvas_pos);
                ImGui::InvisibleButton("empty_timeline", canvas_size);
            }

            ImGui::Spacing();

            // === VOLUME/LOOP TOOLBAR (BELOW TIMELINE) ===
            // Align all text to frame padding for consistent baseline with buttons/sliders
            ImGui::AlignTextToFramePadding();

            // Volume control on left
            if (font_mono) ImGui::PushFont(font_mono);
            ImGui::Text("Volume:");
            if (font_mono) ImGui::PopFont();
            ImGui::SameLine();
            // NEW: Mute button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                const char* volume_icon = is_muted ? ICON_VOLUME_MUTE : ICON_VOLUME_UP;
                ImVec4 button_color = is_muted ? MutedLight(GetWindowsAccentColor()) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, button_color);

                if (ImGui::Button(volume_icon, ImVec2(25.0f, 22.0f))) {
                    ToggleMute();
                }

                ImGui::PopStyleColor();
                ImGui::PopFont();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(is_muted ? "Unmute (M)" : "Mute (M)");
                }
            }

            ImGui::SameLine();

            // Volume slider - disabled when muted
            ImGui::SetNextItemWidth(120);
            ImGui::BeginDisabled(is_muted);  // Disable slider when muted
            if (ImGui::SliderInt("##volume_slider", &current_volume, 0, 100, "%d%%")) {
                if (!is_muted) {  // Only apply if not muted
                    video_player->SetVolume(current_volume);
                }
            }
            ImGui::EndDisabled();

            // Show muted indicator
            if (is_muted) {
                ImGui::SameLine();
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "MUTED");
                if (font_mono) ImGui::PopFont();
            }

            // Context-aware Loop toggle
            ImGui::SameLine();
            if (font_mono) ImGui::PushFont(font_mono);
            ImGui::Text("Loop:");
            if (font_mono) ImGui::PopFont();
            ImGui::SameLine();
            bool loop_enabled = video_player->IsLooping();
            bool is_playlist_mode = project_manager->IsInSequenceMode();

            // Determine tooltip text based on mode
            const char* loop_tooltip;
            const char* loop_status_text = nullptr;
            ImVec4 status_color;

            if (is_playlist_mode) {
                loop_tooltip = loop_enabled ? "Playlist Loop: ON" : "Playlist Loop: OFF";
                if (loop_enabled) {
                    loop_status_text = "PLAYLIST LOOP";
                    status_color = MutedLight(GetWindowsAccentColor());
                }
            }
            else {
                loop_tooltip = loop_enabled ? "Single File Loop: ON" : "Single File Loop: OFF";
                if (loop_enabled) {
                    loop_status_text = "SINGLE LOOP";
                    status_color = MutedLight(GetWindowsAccentColor());
                }
            }

            const char* loop_icon = loop_enabled ? ICON_LOOP : ICON_LOOP_OFF;
            ImVec4 button_color = loop_enabled ? MutedLight(GetWindowsAccentColor()) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::PushStyleColor(ImGuiCol_Text, button_color);

                if (ImGui::Button(loop_icon, ImVec2(25.0f, 22.0f))) {
                    ToggleLoop();
                }

                ImGui::PopStyleColor();
                ImGui::PopFont();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", loop_tooltip);
                }
            }

            // Show loop status indicator
            if (loop_status_text) {
                ImGui::SameLine();
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::TextColored(status_color, "%s", loop_status_text);
                if (font_mono) ImGui::PopFont();
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", loop_tooltip);
            }

            // === COMPACT FRAME COUNTER (RIGHT ALIGNED) ===
            if (video_player && video_player->HasVideo()) {
                ImGui::PushFont(font_mono);

                double position = video_player->GetPosition();
                double duration = video_player->GetDuration();
                int current_frame = video_player->GetCurrentFrame();
                int total_frames = video_player->GetTotalFrames();

                // Calculate display time and frame
                std::string frame_str;
                if (video_player->IsInEXRMode() || video_player->IsImageSequence()) {
                    // IMAGE SEQUENCE: Show file frame numbers (e.g., 1001, 1002, 1003...)
                    int start_frame = video_player->IsInEXRMode()
                        ? video_player->GetEXRSequenceStartFrame()
                        : video_player->GetImageSequenceStartFrame();

                    double frame_duration = 1.0 / video_player->GetFrameRate();
                    double display_position = current_frame * frame_duration;

                    // Check if timecode mode is enabled
                    std::string time_display;
                    if (timecode_mode_enabled && timecode_state == AVAILABLE) {
                        time_display = FormatCurrentTimecodeWithOffset(display_position);
                    } else {
                        int pos_min = (int)(display_position / 60);
                        int pos_sec = (int)fmod(display_position, 60.0);
                        int pos_ms = (int)((display_position - (int)display_position) * 1000);
                        char buffer[32];
                        snprintf(buffer, sizeof(buffer), "%02d:%02d.%03d", pos_min, pos_sec, pos_ms);
                        time_display = buffer;
                    }

                    int display_frame = ump::FrameIndexing::InternalToSequenceDisplay(current_frame, start_frame);
                    frame_str = time_display + " Frame " + std::to_string(display_frame);
                } else {
                    // REGULAR VIDEO: Show 1-based frame numbers (Frame 1, 2, 3...)
                    double frame_duration = 1.0 / video_player->GetFrameRate();
                    double display_position = current_frame * frame_duration;

                    // Check if timecode mode is enabled
                    std::string time_display;
                    if (timecode_mode_enabled && timecode_state == AVAILABLE) {
                        time_display = FormatCurrentTimecodeWithOffset(display_position);
                    } else {
                        int pos_min = (int)(display_position / 60);
                        int pos_sec = (int)fmod(display_position, 60.0);
                        int pos_ms = (int)((display_position - (int)display_position) * 1000);
                        char buffer[32];
                        snprintf(buffer, sizeof(buffer), "%02d:%02d.%03d", pos_min, pos_sec, pos_ms);
                        time_display = buffer;
                    }

                    // Use 1-based display for regular videos (Frame 1, not Frame 0)
                    int display_frame = ump::FrameIndexing::InternalToDisplay(current_frame);
                    frame_str = time_display + " Frame " + std::to_string(display_frame);
                }

                // Calculate position for right-aligned text
                ImVec2 text_size = ImGui::CalcTextSize(frame_str.c_str());
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowSize().x - text_size.x - 15);

                // Show in accent color when in timecode mode
                if (timecode_mode_enabled && timecode_state == AVAILABLE) {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", frame_str.c_str());
                } else {
                    ImGui::Text("%s", frame_str.c_str());
                }

                ImGui::PopFont();
            }

        }
        ImGui::End();
    }

    void CreateProjectPanel() {
        project_manager->CreateProjectPanel(&show_project_panel);
    }

    void CreateInspectorPanel() {
        if (!show_inspector_panel) return;

        if (ImGui::Begin("Inspector", &show_inspector_panel)) {
            if (project_manager->IsInSequenceMode()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                if (font_icons) {
                    ImGui::PushFont(font_icons);
                    ImGui::Text(ICON_ARTICLE);
                    ImGui::PopFont();
                    ImGui::SameLine();
                }
                ImGui::Text("Playlist Manager");
                ImGui::PopStyleColor();
                ImGui::Separator();

                auto seq = project_manager->GetCurrentSequence();
                if (seq) {
                    ImGui::Text("Playlist: %s", seq->name.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%d clips)", (int)seq->clips.size());

                    ImGui::Spacing();

                    // Style buttons with more padding (matching Project Manager toolbar)
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 8.0f));

                    // Always full-width buttons, stacked vertically
                    if (ImGui::Button("Clear All", ImVec2(-1, 0))) {
                        project_manager->ClearCurrentPlaylist();
                    }

                    if (!seq->clips.empty()) {
                        if (ImGui::Button("Remove Duplicates", ImVec2(-1, 0))) {
                            project_manager->RemoveDuplicatesFromPlaylist();
                        }
                    }

                    ImGui::PopStyleVar();  // Pop FramePadding

                    ImGui::Separator();

                    if (!seq->clips.empty()) {
                        int current_pos = project_manager->GetCurrentPlaylistIndex();
                        int total_clips = project_manager->GetPlaylistLength();

                        if (current_pos >= 0 && total_clips > 0) {
                            ImGui::PushStyleColor(ImGuiCol_Text, MutedLight(GetWindowsAccentColor()));
                            if (font_icons) {
                                ImGui::PushFont(font_icons);
                                ImGui::Text(ICON_PLAY_ARROW);
                                ImGui::PopFont();
                                ImGui::SameLine();
                            }
                            ImGui::Text("Media: %d of %d", current_pos + 1, total_clips);
                            ImGui::PopStyleColor();

                            std::string current_clip = project_manager->GetCurrentClipName();
                            if (!current_clip.empty()) {
                                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Current: %s", current_clip.c_str());
                            }
                        }

                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                        if (font_icons) {
                            ImGui::PushFont(font_icons);
                            ImGui::Text(ICON_WINDOWS);
                            ImGui::PopFont();
                            ImGui::SameLine();
                        }
                        ImGui::Text("Playlist Contents:");
                        ImGui::PopStyleColor();

                        ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "Drag videos here to add � Drag within to reorder");

                        auto sorted_clips = seq->GetAllClipsSorted();

                        float item_height = ImGui::GetTextLineHeightWithSpacing();
                        int clip_count = (int)sorted_clips.size();
                        float min_height = 60.0f;
                        float max_height = 500.0f;
                        float base_height = item_height * clip_count + 40.0f;
                        float clamped_min = (base_height > min_height) ? base_height : min_height;
                        float calculated_height = (clamped_min < max_height) ? clamped_min : max_height;

                        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 28, 28, 255));
                        ImGui::BeginChild("PlaylistContents", ImVec2(0, calculated_height), true,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                        if (!sorted_clips.empty()) {
                            for (int i = 0; i < (int)sorted_clips.size(); i++) {
                                const auto& clip = sorted_clips[i];

                                ImGui::PushID(("playlist_item_" + std::to_string(i)).c_str());

                                bool is_selected = project_manager->IsPlaylistItemSelected(i);
                                bool is_current = (i == project_manager->GetCurrentPlaylistIndex());

                                if (is_current) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, MutedLight(GetWindowsAccentColor()));
                                }

                                if (is_selected) {
                                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                                }

                                std::string display_name = std::to_string(i + 1) + ". " + clip.name;
                                if (is_current) {
                                    display_name += " [PLAYING]";
                                }

                                if (font_mono) ImGui::PushFont(font_mono);
                                bool clicked = ImGui::Selectable(display_name.c_str(), is_selected,
                                    ImGuiSelectableFlags_AllowDoubleClick);
                                if (font_mono) ImGui::PopFont();

                                if (is_selected) {
                                    ImGui::PopStyleColor(3);
                                }
                                if (is_current) {
                                    ImGui::PopStyleColor();
                                }

                                if (clicked) {
                                    bool ctrl_held = ImGui::GetIO().KeyCtrl;
                                    bool shift_held = ImGui::GetIO().KeyShift;

                                    // Single click - handle selection
                                    project_manager->SelectPlaylistItem(i, ctrl_held, shift_held);

                                    // Double-click - jump to this clip
                                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                                        Debug::Log("Double-click detected on playlist item: " + clip.name);
                                        project_manager->JumpToPlaylistIndex(i);
                                    }
                                }

                                // Right-click context menu
                                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                                    Debug::Log("Right-clicked playlist item: " + clip.name);

                                    std::string popup_id = "playlist_context_" + std::to_string(i);
                                    ImGui::OpenPopup(popup_id.c_str());

                                    if (!is_selected) {
                                        project_manager->ClearPlaylistSelection();
                                        project_manager->SelectPlaylistItem(i, false, false);
                                    }
                                }

                                // Context menu popup
                                std::string popup_id = "playlist_context_" + std::to_string(i);
                                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
                                if (ImGui::BeginPopup(popup_id.c_str())) {
                                    int selection_count = project_manager->GetSelectedPlaylistItemsCount();

                                    if (selection_count <= 1) {
                                        ImGui::Text("Options for: %s", clip.name.c_str());
                                    }
                                    else {
                                        ImGui::Text("%d clips selected", selection_count);
                                    }
                                    ImGui::Separator();

                                    // Delete option
                                    if (ImGui::MenuItem("Delete", "Del")) {
                                        project_manager->DeleteSelectedPlaylistItems();
                                        ImGui::CloseCurrentPopup();
                                    }

                                    // Move operations
                                    if (selection_count >= 1) {
                                        ImGui::Separator();
                                        if (ImGui::MenuItem("Move Up", "Ctrl+Up")) {
                                            project_manager->MoveSelectedPlaylistItemsUp();
                                            ImGui::CloseCurrentPopup();
                                        }
                                        if (ImGui::MenuItem("Move Down", "Ctrl+Down")) {
                                            project_manager->MoveSelectedPlaylistItemsDown();
                                            ImGui::CloseCurrentPopup();
                                        }
                                    }

                                    // Clear selection option (when multiple selected)
                                    if (selection_count > 1) {
                                        ImGui::Separator();
                                        if (ImGui::MenuItem("Clear Selection")) {
                                            project_manager->ClearPlaylistSelection();
                                            ImGui::CloseCurrentPopup();
                                        }
                                    }

                                    ImGui::EndPopup();
                                }
                                ImGui::PopStyleColor();

                                // Existing drag & drop functionality (preserve this)
                                if (ImGui::BeginDragDropSource()) {
                                    ImGui::SetDragDropPayload("PLAYLIST_ITEM", &i, sizeof(int));
                                    if (font_mono) ImGui::PushFont(font_mono);
                                    ImGui::Text("Moving: %s", clip.name.c_str());
                                    if (font_mono) ImGui::PopFont();
                                    ImGui::EndDragDropSource();
                                }

                                if (ImGui::BeginDragDropTarget()) {
                                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLAYLIST_ITEM")) {
                                        int source_index = *(const int*)payload->Data;
                                        if (source_index != i) {
                                            project_manager->MovePlaylistItem(source_index, i);
                                        }
                                    }
                                    ImGui::EndDragDropTarget();
                                }

                                ImGui::PopID();
                            }
                        }
                        else {
                            // Empty playlist state
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                            if (font_mono) ImGui::PushFont(font_mono);
                            ImGui::Text("Playlist is empty");
                            ImGui::Text("Drag videos from Project panel to add clips");
                            if (font_mono) ImGui::PopFont();
                            ImGui::PopStyleColor();
                        }

                        ImGui::Spacing();
                        ImGui::InvisibleButton("AppendDropZone", ImVec2(-1, 25));

                        // Visual feedback when dragging over
                        if (ImGui::IsItemHovered() && ImGui::GetDragDropPayload()) {
                            ImGui::GetWindowDrawList()->AddRectFilled(
                                ImGui::GetItemRectMin(),
                                ImGui::GetItemRectMax(),
                                IM_COL32(70, 130, 180, 80) // Subtle blue highlight
                            );

                            // Add text hint
                            ImVec2 text_pos = ImGui::GetItemRectMin();
                            text_pos.x += 10;
                            text_pos.y += 5;
                            ImGui::GetWindowDrawList()->AddText(
                                text_pos,
                                IM_COL32(200, 200, 200, 255),
                                "Drop here to append to playlist"
                            );
                        }

                        // Drop target for appending new items
                        if (ImGui::BeginDragDropTarget()) {
                            // Check if dragging image sequences and show warning tooltip
                            const ImGuiPayload* payload_peek = ImGui::GetDragDropPayload();
                            if (payload_peek && (payload_peek->IsDataType("MEDIA_ITEM") || payload_peek->IsDataType("MEDIA_ITEMS_MULTI"))) {
                                std::string payload_str(static_cast<const char*>(payload_peek->Data), payload_peek->DataSize - 1);

                                // Check if any items are image sequences (EXR or TIFF/PNG/JPEG)
                                bool contains_image_seq = false;
                                if (payload_peek->IsDataType("MEDIA_ITEM")) {
                                    auto* item = project_manager->GetMediaItem(payload_str);
                                    if (item && (item->type == ump::MediaType::EXR_SEQUENCE || item->type == ump::MediaType::IMAGE_SEQUENCE)) {
                                        contains_image_seq = true;
                                    }
                                } else {
                                    // Parse multi-select payload
                                    std::istringstream ss(payload_str);
                                    std::string media_id;
                                    while (std::getline(ss, media_id, ';')) {
                                        auto* item = project_manager->GetMediaItem(media_id);
                                        if (item && (item->type == ump::MediaType::EXR_SEQUENCE || item->type == ump::MediaType::IMAGE_SEQUENCE)) {
                                            contains_image_seq = true;
                                            break;
                                        }
                                    }
                                }

                                if (contains_image_seq) {
                                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 0.85f));
                                    ImGui::SetTooltip("Image sequences cannot be added to playlists");
                                    ImGui::PopStyleColor();
                                }
                            }

                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
                                std::string media_id(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                                project_manager->AddToPlaylist(media_id);
                                Debug::Log("Appended item to existing playlist: " + media_id);
                            }
                            else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEMS_MULTI")) {
                                std::string payload_str(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                                project_manager->AddMultipleToPlaylist(payload_str);
                                Debug::Log("Appended multiple items to existing playlist");
                            }
                            ImGui::EndDragDropTarget();
                        }

                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }
                    else {
                        // Empty playlist 
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                        if (font_icons) {
                            ImGui::PushFont(font_icons);
                            ImGui::Text(ICON_WINDOWS);
                            ImGui::PopFont();
                            ImGui::SameLine();
                        }
                        ImGui::Text("Playlist is empty");
                        ImGui::PopStyleColor();

                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Drag videos from Project panel to add clips");

                        // Empty playlist drop zone 
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 28, 28, 255));
                        ImGui::BeginChild("EmptyPlaylistDropZone", ImVec2(0, 60), true);
                        ImGui::PushStyleColor(ImGuiCol_Text, MutedLight(GetWindowsAccentColor()));
                        if (font_icons) {
                            ImGui::PushFont(font_icons);
                            ImGui::Text(ICON_FOLDER_OPEN);
                            ImGui::PopFont();
                            ImGui::SameLine();
                        }
                        if (font_mono) ImGui::PushFont(font_mono);
                        ImGui::Text("Drop videos here to build playlist");
                        if (font_mono) ImGui::PopFont();
                        ImGui::PopStyleColor();

                        if (ImGui::BeginDragDropTarget()) {
                            // Check if dragging image sequences and show warning tooltip
                            const ImGuiPayload* payload_peek = ImGui::GetDragDropPayload();
                            if (payload_peek && (payload_peek->IsDataType("MEDIA_ITEM") || payload_peek->IsDataType("MEDIA_ITEMS_MULTI"))) {
                                std::string payload_str(static_cast<const char*>(payload_peek->Data), payload_peek->DataSize - 1);

                                // Check if any items are image sequences (EXR or TIFF/PNG/JPEG)
                                bool contains_image_seq = false;
                                if (payload_peek->IsDataType("MEDIA_ITEM")) {
                                    auto* item = project_manager->GetMediaItem(payload_str);
                                    if (item && (item->type == ump::MediaType::EXR_SEQUENCE || item->type == ump::MediaType::IMAGE_SEQUENCE)) {
                                        contains_image_seq = true;
                                    }
                                } else {
                                    // Parse multi-select payload
                                    std::istringstream ss(payload_str);
                                    std::string media_id;
                                    while (std::getline(ss, media_id, ';')) {
                                        auto* item = project_manager->GetMediaItem(media_id);
                                        if (item && (item->type == ump::MediaType::EXR_SEQUENCE || item->type == ump::MediaType::IMAGE_SEQUENCE)) {
                                            contains_image_seq = true;
                                            break;
                                        }
                                    }
                                }

                                if (contains_image_seq) {
                                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 0.85f));
                                    ImGui::SetTooltip("Image sequences cannot be added to playlists");
                                    ImGui::PopStyleColor();
                                }
                            }

                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
                                std::string media_id(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                                project_manager->AddToPlaylist(media_id);
                            }
                            else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEMS_MULTI")) {
                                std::string payload_str(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                                project_manager->AddMultipleToPlaylist(payload_str);
                            }
                            ImGui::EndDragDropTarget();
                        }

                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::Separator();
                ImGui::Spacing();

                // === Properties Section ===
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                if (font_icons) {
                    ImGui::PushFont(font_icons);
                    ImGui::Text(ICON_MOVIE);
                    ImGui::PopFont();
                    ImGui::SameLine();
                }
                ImGui::Text("Properties");
                ImGui::PopStyleColor();
                ImGui::Separator();

                project_manager->CreatePropertiesSection();

            }
            else {
                // Non-sequence mode - just show properties
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                if (font_icons) {
                    ImGui::PushFont(font_icons);
                    ImGui::Text(ICON_MOVIE);
                    ImGui::PopFont();
                    ImGui::SameLine();
                }
                ImGui::Text("Video Properties");
                ImGui::PopStyleColor();
                ImGui::Separator();

                project_manager->CreatePropertiesSection();
            }
        }
        ImGui::End();
    }

    void CreateComponentPaletteContent() {
        // Create tab bar for Components vs Presets
        if (ImGui::BeginTabBar("PaletteTabBar", ImGuiTabBarFlags_None)) {

            // Current/Components Tab
            if (ImGui::BeginTabItem("Configs")) {
                CreateCurrentComponentsTab();
                ImGui::EndTabItem();
            }

            // Presets Tab
            if (ImGui::BeginTabItem("Presets")) {
                CreatePresetsTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    void CreateCurrentComponentsTab() {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_INPUT_SETTINGS);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        ImGui::Text("OCIO Configs");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (!ocio_manager) {
            ImGui::TextDisabled("OCIO manager not initialized");
            return;
        }

        // Config dropdown (keeping as is)
        const auto& available_configs = ocio_manager->GetAvailableConfigs();
        if (!available_configs.empty()) {
            std::string current_config_name = ocio_manager->GetActiveConfigName();

            if (ImGui::BeginCombo("Config", current_config_name.c_str())) {
                for (const auto& config_info : available_configs) {
                    bool is_selected = (config_info.name == current_config_name);
                    if (ImGui::Selectable(config_info.name.c_str(), is_selected)) {
                        ocio_manager->LoadConfiguration(config_info.type);

                        // Refresh current frame with new config
                        RefreshCurrentFrame();
                        Debug::Log("Switched OCIO config and refreshed frame");
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Separator();

        // Input colorspaces
        if (ImGui::TreeNode("Input Colorspaces")) {
            auto colorspaces = ocio_manager->GetInputColorSpaces();
            for (const auto& cs : colorspaces) {
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Selectable(cs.c_str());
                if (font_mono) ImGui::PopFont();

                // Check for double-click AFTER the Selectable
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (node_manager) {
                        node_manager->SetPendingNodeCreation(
                            ump::NodeType::INPUT_COLORSPACE, cs);
                        Debug::Log("Double-clicked to create: " + cs);
                        UpdateColorPipeline();
                    }
                }

                // Drag source - use SourceAllowNullID flag
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    struct DragPayload {
                        ump::NodeType type;
                        char name[256];
                    };

                    DragPayload payload;
                    payload.type = ump::NodeType::INPUT_COLORSPACE;
                    strncpy_s(payload.name, sizeof(payload.name), cs.c_str(), _TRUNCATE);

                    ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                    ImGui::Text("Creating: %s", cs.c_str());
                    ImGui::EndDragDropSource();
                    UpdateColorPipeline();
                }
            }
            ImGui::TreePop();
        }

        // Looks
        auto looks = ocio_manager->GetLooks();
        if (!looks.empty() && ImGui::TreeNode("Looks")) {
            for (const auto& look : looks) {
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Selectable(look.c_str());
                if (font_mono) ImGui::PopFont();

                // Check for double-click AFTER the Selectable
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (node_manager) {
                        node_manager->SetPendingNodeCreation(
                            ump::NodeType::LOOK, look);
                        Debug::Log("Double-clicked to create: " + look);
                        UpdateColorPipeline();
                    }
                }

                // Drag source
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    struct DragPayload {
                        ump::NodeType type;
                        char name[256];
                    };

                    DragPayload payload;
                    payload.type = ump::NodeType::LOOK;
                    strncpy_s(payload.name, sizeof(payload.name), look.c_str(), _TRUNCATE);

                    ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                    ImGui::Text("Creating: %s", look.c_str());
                    ImGui::EndDragDropSource();
                    UpdateColorPipeline();
                }
            }
            ImGui::TreePop();
        }

        // Output displays
        auto displays = ocio_manager->GetDisplays();
        if (!displays.empty() && ImGui::TreeNode("Output Displays")) {
            for (const auto& display : displays) {
                if (font_mono) ImGui::PushFont(font_mono);
                ImGui::Selectable(display.c_str());
                if (font_mono) ImGui::PopFont();

                // Check for double-click AFTER the Selectable
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (node_manager) {
                        node_manager->SetPendingNodeCreation(
                            ump::NodeType::OUTPUT_DISPLAY, display);
                        Debug::Log("Double-clicked to create: " + display);
                        UpdateColorPipeline();
                    }
                }

                // Drag source
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    struct DragPayload {
                        ump::NodeType type;
                        char name[256];
                    };

                    DragPayload payload;
                    payload.type = ump::NodeType::OUTPUT_DISPLAY;
                    strncpy_s(payload.name, sizeof(payload.name), display.c_str(), _TRUNCATE);

                    ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                    ImGui::Text("Creating: %s", display.c_str());
                    ImGui::EndDragDropSource();
                    UpdateColorPipeline();
                }
            }
            ImGui::TreePop();
        }
    }

    void CreatePresetsTab() {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_FLOWCHART);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        ImGui::Text("Color Presets");
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Standard Presets (Top priority - using Blender config)
        if (ImGui::TreeNode("Standard")) {
            CreateStandardPresets();
            ImGui::TreePop();
        }

        // ACES 1.3 Presets
        if (ImGui::TreeNode("ACES 1.3")) {
            CreateACESPresets();
            ImGui::TreePop();
        }

        // Blender Presets
        if (ImGui::TreeNode("Blender")) {
            CreateBlenderPresets();
            ImGui::TreePop();
        }

        // Custom Presets
        if (ImGui::TreeNode("Custom")) {
            CreateCustomPresets();
            ImGui::TreePop();
        }
    }

    void CreateACESPresets() {
        if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
            ImGui::TextDisabled("No ACES config loaded");
            return;
        }


        // ACES presets using alias resolver

        if (font_mono) ImGui::PushFont(font_mono);
        if (ImGui::Selectable("ACEScg -> sRGB")) {
            ApplyAliasPreset("lin_ap1", "srgb_display", "ACES 1.0 - SDR Video");
        }

        if (ImGui::Selectable("ACEScg -> Rec.709")) {
            ApplyAliasPreset("lin_ap1", "rec1886_rec709_display", "ACES 1.0 - SDR Video");
        }

        if (ImGui::Selectable("ACES2065-1 -> sRGB")) {
            ApplyAliasPreset("aces2065_1", "srgb_display", "ACES 1.0 - SDR Video");
        }

        if (ImGui::Selectable("ACES2065-1 -> Rec.709")) {
            ApplyAliasPreset("aces2065_1", "rec1886_rec709_display", "ACES 1.0 - SDR Video");
        }
        if (font_mono) ImGui::PopFont();
    }

    void CreateStandardPresets() {
        // Auto-switch to Blender config for Standard workflows
        if (ocio_manager && ocio_manager->GetActiveConfigType() != OCIOConfigType::BLENDER) {
            if (!ocio_manager->SwitchToConfig(OCIOConfigType::BLENDER)) {
                ImGui::TextDisabled("Blender config not available");
                ImGui::Text("Standard workflows require Blender OCIO config.");
                return;
            }
        }

        if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
            ImGui::TextDisabled("No config loaded");
            return;
        }

        // Single Standard preset: Rec.1886 -> sRGB Standard (labeled as "Rec.709 -> sRGB")
        if (font_mono) ImGui::PushFont(font_mono);
        if (ImGui::Selectable("Rec.709 -> sRGB")) {
            // Uses Rec.1886 input (gamma corrected) to sRGB Standard output
            ApplyPreset(R"({
                "name": "Rec.709 to sRGB Standard",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Rec.1886", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB", "view": "Standard", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }
        if (font_mono) ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Standard Rec.1886 to sRGB transform (labeled as Rec.709)");
        }
    }

    void CreateBlenderPresets() {
        if (font_mono) ImGui::PushFont(font_mono);

        if (ImGui::Selectable("Linear Rec.709 -> sRGB Standard")) {
            ApplyPreset(R"({
                "name": "Linear Rec.709 to sRGB Standard",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB", "view": "Standard", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }

        if (ImGui::Selectable("Linear Rec.709 -> rec.709 Standard")) {
            ApplyPreset(R"({
                "name": "Linear Rec.709 to Rec.709 Standard",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Rec.1886", "view": "Standard", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }

        if (ImGui::Selectable("Linear Rec.709 -> sRGB AgX")) {
            ApplyPreset(R"({
                "name": "Linear Rec.709 to sRGB AgX",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB", "view": "AgX", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }

        if (ImGui::Selectable("Linear Rec.709 -> rec.709 AgX")) {
            ApplyPreset(R"({
                "name": "Linear Rec.709 to Rec.709 AgX",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Rec.1886", "view": "AgX", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
          })");
        }

        if (font_mono) ImGui::PopFont();
    }

    std::string GetNodeTreesFolder() {
        const char* localappdata = std::getenv("LOCALAPPDATA");
        if (localappdata) {
            std::filesystem::path nodes_path = std::filesystem::path(localappdata) / "ump" / "nodes";

            // Create directory if it doesn't exist
            if (!std::filesystem::exists(nodes_path)) {
                std::filesystem::create_directories(nodes_path);
                Debug::Log("Created node trees folder: " + nodes_path.string());
            }

            return nodes_path.string();
        }

        // Fallback
        return "nodes/";
    }

    void SaveCurrentNodeTree(const std::string& name) {
        if (!node_manager) {
            Debug::Log("ERROR: No node manager available");
            return;
        }

        using json = nlohmann::json;

        try {
            // Build preset JSON in the same format as existing presets
            json preset;
            preset["name"] = name;

            // Serialize nodes
            json nodes_array = json::array();
            auto all_nodes = node_manager->GetAllNodes();

            Debug::Log("SaveNodeTree: Found " + std::to_string(all_nodes.size()) + " nodes");

            // Create a map from node ID to array index
            std::unordered_map<int, int> node_id_to_index;
            int index = 0;

            for (auto* node : all_nodes) {
                if (!node) continue;

                node_id_to_index[node->GetId()] = index++;

                json node_obj;
                ImVec2 pos = ImNodes::GetNodeGridSpacePos(node->GetId());
                node_obj["position"] = json::array({pos.x, pos.y});

                // Store type-specific data
                switch (node->GetType()) {
                    case ump::NodeType::INPUT_COLORSPACE: {
                        auto* cs_node = dynamic_cast<ump::InputColorSpaceNode*>(node);
                        if (cs_node) {
                            node_obj["type"] = "INPUT_COLORSPACE";
                            node_obj["data"] = cs_node->GetColorSpace();
                            Debug::Log("  - Node " + std::to_string(index-1) + ": INPUT " + cs_node->GetColorSpace());
                        }
                        break;
                    }
                    case ump::NodeType::LOOK: {
                        auto* look_node = dynamic_cast<ump::LookNode*>(node);
                        if (look_node) {
                            node_obj["type"] = "LOOK";
                            node_obj["data"] = look_node->GetLook();
                            Debug::Log("  - Node " + std::to_string(index-1) + ": LOOK " + look_node->GetLook());
                        }
                        break;
                    }
                    case ump::NodeType::OUTPUT_DISPLAY: {
                        auto* output_node = dynamic_cast<ump::OutputDisplayNode*>(node);
                        if (output_node) {
                            node_obj["type"] = "OUTPUT_DISPLAY";
                            node_obj["display"] = output_node->GetDisplay();
                            node_obj["view"] = output_node->GetView();
                            Debug::Log("  - Node " + std::to_string(index-1) + ": OUTPUT " + output_node->GetDisplay());
                        }
                        break;
                    }
                }

                nodes_array.push_back(node_obj);
            }
            preset["nodes"] = nodes_array;

            // Serialize connections using node indices
            json connections_array = json::array();
            auto connections = node_manager->GetConnections();

            Debug::Log("SaveNodeTree: Found " + std::to_string(connections.size()) + " connections");

            for (const auto& conn : connections) {
                // Find which nodes own these pins
                int from_node_idx = -1;
                int to_node_idx = -1;

                for (auto* node : all_nodes) {
                    if (!node) continue;

                    // Check if this node owns the from_pin (output pin)
                    if (from_node_idx == -1) {  // Only search if not found yet
                        for (const auto& pin : node->GetOutputPins()) {
                            if (pin.id == conn.from_pin) {
                                from_node_idx = node_id_to_index[node->GetId()];
                                break;
                            }
                        }
                    }

                    // Check if this node owns the to_pin (input pin)
                    if (to_node_idx == -1) {  // Only search if not found yet
                        for (const auto& pin : node->GetInputPins()) {
                            if (pin.id == conn.to_pin) {
                                to_node_idx = node_id_to_index[node->GetId()];
                                break;
                            }
                        }
                    }

                    // If we found both, no need to continue
                    if (from_node_idx >= 0 && to_node_idx >= 0) {
                        break;
                    }
                }

                if (from_node_idx >= 0 && to_node_idx >= 0) {
                    json conn_obj;
                    conn_obj["from_node"] = from_node_idx;
                    conn_obj["from_pin"] = 0;  // Always 0 for our simple nodes
                    conn_obj["to_node"] = to_node_idx;
                    conn_obj["to_pin"] = 0;  // Always 0 for our simple nodes
                    connections_array.push_back(conn_obj);
                    Debug::Log("  - Connection: " + std::to_string(from_node_idx) + " -> " + std::to_string(to_node_idx));
                } else {
                    Debug::Log("  - WARNING: Could not find nodes for connection (from_pin=" + std::to_string(conn.from_pin) + ", to_pin=" + std::to_string(conn.to_pin) + ")");
                }
            }
            preset["connections"] = connections_array;

            // Write to file
            std::string folder = GetNodeTreesFolder();
            std::string file_path = folder + "/" + name + ".umpnode";

            std::ofstream file(file_path);
            if (!file.is_open()) {
                Debug::Log("ERROR: Failed to save node tree: " + file_path);
                return;
            }

            file << preset.dump(2);
            file.close();

            Debug::Log("Saved node tree: " + file_path);

            // Reload custom presets list
            LoadCustomNodeTrees();

        } catch (const std::exception& e) {
            Debug::Log("ERROR saving node tree: " + std::string(e.what()));
        }
    }

    std::vector<std::string> custom_node_trees;  // List of custom preset names

    void LoadCustomNodeTrees() {
        custom_node_trees.clear();

        std::string folder = GetNodeTreesFolder();

        try {
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (entry.is_regular_file() && entry.path().extension() == ".umpnode") {
                    std::string name = entry.path().stem().string();
                    custom_node_trees.push_back(name);
                }
            }

            Debug::Log("Loaded " + std::to_string(custom_node_trees.size()) + " custom node trees");

        } catch (const std::exception& e) {
            Debug::Log("ERROR loading custom node trees: " + std::string(e.what()));
        }
    }

    void LoadNodeTreeFromFile(const std::string& name) {
        try {
            std::string folder = GetNodeTreesFolder();
            std::string file_path = folder + "/" + name + ".umpnode";

            std::ifstream file(file_path);
            if (!file.is_open()) {
                Debug::Log("ERROR: Failed to open node tree: " + file_path);
                return;
            }

            // Read the entire file as string
            std::string json_content((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
            file.close();

            // Use the existing ApplyPreset function which handles everything
            ApplyPreset(json_content);

            Debug::Log("Loaded custom node tree: " + name);

        } catch (const std::exception& e) {
            Debug::Log("ERROR loading node tree: " + std::string(e.what()));
        }
    }

    void DeleteNodeTree(const std::string& name) {
        try {
            std::string folder = GetNodeTreesFolder();
            std::string file_path = folder + "/" + name + ".umpnode";

            if (std::filesystem::exists(file_path)) {
                std::filesystem::remove(file_path);
                Debug::Log("Deleted node tree: " + name);

                // Reload list
                LoadCustomNodeTrees();
            }

        } catch (const std::exception& e) {
            Debug::Log("ERROR deleting node tree: " + std::string(e.what()));
        }
    }

    void CreateCustomPresets() {
        // Show custom presets
        if (font_mono) ImGui::PushFont(font_mono);
        for (const auto& tree_name : custom_node_trees) {
            ImGui::PushID(tree_name.c_str());

            if (ImGui::Selectable(tree_name.c_str())) {
                LoadNodeTreeFromFile(tree_name);
            }

            // Right-click to delete
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete")) {
                    DeleteNodeTree(tree_name);
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        if (font_mono) ImGui::PopFont();

        if (custom_node_trees.empty()) {
            ImGui::TextDisabled("No custom presets defined");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Save button with name input
        static char preset_name[256] = "My Preset";
        ImGui::InputText("Name", preset_name, sizeof(preset_name));

        if (ImGui::Button("Save Current as Preset", ImVec2(-1, 0))) {
            if (strlen(preset_name) > 0) {
                SaveCurrentNodeTree(preset_name);
            } else {
                Debug::Log("ERROR: Preset name cannot be empty");
            }
        }
    }

    void ApplyPreset(const std::string& json_preset) {
        Debug::Log("=== Applying Color Preset ===");

        // Switch to Blender config for JSON-based presets (typically Blender presets)
        if (!ocio_manager || !ocio_manager->SwitchToConfig(OCIOConfigType::BLENDER)) {
            Debug::Log("ERROR: Failed to switch to Blender config");
            return;
        }
        Debug::Log("Successfully switched to Blender config");

        // Parse JSON (simplified - in production would use proper JSON library)
        // For now, extract the key information manually

        // Clear existing nodes first
        if (node_manager) {
            Debug::Log("Clearing existing nodes...");
            auto all_nodes = node_manager->GetAllNodes();
            for (auto* node : all_nodes) {
                if (node) {
                    node_manager->DeleteNode(node->GetId());
                }
            }
        }

        // Parse JSON using nlohmann::json
        std::vector<NodePresetData> nodes;
        std::vector<ConnectionPresetData> connections;

        try {
            nlohmann::json j = nlohmann::json::parse(json_preset);
            Debug::Log("Parsing JSON preset with " + std::to_string(j["nodes"].size()) + " nodes");

            // Parse nodes
            for (const auto& node_json : j["nodes"]) {
                NodePresetData node_data;
                node_data.type = node_json["type"].get<std::string>();

                // Get position
                if (node_json.contains("position")) {
                    node_data.position.x = node_json["position"][0].get<float>();
                    node_data.position.y = node_json["position"][1].get<float>();
                }

                // Get data based on node type
                if (node_data.type == "INPUT_COLORSPACE" || node_data.type == "LOOK") {
                    node_data.data = node_json["data"].get<std::string>();
                }
                else if (node_data.type == "OUTPUT_DISPLAY") {
                    node_data.display = node_json["display"].get<std::string>();
                    node_data.view = node_json["view"].get<std::string>();
                }

                nodes.push_back(node_data);
                Debug::Log("Parsed " + node_data.type + " node: " +
                          (node_data.type == "OUTPUT_DISPLAY" ? node_data.display : node_data.data));
            }

            // Parse connections
            for (const auto& conn_json : j["connections"]) {
                ConnectionPresetData conn_data;
                conn_data.from_node = conn_json["from_node"].get<int>();
                conn_data.from_pin = conn_json["from_pin"].get<int>();
                conn_data.to_node = conn_json["to_node"].get<int>();
                conn_data.to_pin = conn_json["to_pin"].get<int>();
                connections.push_back(conn_data);
            }

        } catch (const std::exception& e) {
            Debug::Log("ERROR parsing JSON preset: " + std::string(e.what()));
            return;
        }

        // Create nodes with proper display/view setup
        std::vector<int> node_ids;
        for (const auto& node_data : nodes) {
            int node_id = -1;

            if (node_data.type == "INPUT_COLORSPACE") {
                node_id = node_manager->CreateInputColorSpaceNode(node_data.data, node_data.position);
                Debug::Log("Created Input ColorSpace node: " + node_data.data);
            }
            else if (node_data.type == "OUTPUT_DISPLAY") {
                // Create display node with proper display AND view setup
                node_id = node_manager->CreateOutputDisplayNode(node_data.display, node_data.position);

                // CRITICAL: Set both display and view properly
                auto* display_node = dynamic_cast<ump::OutputDisplayNode*>(node_manager->GetNode(node_id));
                if (display_node) {
                    display_node->SetDisplay(node_data.display);
                    display_node->SetView(node_data.view);
                    Debug::Log("Created Output Display node: " + node_data.display + " - " + node_data.view);
                }
            }
            else if (node_data.type == "LOOK") {
                node_id = node_manager->CreateLookNode(node_data.data, node_data.position);
                Debug::Log("Created Look node: " + node_data.data);
            }

            node_ids.push_back(node_id);
        }

        // Create connections using the new method
        for (const auto& conn_data : connections) {
            if (conn_data.from_node < (int)node_ids.size() && conn_data.to_node < (int)node_ids.size()) {
                int from_node_id = node_ids[conn_data.from_node];
                int to_node_id = node_ids[conn_data.to_node];

                // Use the new CreateConnection method
                node_manager->CreateConnection(from_node_id, conn_data.from_pin, to_node_id, conn_data.to_pin);
            }
        }

        // Auto-generate the pipeline
        Debug::Log("Auto-generating OCIO pipeline...");
        GenerateOCIOPipeline();

        Debug::Log("=== Preset Applied Successfully ===");
    }


    void ApplyAliasPreset(const std::string& input_alias, const std::string& display_alias, const std::string& view_name) {
        Debug::Log("=== ApplyAliasPreset ENTRY ===");
        Debug::Log("Input params: " + input_alias + ", " + display_alias + ", " + view_name);

        Debug::Log("Checking ocio_manager pointer...");
        if (!ocio_manager) {
            Debug::Log("ERROR: ocio_manager is nullptr!");
            return;
        }

        Debug::Log("Checking if config is loaded...");
        if (!ocio_manager->IsConfigLoaded()) {
            Debug::Log("ERROR: No OCIO config loaded");
            return;
        }

        Debug::Log("OCIO manager and config OK, switching to ACES config...");

        // Switch to ACES config for alias-based presets
        if (!ocio_manager->SwitchToConfig(OCIOConfigType::ACES_13)) {
            Debug::Log("ERROR: Failed to switch to ACES 1.3 config");
            return;
        }
        Debug::Log("Successfully switched to ACES 1.3 config");

        // Resolve aliases to full names
        Debug::Log("Resolving input alias: " + input_alias);
        std::string input_colorspace = ocio_manager->ResolveAlias(input_alias);
        Debug::Log("Input resolved to: " + input_colorspace);

        Debug::Log("Resolving display alias: " + display_alias);
        std::string display_name = ocio_manager->ResolveAlias(display_alias);
        Debug::Log("Display resolved to: " + display_name);

        Debug::Log("Resolved aliases:");
        Debug::Log("  Input: " + input_alias + " -> " + input_colorspace);
        Debug::Log("  Display: " + display_alias + " -> " + display_name);
        Debug::Log("  View: " + view_name);

        // Clear existing nodes first
        if (node_manager) {
            Debug::Log("Clearing existing nodes...");
            auto all_nodes = node_manager->GetAllNodes();
            for (auto* node : all_nodes) {
                if (node) {
                    node_manager->DeleteNode(node->GetId());
                }
            }
        }

        // Create nodes with resolved names
        std::vector<int> node_ids;

        // Create input colorspace node
        int input_node_id = node_manager->CreateInputColorSpaceNode(input_colorspace, ImVec2(100, 100));
        if (input_node_id == -1) {
            Debug::Log("ERROR: Failed to create input colorspace node for: " + input_colorspace);
            return;
        }
        node_ids.push_back(input_node_id);
        Debug::Log("Created Input ColorSpace node: " + input_colorspace);

        // Create display node with proper display AND view setup
        int display_node_id = node_manager->CreateOutputDisplayNode(display_name, ImVec2(400, 100));
        if (display_node_id == -1) {
            Debug::Log("ERROR: Failed to create display node for: " + display_name);
            return;
        }
        node_ids.push_back(display_node_id);

        // CRITICAL: Set both display and view properly
        auto* base_node = node_manager->GetNode(display_node_id);
        if (!base_node) {
            Debug::Log("ERROR: Failed to get display node with ID " + std::to_string(display_node_id));
            return;
        }

        auto* display_node = dynamic_cast<ump::OutputDisplayNode*>(base_node);
        if (!display_node) {
            Debug::Log("ERROR: Failed to cast to OutputDisplayNode");
            return;
        }

        Debug::Log("Setting display: " + display_name + " and view: " + view_name);
        display_node->SetDisplay(display_name);
        display_node->SetView(view_name);
        Debug::Log("Created Output Display node: " + display_name + " - " + view_name);

        // Connect the nodes
        if (node_ids.size() >= 2) {
            node_manager->CreateConnection(node_ids[0], 0, node_ids[1], 0);
            Debug::Log("Connected nodes");
        }

        // Auto-generate the pipeline
        Debug::Log("Auto-generating OCIO pipeline...");
        GenerateOCIOPipeline();

        Debug::Log("=== Alias Preset Applied Successfully ===");
    }

    struct NodePresetData {
        std::string type;
        std::string data;     // For INPUT_COLORSPACE
        std::string display;  // For OUTPUT_DISPLAY
        std::string view;     // For OUTPUT_DISPLAY
        ImVec2 position;
    };

    struct ConnectionPresetData {
        int from_node;
        int from_pin;
        int to_node;
        int to_pin;
    };

    void CreateNodeEditorContent() {

        struct DragPayload {
            ump::NodeType type;
            char name[256];
        };

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_FLOWCHART);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        ImGui::Text("Color Nodes");
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImNodes::BeginNodeEditor();


        // Check if editor is hovered and we're releasing a drag
        if (ImNodes::IsEditorHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Check if there's an active drag payload
            const ImGuiPayload* payload = ImGui::GetDragDropPayload();
            if (payload && payload->IsDataType("OCIO_NODE")) {
                const DragPayload* data = (const DragPayload*)payload->Data;

                // Create node at current mouse position
                if (node_manager) {
                    node_manager->SetPendingNodeCreation(data->type, data->name);
                    Debug::Log("Dropped in editor: " + std::string(data->name));
                }
            }
        }

        if (node_manager) {
            node_manager->HandlePendingNodeCreation();
            node_manager->RenderAllNodes();
        }

        ImNodes::EndNodeEditor();

        if (node_manager) {
            node_manager->HandleConnections();
            node_manager->UpdateSelection();
        }
    }

    // In main.cpp, replace the existing CreateNodePropertiesContent() function:

    void CreateNodePropertiesContent() {
        // Header section (non-scrollable)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_DISPLAY_SETTINGS);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        ImGui::Text("Color Paramaters");
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Main scrollable content area (includes footer)
        ImGui::BeginChild("ScrollableContent", ImVec2(0, 0), false);

        // Get selected nodes
        int num_selected = ImNodes::NumSelectedNodes();

        if (num_selected == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::Text("No node selected");
            ImGui::Spacing();
            ImGui::TextWrapped("Select a node in the editor to view and edit its properties.");
            ImGui::PopStyleColor();
        }
        else if (num_selected > 1) {
            ImGui::TextColored(MutedLight(GetWindowsAccentColor()),
                "Multiple nodes selected (%d)", num_selected);
            ImGui::TextWrapped("Select a single node to edit properties.");
        }
        else {
            // Single node selected
            int selected_id;
            ImNodes::GetSelectedNodes(&selected_id);

            auto* node = node_manager->GetNodeById(selected_id);
            if (node) {
                RenderNodeSpecificProperties(node);
            }
        }

        // Footer area (inside scrollable region)
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Check if output display node is selected
        bool output_node_selected = false;
        if (num_selected == 1) {
            int selected_id;
            ImNodes::GetSelectedNodes(&selected_id);
            auto* node = node_manager->GetNodeById(selected_id);
            output_node_selected = (node && node->GetType() == ump::NodeType::OUTPUT_DISPLAY);
        }

        // Only enable button if output node selected AND pipeline ready
        bool pipeline_ready = output_node_selected && CheckPipelineReadiness();

        if (!pipeline_ready) {
            ImGui::BeginDisabled();
        }

        if (pipeline_ready) {
            ImGui::PushStyleColor(ImGuiCol_Button, MutedDark(GetWindowsAccentColor()));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetWindowsAccentColor());
        }

        if (ImGui::Button("Generate Shader", ImVec2(-1, 30.0f))) {
            GenerateOCIOPipeline();
        }

        if (pipeline_ready) {
            ImGui::PopStyleColor(2);
        }

        // Add Remove OCIO button - always available
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, MutedDark(GetWindowsAccentColor()));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetWindowsAccentColor());
        if (ImGui::Button("Remove Shader", ImVec2(-1, 25.0f))) {
            Debug::Log("User requested OCIO pipeline removal");
            video_player->ClearColorPipeline();
        }
        ImGui::PopStyleColor(2);

        if (!pipeline_ready) {
            ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, MutedLight(GetWindowsAccentColor()));
            if (!output_node_selected) {
                ImGui::TextWrapped("Select Output Display node");
            }
            else {
                ImGui::TextWrapped("Connect Input to Output");
            }
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();

        ImGui::PopStyleVar(2);
    }

    void RenderNodeSpecificProperties(ump::NodeBase* node) {
        if (!node) return;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

        // Node type header
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", node->GetTitle().c_str());
        ImGui::Separator();
        ImGui::Spacing();

        switch (node->GetType()) {
        case ump::NodeType::OUTPUT_DISPLAY:
            RenderOutputDisplayProperties(node);
            break;

        case ump::NodeType::INPUT_COLORSPACE:
            RenderInputColorSpaceProperties(node);
            break;

        case ump::NodeType::LOOK:
            RenderLookProperties(node);
            break;

        default:
            ImGui::TextDisabled("No editable properties");
            break;
        }

        ImGui::PopStyleVar();
    }

    void RenderOutputDisplayProperties(ump::NodeBase* node) {
        auto* display_node = dynamic_cast<ump::OutputDisplayNode*>(node);
        if (!display_node) return;

        ImGui::Text("Display Settings");
        ImGui::Separator();
        ImGui::Spacing();

        std::string current_display = display_node->GetDisplay();
        std::string current_view = display_node->GetView();

        // Show current display
        ImGui::Text("Display:");
        ImGui::SameLine();
        ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", current_display.c_str());

        // View selection with radio buttons
        ImGui::Spacing();
        ImGui::Text("Select View:");
        ImGui::Separator();

        // Get available views from OCIO config
        if (ocio_manager && ocio_manager->IsConfigLoaded()) {
            auto config = ocio_manager->GetConfig();

            // Get views for this display
            int num_views = config->getNumViews(current_display.c_str());

            if (num_views > 0) {
                ImGui::Indent();

                for (int i = 0; i < num_views; ++i) {
                    const char* view_name = config->getView(current_display.c_str(), i);
                    bool is_selected = (std::string(view_name) == current_view);

                    if (ImGui::RadioButton(view_name, is_selected)) {
                        display_node->SetView(view_name);
                        Debug::Log("View changed to: " + std::string(view_name));
                    }

                    // Add description if it's a special view
                    if (std::string(view_name) == "AgX") {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(Filmic tone mapping)");
                    }
                    else if (std::string(view_name) == "Standard") {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(Default)");
                    }
                    else if (std::string(view_name) == "False Color") {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(Exposure visualization)");
                    }
                }

                ImGui::Unindent();
            }
            else {
                ImGui::TextColored(MutedLight(GetWindowsAccentColor()),
                    "No views found for display '%s'", current_display.c_str());
            }
        }
        else {
            ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "No OCIO config loaded");
        }

        // Show current selection
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(Bright(GetWindowsAccentColor()),
            "Current: %s -> %s", current_display.c_str(), current_view.c_str());
    }

    void RenderInputColorSpaceProperties(ump::NodeBase* node) {
        auto* cs_node = dynamic_cast<ump::InputColorSpaceNode*>(node);
        if (!cs_node) return;

        ImGui::Text("Input ColorSpace");
        ImGui::Spacing();

        std::string current_cs = cs_node->GetColorSpace();

        // Optional: Allow changing colorspace
        if (ImGui::BeginCombo("ColorSpace", current_cs.c_str())) {
            if (ocio_manager) {
                auto colorspaces = ocio_manager->GetInputColorSpaces();
                for (const auto& cs : colorspaces) {
                    bool is_selected = (cs == current_cs);
                    if (ImGui::Selectable(cs.c_str(), is_selected)) {
                        cs_node->SetColorSpace(cs);
                    }
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Type: Input Source");
    }

    void RenderLookProperties(ump::NodeBase* node) {
        auto* look_node = dynamic_cast<ump::LookNode*>(node);
        if (!look_node) return;

        ImGui::Text("Look Settings");
        ImGui::Spacing();

        std::string current_look = look_node->GetLook();
        ImGui::Text("Look: %s", current_look.c_str());

        // Future: Add intensity slider, bypass checkbox, etc.
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Type: Creative Look/LUT");
    }

    bool CheckPipelineReadiness() {
        auto nodes = node_manager->GetAllNodes();

        bool has_input = false;
        bool has_valid_output = false;

        for (auto* node : nodes) {
            if (node->GetType() == ump::NodeType::INPUT_COLORSPACE) {
                has_input = true;
            }
            else if (node->GetType() == ump::NodeType::OUTPUT_DISPLAY) {
                auto* display_node = dynamic_cast<ump::OutputDisplayNode*>(node);
                // Check that view is set and not empty
                if (display_node && !display_node->GetView().empty()) {
                    has_valid_output = true;
                }
            }
        }

        return has_input && has_valid_output;
    }

    void GenerateOCIOPipeline() {
        Debug::Log("=== Pipeline Generation (Using Connected Chain) ===");

        // Get the connected pipeline in proper order
        auto pipeline_nodes = node_manager->GetPipelineOrder();
        if (pipeline_nodes.empty()) {
            Debug::Log("No connected pipeline found");
            return;
        }

        Debug::Log("Found connected pipeline with " + std::to_string(pipeline_nodes.size()) + " nodes");

        std::string src_colorspace;
        std::string display;
        std::string view;
        std::vector<std::string> look_chain; // Proper chain of looks, not concatenated string

        // Process nodes in connection order
        for (size_t i = 0; i < pipeline_nodes.size(); ++i) {
            auto* node = pipeline_nodes[i];
            Debug::Log("Processing node " + std::to_string(i) + ": " + std::to_string((int)node->GetType()));

            switch (node->GetType()) {
            case ump::NodeType::INPUT_COLORSPACE: {
                auto* csNode = dynamic_cast<ump::InputColorSpaceNode*>(node);
                if (csNode) {
                    src_colorspace = csNode->GetColorSpace();
                    Debug::Log("  Input ColorSpace: " + src_colorspace);
                }
                break;
            }

            case ump::NodeType::LOOK: {
                auto* lookNode = dynamic_cast<ump::LookNode*>(node);
                if (lookNode && !lookNode->GetLook().empty()) {
                    look_chain.push_back(lookNode->GetLook());
                    Debug::Log("  Look #" + std::to_string(look_chain.size()) + ": " + lookNode->GetLook());
                }
                break;
            }

            case ump::NodeType::OUTPUT_DISPLAY: {
                auto* displayNode = dynamic_cast<ump::OutputDisplayNode*>(node);
                if (displayNode) {
                    display = displayNode->GetDisplay();
                    view = displayNode->GetView();
                    Debug::Log("  Output: " + display + " - " + view);
                }
                break;
            }
            }
        }

        // Convert look chain to comma-separated string for current OCIO implementation
        std::string looks;
        for (size_t i = 0; i < look_chain.size(); ++i) {
            if (i > 0) looks += ", ";
            looks += look_chain[i];
        }

        // Validate and build pipeline
        if (!src_colorspace.empty() && !display.empty() && !view.empty()) {
            Debug::Log("Building OCIO pipeline...");
            Debug::Log("  Source: " + src_colorspace);
            Debug::Log("  Display: " + display);
            Debug::Log("  View: " + view);
            if (!looks.empty()) {
                Debug::Log("  Looks: " + looks);
            }

            auto ocio_pipeline = std::make_unique<OCIOPipeline>();
            if (ocio_pipeline->BuildFromDescription(src_colorspace, display, view, looks)) {
                video_player->SetColorPipeline(std::move(ocio_pipeline));
                Debug::Log("Pipeline generated successfully!");
            }
            else {
                Debug::Log("Pipeline generation failed");
            }
        }
        else {
            Debug::Log("Pipeline incomplete:");
            if (src_colorspace.empty()) Debug::Log("  - Missing input colorspace");
            if (display.empty()) Debug::Log("  - Missing output display");
            if (view.empty()) Debug::Log("  - Missing output view");
        }

    }

    void CreateAnnotationPanel() {
        if (!annotation_panel) return;

        // Get system accent colors
        ImVec4 accent_regular = GetWindowsAccentColor();
        ImVec4 accent_muted_dark = MutedDark(GetWindowsAccentColor());

        annotation_panel->Render(&show_annotation_panel, accent_regular, accent_muted_dark);
    }


    void CreateAnnotationToolbar() {
        if (!annotation_toolbar || !viewport_annotator) return;

        // Only show toolbar in annotation mode
        if (!viewport_annotator->IsAnnotationMode()) {
            annotation_toolbar->SetVisible(false);
            return;
        }

        // Set up toolbar callbacks
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
            annotation_toolbar->SetVisible(false);
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
            annotation_toolbar->SetVisible(false);
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

        // Render the toolbar
        bool can_undo = !annotation_undo_stack_.empty();
        bool can_redo = !annotation_redo_stack_.empty();

        // Get system accent colors
        ImVec4 accent_regular = GetWindowsAccentColor();
        ImVec4 accent_muted_dark = MutedDark(GetWindowsAccentColor());

        annotation_toolbar->Render(viewport_annotator.get(), can_undo, can_redo,
                                   font_icons, accent_regular, accent_muted_dark);
    }

    void CreateColorPanels() {
        if (!show_color_panels) return;

        // Load custom node trees on first open
        static bool custom_trees_loaded = false;
        if (!custom_trees_loaded) {
            LoadCustomNodeTrees();
            custom_trees_loaded = true;
        }

        if (!ImGui::Begin("Color", &show_color_panels)) {
            ImGui::End();
            return;
        }

        static float left_panel_width = 350.0f;
        static float right_panel_width = 400.0f;

        ImVec2 avail = ImGui::GetContentRegionAvail();

        // Left panel
        ImGui::BeginChild("ComponentPalette", ImVec2(left_panel_width, 0), true);
        CreateComponentPaletteContent();
        ImGui::EndChild();

        ImGui::SameLine();

        // Splitter between left and middle
        ImGui::InvisibleButton("##vsplitter1", ImVec2(4.0f, -1));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);  // East-West resize cursor
        }
        if (ImGui::IsItemActive()) {
            left_panel_width += ImGui::GetIO().MouseDelta.x;
        }

        ImGui::SameLine();

        // Middle panel (node editor)
        float middle_width = avail.x - left_panel_width - right_panel_width - 24.0f;
        ImGui::BeginChild("NodeEditor", ImVec2(middle_width, 0), true);
        CreateNodeEditorContent();
        ImGui::EndChild();

        ImGui::SameLine();

        // Splitter between middle and right
        ImGui::InvisibleButton("##vsplitter2", ImVec2(4.0f, -1));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);  // East-West resize cursor
        }
        if (ImGui::IsItemActive()) {
            right_panel_width -= ImGui::GetIO().MouseDelta.x;
        }


        ImGui::SameLine();

        // Right panel
        ImGui::BeginChild("NodeProperties", ImVec2(right_panel_width, 0), true);
        CreateNodePropertiesContent();
        ImGui::EndChild();

        ImGui::End();
    }

    // ------------------------------------------------------------------------
    // RENDERING METHODS
    // ------------------------------------------------------------------------

    // Helper to draw a line on an image buffer (Bresenham's algorithm)
    void DrawLineOnImage(unsigned char* image, int width, int height,
                         int x0, int y0, int x1, int y1,
                         unsigned char r, unsigned char g, unsigned char b, unsigned char a, int thickness = 2) {
        auto setPixel = [&](int x, int y) {
            if (x >= 0 && x < width && y >= 0 && y < height) {
                int idx = (y * width + x) * 4;
                // Alpha blending
                float alpha = a / 255.0f;
                image[idx + 0] = (unsigned char)(r * alpha + image[idx + 0] * (1 - alpha));
                image[idx + 1] = (unsigned char)(g * alpha + image[idx + 1] * (1 - alpha));
                image[idx + 2] = (unsigned char)(b * alpha + image[idx + 2] * (1 - alpha));
            }
        };

        // Draw thick line by drawing multiple parallel lines
        for (int t = -thickness/2; t <= thickness/2; t++) {
            int dx = abs(x1 - x0);
            int dy = abs(y1 - y0);
            int sx = x0 < x1 ? 1 : -1;
            int sy = y0 < y1 ? 1 : -1;
            int err = dx - dy;

            int x = x0;
            int y = y0;

            while (true) {
                if (dx > dy) {
                    setPixel(x, y + t);
                } else {
                    setPixel(x + t, y);
                }

                if (x == x1 && y == y1) break;

                int e2 = 2 * err;
                if (e2 > -dy) {
                    err -= dy;
                    x += sx;
                }
                if (e2 < dx) {
                    err += dx;
                    y += sy;
                }
            }
        }
    }

    // Helper to draw a rectangle on an image buffer
    void DrawRectangleOnImage(unsigned char* image, int width, int height,
                              int x0, int y0, int x1, int y1,
                              unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                              int thickness = 2, bool filled = false) {
        if (filled) {
            // Draw filled rectangle
            int minX = std::min(x0, x1);
            int maxX = std::max(x0, x1);
            int minY = std::min(y0, y1);
            int maxY = std::max(y0, y1);

            for (int y = minY; y <= maxY; y++) {
                for (int x = minX; x <= maxX; x++) {
                    if (x >= 0 && x < width && y >= 0 && y < height) {
                        int idx = (y * width + x) * 4;
                        float alpha = a / 255.0f;
                        image[idx + 0] = (unsigned char)(r * alpha + image[idx + 0] * (1 - alpha));
                        image[idx + 1] = (unsigned char)(g * alpha + image[idx + 1] * (1 - alpha));
                        image[idx + 2] = (unsigned char)(b * alpha + image[idx + 2] * (1 - alpha));
                    }
                }
            }
        } else {
            // Draw rectangle outline
            DrawLineOnImage(image, width, height, x0, y0, x1, y0, r, g, b, a, thickness); // Top
            DrawLineOnImage(image, width, height, x1, y0, x1, y1, r, g, b, a, thickness); // Right
            DrawLineOnImage(image, width, height, x1, y1, x0, y1, r, g, b, a, thickness); // Bottom
            DrawLineOnImage(image, width, height, x0, y1, x0, y0, r, g, b, a, thickness); // Left
        }
    }

    // Helper to draw an oval/ellipse on an image buffer (midpoint algorithm)
    void DrawOvalOnImage(unsigned char* image, int width, int height,
                         int x0, int y0, int x1, int y1,
                         unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                         int thickness = 2, bool filled = false) {
        auto setPixel = [&](int x, int y) {
            if (x >= 0 && x < width && y >= 0 && y < height) {
                int idx = (y * width + x) * 4;
                float alpha = a / 255.0f;
                image[idx + 0] = (unsigned char)(r * alpha + image[idx + 0] * (1 - alpha));
                image[idx + 1] = (unsigned char)(g * alpha + image[idx + 1] * (1 - alpha));
                image[idx + 2] = (unsigned char)(b * alpha + image[idx + 2] * (1 - alpha));
            }
        };

        int cx = (x0 + x1) / 2;
        int cy = (y0 + y1) / 2;
        int rx = abs(x1 - x0) / 2;
        int ry = abs(y1 - y0) / 2;

        if (rx == 0 || ry == 0) return;

        // Midpoint ellipse algorithm
        auto drawEllipsePoints = [&](int x, int y) {
            for (int t = -thickness/2; t <= thickness/2; t++) {
                setPixel(cx + x, cy + y + t);
                setPixel(cx - x, cy + y + t);
                setPixel(cx + x, cy - y + t);
                setPixel(cx - x, cy - y + t);
            }
        };

        if (filled) {
            // Draw filled ellipse by drawing horizontal lines
            for (int y = -ry; y <= ry; y++) {
                int x = (int)(rx * sqrt(1.0 - (double)(y * y) / (ry * ry)));
                for (int xi = -x; xi <= x; xi++) {
                    setPixel(cx + xi, cy + y);
                }
            }
        } else {
            // Draw ellipse outline
            int x = 0;
            int y = ry;

            // Region 1
            int rx2 = rx * rx;
            int ry2 = ry * ry;
            int p1 = ry2 - (rx2 * ry) + (0.25 * rx2);
            int dx = 2 * ry2 * x;
            int dy = 2 * rx2 * y;

            while (dx < dy) {
                drawEllipsePoints(x, y);
                x++;
                dx += 2 * ry2;
                if (p1 < 0) {
                    p1 += dx + ry2;
                } else {
                    y--;
                    dy -= 2 * rx2;
                    p1 += dx - dy + ry2;
                }
            }

            // Region 2
            int p2 = ry2 * (x + 0.5) * (x + 0.5) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
            while (y >= 0) {
                drawEllipsePoints(x, y);
                y--;
                dy -= 2 * rx2;
                if (p2 > 0) {
                    p2 += rx2 - dy;
                } else {
                    x++;
                    dx += 2 * ry2;
                    p2 += dx - dy + rx2;
                }
            }
        }
    }

    // Helper to draw an arrow on an image buffer
    void DrawArrowOnImage(unsigned char* image, int width, int height,
                          int x0, int y0, int x1, int y1,
                          unsigned char r, unsigned char g, unsigned char b, unsigned char a, int thickness = 2) {
        // Draw the main line
        DrawLineOnImage(image, width, height, x0, y0, x1, y1, r, g, b, a, thickness);

        // Calculate arrowhead
        double angle = atan2(y1 - y0, x1 - x0);
        double arrowLength = 15.0 + thickness * 2;
        double arrowAngle = 0.4; // ~23 degrees

        int ax1 = x1 - (int)(arrowLength * cos(angle - arrowAngle));
        int ay1 = y1 - (int)(arrowLength * sin(angle - arrowAngle));
        int ax2 = x1 - (int)(arrowLength * cos(angle + arrowAngle));
        int ay2 = y1 - (int)(arrowLength * sin(angle + arrowAngle));

        // Draw arrowhead lines
        DrawLineOnImage(image, width, height, x1, y1, ax1, ay1, r, g, b, a, thickness);
        DrawLineOnImage(image, width, height, x1, y1, ax2, ay2, r, g, b, a, thickness);
    }

    // Capture request structure
    struct CaptureRequest {
        std::string output_path;
        std::vector<ump::Annotations::ActiveStroke> strokes;
        bool pending = false;
        bool completed = false;
        bool success = false;
        bool just_queued = false;  // True for one frame to delay capture

        // Video display area for cropping (when capturing from screen)
        ImVec2 display_pos = ImVec2(0, 0);
        ImVec2 display_size = ImVec2(0, 0);

        // Export dimensions (when exporting at native resolution)
        bool use_native_resolution = false;
        int export_width = 0;
        int export_height = 0;

        // Viewport width where annotations were created (for line width scaling)
        float viewport_width_at_creation = 1920.0f;  // Default to HD if not set
    };

    CaptureRequest pending_capture;

    // Export state machine
    struct ExportState {
        bool active = false;
        ump::Annotations::AnnotationExporter::ExportFormat format;
        ump::Annotations::AnnotationExporter::ExportOptions options;
        std::vector<ump::AnnotationNote> notes;
        size_t current_note_index = 0;
        std::string temp_dir;
        std::vector<std::string> captured_images;
        bool waiting_for_capture = false;
        bool waiting_for_seek = false;  // True when we need to seek before capturing
        int frames_to_wait_after_seek = 0;  // Wait N frames for seek to complete
        int frames_to_wait_for_resize = 0;  // Wait N frames for window resize

        // Data for pending capture (set during seek phase, used during capture phase)
        std::string pending_output_path;
        std::vector<ump::Annotations::ActiveStroke> pending_strokes;
    };

    ExportState export_state;

    // ============================================================================
    // BASE64 HELPERS (Frame.io Token Obfuscation)
    // ============================================================================

    std::string EncodeBase64(const std::string& input) {
        if (input.empty()) return "";

        // Calculate required buffer size
        size_t output_size = AV_BASE64_SIZE(input.size());
        std::vector<char> output(output_size);

        // Encode using FFmpeg's base64 implementation
        char* result = av_base64_encode(output.data(), output_size,
                                       reinterpret_cast<const uint8_t*>(input.data()),
                                       input.size());

        if (result) {
            return std::string(result);
        }
        return "";
    }

    std::string DecodeBase64(const std::string& input) {
        if (input.empty()) return "";

        // Calculate maximum decoded size
        size_t max_decoded_size = input.size();  // Base64 decodes to smaller size
        std::vector<uint8_t> output(max_decoded_size);

        // Decode using FFmpeg's base64 implementation
        int decoded_size = av_base64_decode(output.data(), input.c_str(), max_decoded_size);

        if (decoded_size > 0) {
            return std::string(reinterpret_cast<char*>(output.data()), decoded_size);
        }
        return "";
    }

    bool HasSavedFrameioToken() {
        // Check if token exists in frameio_import_state (loaded from settings)
        return strlen(frameio_import_state.token_buffer) > 0;
    }

    void ClearSavedToken() {
        // Clear the token buffer
        frameio_import_state.token_buffer[0] = '\0';
        // Save settings to persist the cleared state
        SaveSettings();
        Debug::Log("Cleared saved Frame.io token from settings");
    }

    // ============================================================================
    // USER SETTINGS PERSISTENCE
    // ============================================================================

    std::string GetSettingsPath() {
        const char* localappdata = std::getenv("LOCALAPPDATA");
        if (localappdata) {
            std::string base_path = std::string(localappdata) + "\\ump";
            // Ensure directory exists
            std::filesystem::create_directories(base_path);
            return base_path + "\\settings.ump";
        }
        return "settings.ump";  // Fallback to current directory
    }

    std::string GetLayoutIniPath() {
        const char* localappdata = std::getenv("LOCALAPPDATA");
        if (localappdata) {
            std::string base_path = std::string(localappdata) + "\\ump";
            // Ensure directory exists
            std::filesystem::create_directories(base_path);
            return base_path + "\\layout.ini";
        }
        return "imgui.ini";  // Fallback to default
    }

    void LoadSettings() {
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
                if (j["appearance"].contains("video_background")) {
                    std::string bg_str = j["appearance"]["video_background"].get<std::string>();
                    if (bg_str == "BLACK") video_background_type = VideoBackgroundType::BLACK;
                    else if (bg_str == "DEFAULT") video_background_type = VideoBackgroundType::DEFAULT;
                    else if (bg_str == "DARK_CHECKERBOARD") video_background_type = VideoBackgroundType::DARK_CHECKERBOARD;
                    else if (bg_str == "LIGHT_CHECKERBOARD") video_background_type = VideoBackgroundType::LIGHT_CHECKERBOARD;
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
                if (j["exr_cache"].contains("cache_gb")) {
                    g_exr_cache_gb = j["exr_cache"]["cache_gb"].get<double>();
                }
                if (j["exr_cache"].contains("read_behind_seconds")) {
                    g_read_behind_seconds = j["exr_cache"]["read_behind_seconds"].get<float>();
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
                if (j["disk_cache"].contains("dummy_cache_max_gb")) {
                    g_dummy_cache_max_gb = j["disk_cache"]["dummy_cache_max_gb"].get<int>();
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
                if (j["panels"].contains("show_stats")) {
                    show_system_stats_bar = j["panels"]["show_stats"].get<bool>();
                }
            }

            Debug::Log("Loaded user settings from: " + settings_path);

        } catch (const std::exception& e) {
            Debug::Log("Error loading settings: " + std::string(e.what()));
        }
    }

    void SaveSettings() {
        try {
            nlohmann::json j;

            // Appearance settings
            j["appearance"]["use_windows_accent_color"] = use_windows_accent_color;

            std::string bg_str = "BLACK";
            switch (video_background_type) {
                case VideoBackgroundType::BLACK: bg_str = "BLACK"; break;
                case VideoBackgroundType::DEFAULT: bg_str = "DEFAULT"; break;
                case VideoBackgroundType::DARK_CHECKERBOARD: bg_str = "DARK_CHECKERBOARD"; break;
                case VideoBackgroundType::LIGHT_CHECKERBOARD: bg_str = "LIGHT_CHECKERBOARD"; break;
            }
            j["appearance"]["video_background"] = bg_str;

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
            j["exr_cache"]["cache_gb"] = g_exr_cache_gb;
            j["exr_cache"]["read_behind_seconds"] = g_read_behind_seconds;

            // Performance settings (image sequence I/O + EXR transcode)
            j["performance"]["exr_io_threads"] = g_exr_thread_count;
            j["performance"]["exr_transcode_threads"] = g_exr_transcode_threads;

            // Disk cache settings
            j["disk_cache"]["custom_path"] = g_custom_cache_path;
            j["disk_cache"]["retention_days"] = g_cache_retention_days;
            j["disk_cache"]["dummy_cache_max_gb"] = g_dummy_cache_max_gb;
            j["disk_cache"]["transcode_cache_max_gb"] = g_transcode_cache_max_gb;
            j["disk_cache"]["clear_on_exit"] = g_clear_cache_on_exit;

            // Thumbnail scrubbing settings
            j["thumbnails"]["enabled"] = cache_settings.enable_thumbnails;
            j["thumbnails"]["width"] = cache_settings.thumbnail_width;
            j["thumbnails"]["height"] = cache_settings.thumbnail_height;
            j["thumbnails"]["cache_size"] = cache_settings.thumbnail_cache_size;

            // Playback settings
            j["playback"]["auto_play_on_load"] = cache_settings.auto_play_on_load;

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
            j["panels"]["show_stats"] = show_system_stats_bar;

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

    void DeleteAllPreferences() {
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

    // Frame.io import state
    struct FrameioImportState {
        bool show_dialog = false;
        char token_buffer[256] = "";
        char url_buffer[512] = "";
        bool importing = false;
        std::string status_message;
        bool import_success = false;

        // Thumbnail generation state
        bool generating_thumbnails = false;
        std::vector<ump::AnnotationNote> imported_notes;
        size_t current_thumbnail_index = 0;
        bool waiting_for_seek = false;
        int frames_to_wait_after_seek = 0;
    };

    FrameioImportState frameio_import_state;

    // Process Frame.io thumbnail generation (called once per frame)
    void ProcessFrameioThumbnailGeneration() {
        if (!frameio_import_state.generating_thumbnails) return;

        // Wait for seek to complete
        if (frameio_import_state.frames_to_wait_after_seek > 0) {
            frameio_import_state.frames_to_wait_after_seek--;
            return;
        }

        // Capture thumbnail after seek completes
        if (frameio_import_state.waiting_for_seek) {
            frameio_import_state.waiting_for_seek = false;

            // Capture screenshot for current note
            if (frameio_import_state.current_thumbnail_index < frameio_import_state.imported_notes.size()) {
                auto& note = frameio_import_state.imported_notes[frameio_import_state.current_thumbnail_index];

                // Generate filename based on timecode
                std::string filename = "note_" + note.timecode + ".png";
                // Replace colons with underscores for valid filename
                std::replace(filename.begin(), filename.end(), ':', '_');

                // Get the annotations directory for the current project
                if (annotation_manager) {
                    std::filesystem::path annotations_dir = annotation_manager->GetAnnotationsDirectory();
                    std::filesystem::path images_dir = annotations_dir / "images";

                    // Ensure images directory exists
                    std::filesystem::create_directories(images_dir);

                    std::filesystem::path full_path = images_dir / filename;

                    // Capture screenshot
                    if (video_player && video_player->CaptureScreenshotToPath(images_dir.string(), filename)) {
                        // Wait for file to be written to disk (with timeout)
                        int wait_attempts = 0;
                        const int max_attempts = 10;  // 10 frames max
                        while (wait_attempts < max_attempts && !std::filesystem::exists(full_path)) {
                            wait_attempts++;
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }

                        // Verify file exists before updating
                        if (std::filesystem::exists(full_path)) {
                            std::string relative_path = "images/" + filename;
                            annotation_manager->UpdateNoteImagePath(note.timecode, relative_path);
                            Debug::Log("Generated thumbnail for " + note.timecode + ": " + relative_path);
                        } else {
                            Debug::Log("Warning: Thumbnail file not found after capture: " + full_path.string());
                        }
                    }
                }

                frameio_import_state.current_thumbnail_index++;
            }

            // Check if done
            if (frameio_import_state.current_thumbnail_index >= frameio_import_state.imported_notes.size()) {
                Debug::Log("Finished generating thumbnails for imported notes");

                // Disable batch mode and do final save (single save instead of one per thumbnail)
                if (annotation_manager) {
                    annotation_manager->SetBatchMode(false);
                    annotation_manager->ForceSave();
                    Debug::Log("Disabled batch mode and saved all thumbnail paths");
                }

                frameio_import_state.generating_thumbnails = false;
                frameio_import_state.imported_notes.clear();
                frameio_import_state.current_thumbnail_index = 0;

                // Update status message in dialog
                frameio_import_state.status_message += "\nThumbnails generated successfully!";
                return;
            }

            // Update status
            frameio_import_state.status_message = "Generating thumbnails... (" +
                std::to_string(frameio_import_state.current_thumbnail_index) + "/" +
                std::to_string(frameio_import_state.imported_notes.size()) + ")";

            // Fall through to process next note
        }

        // Process next note
        if (frameio_import_state.current_thumbnail_index < frameio_import_state.imported_notes.size()) {
            const auto& note = frameio_import_state.imported_notes[frameio_import_state.current_thumbnail_index];

            // Seek to the timestamp
            if (video_player) {
                double current_pos = video_player->GetPosition();
                const double epsilon = 0.001;

                if (std::abs(current_pos - note.timestamp_seconds) > epsilon) {
                    video_player->Seek(note.timestamp_seconds);
                    frameio_import_state.frames_to_wait_after_seek = 3;
                    frameio_import_state.waiting_for_seek = true;
                } else {
                    // Already at position
                    frameio_import_state.frames_to_wait_after_seek = 1;
                    frameio_import_state.waiting_for_seek = true;
                }
            }
        }
    }

    // Queue a frame capture (called from export flow)
    void QueueFrameCapture(const std::string& output_path,
                          const std::vector<ump::Annotations::ActiveStroke>& strokes,
                          int export_width = 0,
                          int export_height = 0) {
        pending_capture.output_path = output_path;
        pending_capture.strokes = strokes;
        pending_capture.pending = true;
        pending_capture.completed = false;
        pending_capture.just_queued = true;  // Delay capture by one frame
        pending_capture.display_pos = ImVec2(0, 0);  // Reset
        pending_capture.display_size = ImVec2(0, 0);  // Reset

        // If export dimensions provided, use native resolution rendering
        if (export_width > 0 && export_height > 0) {
            pending_capture.use_native_resolution = true;
            pending_capture.export_width = export_width;
            pending_capture.export_height = export_height;
        } else {
            pending_capture.use_native_resolution = false;
        }
    }

    // Wait for capture to complete (yields to render loop)
    // Process export state machine (called once per frame)
    void ProcessExportStateMachine() {
        if (!export_state.active) {
            return;
        }

        // If waiting for window resize, decrement counter
        if (export_state.frames_to_wait_for_resize > 0) {
            export_state.frames_to_wait_for_resize--;
            return;
        }

        // If waiting for frames after seek, decrement counter
        if (export_state.frames_to_wait_after_seek > 0) {
            export_state.frames_to_wait_after_seek--;
            return;
        }

        // If we just finished waiting for seek, queue the capture now
        if (export_state.waiting_for_seek) {
            Debug::Log("Seek complete, queueing capture at native resolution");
            QueueFrameCapture(export_state.pending_output_path, export_state.pending_strokes,
                            export_state.options.width, export_state.options.height);
            export_state.waiting_for_capture = true;
            export_state.waiting_for_seek = false;
            // Return and let next frame render with the queued capture (so display area gets set)
            return;
        }

        // If waiting for a capture to complete, check if it's done
        if (export_state.waiting_for_capture) {
            if (!pending_capture.pending) {
                // Capture completed (or failed)
                export_state.waiting_for_capture = false;

                if (pending_capture.success) {
                    export_state.captured_images.push_back(pending_capture.output_path);
                    export_state.current_note_index++;
                } else {
                    // Capture failed - abort export
                    Debug::Log("Export error: Failed to capture image for note at index " + std::to_string(export_state.current_note_index));
                    FinalizeExport(false);
                    return;
                }
            } else {
                // Still waiting for capture
                return;
            }
        }

        // Check if we've processed all notes
        if (export_state.current_note_index >= export_state.notes.size()) {
            // All captures done - finalize export
            FinalizeExport(true);
            return;
        }

        // Process next note
        const auto& note = export_state.notes[export_state.current_note_index];

        Debug::Log("Processing note " + std::to_string(export_state.current_note_index) +
                   " at timestamp " + std::to_string(note.timestamp_seconds) +
                   " (timecode: " + note.timecode + ")");

        // Parse annotation strokes for this note
        std::vector<ump::Annotations::ActiveStroke> strokes;
        if (!note.annotation_data.empty()) {
            strokes = ump::Annotations::AnnotationSerializer::JsonStringToStrokes(note.annotation_data);
        }

        // Generate output path (use filesystem::path to ensure correct separators)
        std::filesystem::path output_path = std::filesystem::path(export_state.temp_dir) /
            ("note_" + ump::Annotations::AnnotationExporter::FormatTimecode(note.timestamp_seconds, export_state.options.frame_rate) + ".png");

        // Save capture data for after seek completes
        export_state.pending_output_path = output_path.string();
        export_state.pending_strokes = strokes;

        // Seek to timestamp and wait for it to complete before queueing capture
        if (video_player) {
            double current_pos = video_player->GetPosition();
            // Use small epsilon for floating point comparison (1ms tolerance)
            const double epsilon = 0.001;

            if (std::abs(current_pos - note.timestamp_seconds) > epsilon) {
                // Need to seek to different position
                Debug::Log("Seeking from " + std::to_string(current_pos) + " to " + std::to_string(note.timestamp_seconds));
                video_player->Seek(note.timestamp_seconds);
                // Wait 5 frames for seek to complete and frame to render (increased for EXR/slower codecs)
                export_state.frames_to_wait_after_seek = 5;
                export_state.waiting_for_seek = true;
            } else {
                // Already at correct position, just wait for frame to be ready
                Debug::Log("Already at position " + std::to_string(current_pos) + ", skipping seek");
                // Wait 2 frames to ensure current frame is fully rendered
                export_state.frames_to_wait_after_seek = 2;
                export_state.waiting_for_seek = true;
            }
        }
    }

    void FinalizeExport(bool success) {
        // Note: No UI restoration needed - offscreen rendering doesn't change UI state

        if (!success) {
            export_state.active = false;
            Debug::Log("Export aborted");
            return;
        }

        // Create a mapping from note timestamps to captured image paths
        std::map<double, std::string> timestamp_to_image;
        for (size_t i = 0; i < export_state.notes.size() && i < export_state.captured_images.size(); i++) {
            timestamp_to_image[export_state.notes[i].timestamp_seconds] = export_state.captured_images[i];
        }

        // Set up capture callback to return pre-captured images
        annotation_exporter->SetCaptureCallback([timestamp_to_image](
            double timestamp_seconds,
            const std::string& annotation_data,
            const std::string& output_path
        ) -> bool {
            // Find the pre-captured image for this timestamp
            auto it = timestamp_to_image.find(timestamp_seconds);
            if (it != timestamp_to_image.end()) {
                std::filesystem::path source_path = it->second;
                std::filesystem::path dest_path = output_path;

                // Check if source file exists
                if (!std::filesystem::exists(source_path)) {
                    Debug::Log("Source image not found: " + source_path.string());
                    return false;
                }

                // Compare normalized paths to check if they're the same
                try {
                    if (std::filesystem::absolute(source_path) == std::filesystem::absolute(dest_path)) {
                        return true;  // Same file, no need to copy
                    }
                } catch (...) {
                    // Path comparison failed, continue with copy
                }

                // Copy the captured image to the requested output path
                try {
                    std::filesystem::copy_file(source_path, dest_path,
                        std::filesystem::copy_options::overwrite_existing);
                    return true;
                } catch (const std::exception& e) {
                    Debug::Log("Failed to copy image: " + std::string(e.what()));
                    return false;
                }
            }
            return false;
        });

        // Now call the exporter to create the final document (it will use pre-captured images)
        std::string result_path = annotation_exporter->ExportNotes(export_state.notes, export_state.options);

        if (!result_path.empty()) {
            Debug::Log("Export completed successfully: " + result_path);
        } else {
            Debug::Log("Export failed during document generation");
        }

        export_state.active = false;
    }

    // Capture the currently rendered frame (called during render loop)
    bool CaptureRenderedFrame() {
        if (!pending_capture.pending) {
            return false;
        }

        // Skip capture on first frame after queueing - wait for video texture to update
        if (pending_capture.just_queued) {
            Debug::Log("Skipping capture on first frame, waiting for video texture to update after seek");
            pending_capture.just_queued = false;
            return false;  // Wait one frame for video player to render the seeked frame
        }

        int capture_x, capture_y, capture_width, capture_height;
        std::vector<unsigned char> screen_data;
        bool used_offscreen_rendering = false;

        // Check if we're exporting at native resolution (offscreen)
        if (pending_capture.use_native_resolution) {
            // Use the full video resolution for export
            capture_width = pending_capture.export_width;
            capture_height = pending_capture.export_height;
            capture_x = 0;
            capture_y = 0;

            Debug::Log("Using native resolution for export: " + std::to_string(capture_width) + "x" + std::to_string(capture_height));
            Debug::Log("Viewport size at creation: " + std::to_string(pending_capture.viewport_width_at_creation) + "x" + std::to_string(pending_capture.display_size.y));

            // Render to offscreen framebuffer at native resolution
            GLuint fbo = 0;
            GLuint render_texture = 0;

            // Create framebuffer
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);

            // Create texture for rendering
            glGenTextures(1, &render_texture);
            glBindTexture(GL_TEXTURE_2D, render_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, capture_width, capture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            // Attach texture to framebuffer
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_texture, 0);

            // Check framebuffer status
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                Debug::Log("Framebuffer creation failed with status: " + std::to_string(status));
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteTextures(1, &render_texture);
                glDeleteFramebuffers(1, &fbo);
            } else {
                // Set viewport to native resolution
                glViewport(0, 0, capture_width, capture_height);

                // Clear framebuffer
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                // Get video texture from video player
                if (video_player) {
                    GLuint video_texture = video_player->GetCurrentVideoTexture();
                    int video_width = video_player->GetVideoWidth();
                    int video_height = video_player->GetVideoHeight();

                    if (video_texture != 0 && video_width > 0 && video_height > 0) {
                        Debug::Log("Rendering video texture " + std::to_string(video_texture) +
                                  " (" + std::to_string(video_width) + "x" + std::to_string(video_height) + ") to framebuffer");

                        // Apply OCIO color correction if available
                        GLuint display_texture = video_texture;
                        if (video_player->HasColorPipeline()) {
                            GLuint color_corrected = video_player->CreateColorCorrectedTexture(
                                video_texture, video_width, video_height,
                                capture_width, capture_height
                            );
                            if (color_corrected != 0) {
                                display_texture = color_corrected;
                                Debug::Log("Applied OCIO color correction");
                            }
                        }

                        // Render video texture to framebuffer using simple quad
                        glEnable(GL_BLEND);
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                        // Simple passthrough shader
                        const char* vertex_shader = R"(
                            #version 330 core
                            layout(location = 0) in vec2 aPos;
                            layout(location = 1) in vec2 aTexCoord;
                            out vec2 TexCoord;
                            void main() {
                                gl_Position = vec4(aPos, 0.0, 1.0);
                                TexCoord = aTexCoord;
                            }
                        )";

                        const char* fragment_shader = R"(
                            #version 330 core
                            in vec2 TexCoord;
                            out vec4 FragColor;
                            uniform sampler2D uTexture;
                            void main() {
                                FragColor = texture(uTexture, TexCoord);
                            }
                        )";

                        // Compile shaders
                        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                        glShaderSource(vs, 1, &vertex_shader, nullptr);
                        glCompileShader(vs);

                        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                        glShaderSource(fs, 1, &fragment_shader, nullptr);
                        glCompileShader(fs);

                        GLuint shader_program = glCreateProgram();
                        glAttachShader(shader_program, vs);
                        glAttachShader(shader_program, fs);
                        glLinkProgram(shader_program);

                        // Fullscreen quad (flip V coordinate to fix upside-down image)
                        float quad_vertices[] = {
                            -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left: U=0, V=1 (flipped)
                             1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right: U=1, V=1 (flipped)
                             1.0f,  1.0f,  1.0f, 0.0f,  // top-right: U=1, V=0 (flipped)
                            -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left: U=0, V=1 (flipped)
                             1.0f,  1.0f,  1.0f, 0.0f,  // top-right: U=1, V=0 (flipped)
                            -1.0f,  1.0f,  0.0f, 0.0f   // top-left: U=0, V=0 (flipped)
                        };

                        GLuint vao, vbo;
                        glGenVertexArrays(1, &vao);
                        glGenBuffers(1, &vbo);
                        glBindVertexArray(vao);
                        glBindBuffer(GL_ARRAY_BUFFER, vbo);
                        glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
                        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
                        glEnableVertexAttribArray(0);
                        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
                        glEnableVertexAttribArray(1);

                        // Render video texture
                        glUseProgram(shader_program);
                        glBindTexture(GL_TEXTURE_2D, display_texture);
                        glDrawArrays(GL_TRIANGLES, 0, 6);

                        // Cleanup video shader resources
                        glDeleteVertexArrays(1, &vao);
                        glDeleteBuffers(1, &vbo);
                        glDeleteProgram(shader_program);
                        glDeleteShader(vs);
                        glDeleteShader(fs);

                        // Clean up color corrected texture if we created one
                        if (display_texture != video_texture) {
                            glDeleteTextures(1, &display_texture);
                        }

                        // Render annotations on top using raw OpenGL
                        if (!pending_capture.strokes.empty()) {
                            Debug::Log("Rendering " + std::to_string(pending_capture.strokes.size()) + " annotation strokes");

                            // Calculate line width scale factor based on viewport vs export resolution
                            // Scale = export_width / viewport_width (what user sees is what they get, scaled)
                            float viewport_width = pending_capture.viewport_width_at_creation;
                            float line_width_scale = (float)capture_width / viewport_width;
                            Debug::Log("Line width scale: viewport=" + std::to_string(viewport_width) +
                                      "px, export=" + std::to_string(capture_width) +
                                      "px, scale=" + std::to_string(line_width_scale) + "x");

                            // Simple line rendering shader
                            const char* line_vs = R"(
                                #version 330 core
                                layout(location = 0) in vec2 aPos;
                                void main() {
                                    gl_Position = vec4(aPos, 0.0, 1.0);
                                }
                            )";

                            const char* line_fs = R"(
                                #version 330 core
                                out vec4 FragColor;
                                uniform vec4 uColor;
                                void main() {
                                    FragColor = uColor;
                                }
                            )";

                            GLuint line_vs_shader = glCreateShader(GL_VERTEX_SHADER);
                            glShaderSource(line_vs_shader, 1, &line_vs, nullptr);
                            glCompileShader(line_vs_shader);

                            GLuint line_fs_shader = glCreateShader(GL_FRAGMENT_SHADER);
                            glShaderSource(line_fs_shader, 1, &line_fs, nullptr);
                            glCompileShader(line_fs_shader);

                            GLuint line_program = glCreateProgram();
                            glAttachShader(line_program, line_vs_shader);
                            glAttachShader(line_program, line_fs_shader);
                            glLinkProgram(line_program);

                            glUseProgram(line_program);
                            GLint color_loc = glGetUniformLocation(line_program, "uColor");

                            // Enable blending for transparency
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                            // Render each stroke
                            for (const auto& stroke : pending_capture.strokes) {
                                if (stroke.points.empty()) continue;

                                // Convert normalized points (0-1) to NDC (-1 to 1)
                                std::vector<float> vertices;
                                for (const auto& pt : stroke.points) {
                                    float x = pt.x * 2.0f - 1.0f;
                                    float y = -(pt.y * 2.0f - 1.0f);  // Flip Y for OpenGL
                                    vertices.push_back(x);
                                    vertices.push_back(y);
                                }

                                // Set color
                                glUniform4f(color_loc, stroke.color.x, stroke.color.y, stroke.color.z, stroke.color.w);

                                // Set line width (scaled for output resolution)
                                float scaled_line_width = stroke.stroke_width * line_width_scale;
                                glLineWidth(scaled_line_width);

                                // Create VAO/VBO for this stroke
                                GLuint stroke_vao, stroke_vbo;
                                glGenVertexArrays(1, &stroke_vao);
                                glGenBuffers(1, &stroke_vbo);
                                glBindVertexArray(stroke_vao);
                                glBindBuffer(GL_ARRAY_BUFFER, stroke_vbo);
                                glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
                                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
                                glEnableVertexAttribArray(0);

                                // Render based on tool type
                                using DrawingTool = ump::Annotations::DrawingTool;
                                switch (stroke.tool) {
                                    case DrawingTool::FREEHAND:
                                        glDrawArrays(GL_LINE_STRIP, 0, vertices.size() / 2);
                                        break;

                                    case DrawingTool::LINE:
                                        if (stroke.points.size() >= 2) {
                                            glDrawArrays(GL_LINES, 0, 2);
                                        }
                                        break;

                                    case DrawingTool::ARROW:
                                        if (stroke.points.size() >= 2) {
                                            // Draw main line
                                            glDrawArrays(GL_LINES, 0, 2);

                                            // Draw arrowhead (matching ImGui's calculation with viewport scaling)
                                            ImVec2 start = stroke.points[0];
                                            ImVec2 end = stroke.points[1];

                                            // Direction vector (start ? end, matching ImGui)
                                            float dir_x = start.x - end.x;
                                            float dir_y = start.y - end.y;
                                            float len = std::sqrt(dir_x * dir_x + dir_y * dir_y);

                                            if (len > 0.001f) {
                                                // Normalize direction
                                                dir_x /= len;
                                                dir_y /= len;

                                                // Arrow size: base + thickness, scaled like line widths
                                                float arrow_base_pixels = 30.0f;  // Larger base size
                                                float arrow_size_pixels = (arrow_base_pixels + scaled_line_width * 1.5f);

                                                // Convert to normalized space (use width for consistent scaling)
                                                float arrow_size = arrow_size_pixels / (float)capture_width;

                                                // 30 degree rotation (matching ImGui)
                                                float cos_angle = std::cos(0.523599f);  // cos(30�)
                                                float sin_angle = std::sin(0.523599f);  // sin(30�)

                                                // Calculate wing 1 (rotation matrix)
                                                float ax1 = end.x + arrow_size * (dir_x * cos_angle - dir_y * sin_angle);
                                                float ay1 = end.y + arrow_size * (dir_x * sin_angle + dir_y * cos_angle);

                                                // Calculate wing 2 (opposite rotation)
                                                float ax2 = end.x + arrow_size * (dir_x * cos_angle + dir_y * sin_angle);
                                                float ay2 = end.y + arrow_size * (-dir_x * sin_angle + dir_y * cos_angle);

                                                // Convert to NDC coordinates
                                                float tip_x = end.x * 2.0f - 1.0f;
                                                float tip_y = -(end.y * 2.0f - 1.0f);
                                                float w1_x = ax1 * 2.0f - 1.0f;
                                                float w1_y = -(ay1 * 2.0f - 1.0f);
                                                float w2_x = ax2 * 2.0f - 1.0f;
                                                float w2_y = -(ay2 * 2.0f - 1.0f);

                                                // Draw filled triangle (matching ImGui's AddTriangleFilled)
                                                float arrow_verts[] = {
                                                    tip_x, tip_y,  // tip
                                                    w1_x, w1_y,    // wing 1
                                                    w2_x, w2_y     // wing 2
                                                };

                                                glBufferData(GL_ARRAY_BUFFER, sizeof(arrow_verts), arrow_verts, GL_STATIC_DRAW);
                                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                            }
                                        }
                                        break;

                                    case DrawingTool::RECTANGLE:
                                        if (stroke.points.size() >= 4) {
                                            // Rectangle has 4 points: [0]=top-left, [1]=top-right, [2]=bottom-right, [3]=bottom-left
                                            ImVec2 p0 = stroke.points[0];  // top-left
                                            ImVec2 p1 = stroke.points[1];  // top-right
                                            ImVec2 p2 = stroke.points[2];  // bottom-right
                                            ImVec2 p3 = stroke.points[3];  // bottom-left
                                            float rect_verts[] = {
                                                p0.x * 2.0f - 1.0f, -(p0.y * 2.0f - 1.0f),  // top-left
                                                p1.x * 2.0f - 1.0f, -(p1.y * 2.0f - 1.0f),  // top-right
                                                p2.x * 2.0f - 1.0f, -(p2.y * 2.0f - 1.0f),  // bottom-right
                                                p3.x * 2.0f - 1.0f, -(p3.y * 2.0f - 1.0f)   // bottom-left
                                            };
                                            glBufferData(GL_ARRAY_BUFFER, sizeof(rect_verts), rect_verts, GL_STATIC_DRAW);
                                            glDrawArrays(GL_LINE_LOOP, 0, 4);
                                        }
                                        break;

                                    case DrawingTool::OVAL:
                                        if (stroke.points.size() >= 2) {
                                            // Oval format: point[0] = center (normalized), point[1] = radii (normalized)
                                            ImVec2 center = stroke.points[0];
                                            ImVec2 radii = stroke.points[1];

                                            // Radii are in normalized space (relative to video dimensions)
                                            // No additional scaling needed - they're already proportional

                                            // Generate ellipse vertices
                                            const int segments = 32;
                                            std::vector<float> circle_verts;
                                            for (int i = 0; i <= segments; i++) {
                                                float angle = (float)i / segments * 2.0f * 3.14159f;
                                                float x = (center.x + radii.x * std::cos(angle)) * 2.0f - 1.0f;
                                                float y = -((center.y + radii.y * std::sin(angle)) * 2.0f - 1.0f);
                                                circle_verts.push_back(x);
                                                circle_verts.push_back(y);
                                            }
                                            glBufferData(GL_ARRAY_BUFFER, circle_verts.size() * sizeof(float), circle_verts.data(), GL_STATIC_DRAW);
                                            glDrawArrays(GL_LINE_STRIP, 0, segments + 1);
                                        }
                                        break;

                                    default:
                                        break;
                                }

                                // Cleanup stroke resources
                                glDeleteVertexArrays(1, &stroke_vao);
                                glDeleteBuffers(1, &stroke_vbo);
                            }

                            // Cleanup annotation shader
                            glDeleteProgram(line_program);
                            glDeleteShader(line_vs_shader);
                            glDeleteShader(line_fs_shader);
                            glDisable(GL_BLEND);

                            Debug::Log("Annotation rendering complete");
                        }

                        // Read pixels from framebuffer
                        size_t buffer_size = static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4;
                        screen_data.resize(buffer_size);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, capture_width, capture_height, GL_RGBA, GL_UNSIGNED_BYTE, screen_data.data());

                        used_offscreen_rendering = true;
                        Debug::Log("Successfully rendered to offscreen framebuffer");
                    } else {
                        Debug::Log("No valid video texture available for offscreen rendering");
                    }
                }

                // Cleanup framebuffer
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteTextures(1, &render_texture);
                glDeleteFramebuffers(1, &fbo);

                // Restore viewport
                GLint viewport[4];
                glfwGetFramebufferSize(window, &viewport[2], &viewport[3]);
                glViewport(0, 0, viewport[2], viewport[3]);
            }

        } else {
            // Get the video display area (cropped to exclude menu bar and letterboxing)
            capture_x = static_cast<int>(pending_capture.display_pos.x);
            capture_width = static_cast<int>(pending_capture.display_size.x);
            capture_height = static_cast<int>(pending_capture.display_size.y);

            // Get viewport dimensions
            GLint viewport[4];
            glGetIntegerv(GL_VIEWPORT, viewport);

            // Fallback: if display area not set yet (viewport not calculated), use full framebuffer
            if (capture_width <= 0 || capture_height <= 0) {
                capture_x = viewport[0];
                capture_y = viewport[1];
                capture_width = viewport[2];
                capture_height = viewport[3];
                Debug::Log("Display area not set, using full viewport: " + std::to_string(capture_width) + "x" + std::to_string(capture_height));
            } else {
                // Use ImGui coordinates directly - they already account for the menu bar
                // No offset needed since display_pos is already in screen coordinates
                int imgui_y = static_cast<int>(pending_capture.display_pos.y);
                capture_y = imgui_y;

                Debug::Log("Capture area: x=" + std::to_string(capture_x) +
                          " y=" + std::to_string(capture_y) +
                          " width=" + std::to_string(capture_width) + " height=" + std::to_string(capture_height));
            }

            // Only do screen capture if we didn't use offscreen rendering
            if (!used_offscreen_rendering) {
                // Calculate buffer size with overflow check
                size_t buffer_size = static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4;
                Debug::Log("Allocating buffer: " + std::to_string(buffer_size) + " bytes for " +
                           std::to_string(capture_width) + "x" + std::to_string(capture_height));

                // Set pixel store alignment for proper reading (especially important for non-multiple-of-4 widths)
                glPixelStorei(GL_PACK_ALIGNMENT, 1);

                // Read pixels from the video display area only
                screen_data.resize(buffer_size);
                glReadPixels(capture_x, capture_y, capture_width, capture_height, GL_RGBA, GL_UNSIGNED_BYTE, screen_data.data());

                // Check for OpenGL errors
                GLenum gl_error = glGetError();
                if (gl_error != GL_NO_ERROR) {
                    Debug::Log("OpenGL error during glReadPixels: " + std::to_string(gl_error));
                }
            }
        }

        // Validate dimensions
        if (capture_width <= 0 || capture_height <= 0) {
            Debug::Log("Invalid capture dimensions: " + std::to_string(capture_width) + "x" + std::to_string(capture_height));
            pending_capture.completed = true;
            pending_capture.success = false;
            pending_capture.pending = false;
            return false;
        }

        // Validate we have data
        if (screen_data.empty()) {
            Debug::Log("No screen data captured");
            pending_capture.completed = true;
            pending_capture.success = false;
            pending_capture.pending = false;
            return false;
        }

        // Flip vertically (OpenGL is bottom-up)
        size_t buffer_size = static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4;
        std::vector<unsigned char> flipped(buffer_size);
        int row_size = capture_width * 4;
        for (int y = 0; y < capture_height; y++) {
            memcpy(&flipped[y * row_size],
                   &screen_data[(capture_height - 1 - y) * row_size],
                   row_size);
        }

        // Save the captured frame
        bool success = stbi_write_png(pending_capture.output_path.c_str(), capture_width, capture_height, 4, flipped.data(), row_size) != 0;

        // Reset pixel store alignment to default
        glPixelStorei(GL_PACK_ALIGNMENT, 4);

        if (success) {
            Debug::Log("Successfully captured frame: " + pending_capture.output_path);
        } else {
            Debug::Log("Failed to save frame: " + pending_capture.output_path);
        }

        // Mark capture as completed
        pending_capture.completed = true;
        pending_capture.success = success;
        pending_capture.pending = false;

        return success;
    }

    // Capture video frame with annotations (synchronous)
    // Start export process (initiates state machine)
    void StartExport(
        ump::Annotations::AnnotationExporter::ExportFormat format,
        const ump::Annotations::AnnotationExporter::ExportOptions& options,
        const std::vector<ump::AnnotationNote>& notes,
        const std::string& temp_dir
    ) {
        export_state.active = true;
        export_state.format = format;
        export_state.options = options;
        export_state.notes = notes;
        export_state.current_note_index = 0;
        export_state.temp_dir = temp_dir;
        export_state.captured_images.clear();
        export_state.waiting_for_capture = false;
        export_state.frames_to_wait_after_seek = 0;
        export_state.frames_to_wait_for_resize = 2;  // Wait 2 frames for video sync before starting

        // Note: No need to hide UI or resize window - offscreen FBO rendering is independent of display

        Debug::Log("Export state machine started with " + std::to_string(notes.size()) + " notes");
        Debug::Log("Will render offscreen at video resolution: " + std::to_string(options.width) + "x" + std::to_string(options.height));
    }

    // ------------------------------------------------------------------------
    // URI ENCODING/DECODING FOR PROJECT SHARING
    // ------------------------------------------------------------------------
    std::string EncodeURIComponent(const std::string& str) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : str) {
            // Keep alphanumeric and certain characters
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') {
                escaped << c;
            } else {
                // Encode other characters
                escaped << '%' << std::setw(2) << int((unsigned char)c);
            }
        }

        return escaped.str();
    }

    std::string DecodeURIComponent(const std::string& str) {
        std::ostringstream decoded;
        for (size_t i = 0; i < str.length(); i++) {
            if (str[i] == '%' && i + 2 < str.length()) {
                // Decode hex sequence
                int value;
                std::istringstream is(str.substr(i + 1, 2));
                if (is >> std::hex >> value) {
                    decoded << static_cast<char>(value);
                    i += 2;
                }
            } else {
                decoded << str[i];
            }
        }
        return decoded.str();
    }

    std::string BuildProjectURI(const std::string& project_path) {
        // Convert backslashes to forward slashes
        std::string normalized_path = project_path;
        std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');

        // Encode the path for URI
        std::string encoded_path = EncodeURIComponent(normalized_path);

        // Build URI: ump:///path
        return "ump:///" + encoded_path;
    }

    std::string ParseProjectURI(const std::string& uri) {
        // Check if it starts with ump:///
        if (uri.substr(0, 7) != "ump:///") {
            return "";
        }

        // Extract path after ump:///
        std::string encoded_path = uri.substr(7);

        // Decode the path
        std::string decoded_path = DecodeURIComponent(encoded_path);

        // Convert back to Windows path (forward slashes to backslashes on Windows)
        #ifdef _WIN32
        std::replace(decoded_path.begin(), decoded_path.end(), '/', '\\');
        #endif

        return decoded_path;
    }

    void ShareProject() {
        if (!project_manager) {
            Debug::Log("ShareProject: No project manager available");
            return;
        }

        // Ensure project is saved first
        project_manager->SaveProject();

        // Get the project path
        std::string project_path = project_manager->GetProjectPath();
        if (project_path.empty()) {
            Debug::Log("ShareProject: No project file saved yet");
            ImGui::OpenPopup("No Project Saved##ShareProject");
            return;
        }

        // Build the URI
        std::string uri = BuildProjectURI(project_path);
        Debug::Log("ShareProject: Generated URI: " + uri);

        // Copy to clipboard
        ImGui::SetClipboardText(uri.c_str());
        Debug::Log("ShareProject: URI copied to clipboard");

        // Show success popup
        ImGui::OpenPopup("URI Copied##ShareProject");
    }

    // ------------------------------------------------------------------------
    // FILE OPERATIONS
    // ------------------------------------------------------------------------
    void OpenFileDialog() {
        nfdchar_t* outPath;

        // Supported formats: Video (MP4/AVI/MKV/MOV/etc), Audio (WAV/MP3/etc), Images (JPEG/PNG/TIFF/EXR)
        // Removed unsupported: DPX, TGA, BMP, JPEG2000
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

            result = NFD_OpenDialog(&outPath, filterItem, 1, nullptr);

            if (result == NFD_OKAY || result == NFD_CANCEL) {
                break;  // Success or user cancelled - stop retrying
            }

            // NFD_ERROR - may be due to cloud sync issues, try again
            Debug::Log("OpenFileDialog: NFD error on attempt " + std::to_string(attempt) + ": " + std::string(NFD_GetError()));
        }

        if (result == NFD_OKAY) {
            std::string selected_file = std::string(outPath);
            Debug::Log("*** Opening file dialog result: " + selected_file);

            // Route through project manager for sequence detection
            if (project_manager) {
                project_manager->LoadSingleFileFromDrop(selected_file);
            } else {
                // Fallback to direct loading
                current_file_path = selected_file;
                if (video_player) {
                    video_player->LoadFile(selected_file);
                }
            }

            NFD_FreePath(outPath);
        }
        else if (result == NFD_CANCEL) {
            Debug::Log("OpenFileDialog: User cancelled");
        }
        else {
            Debug::Log("OpenFileDialog: Failed after " + std::to_string(max_attempts) + " attempts");
            std::cerr << "Error opening file dialog: " << NFD_GetError() << std::endl;
        }
    }

    void CloseVideo() {
        video_player->Reset();
        current_file_path.clear();
    }

    void TriggerAutoPlay() {
        if (cache_settings.auto_play_on_load) {
            pending_auto_play = true;
            auto_play_timer = std::chrono::steady_clock::now();
            Debug::Log("Auto-play: Timer started (500ms delay)");
        }
    }

    void AddToRecentFiles(const std::string& file_path) {
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

    void ResetTimecodeStateForNewVideo() {
        timecode_state = NOT_CHECKED;
        start_timecode_checked = false;
        timecode_mode_enabled = false;  // Disable timecode mode when switching videos
        cached_start_timecode = "";
        Debug::Log("Timecode state reset for new video");
    }

    void OnVideoChanged(const std::string& new_file_path) {
        Debug::Log("Video changed to: " + new_file_path);
        ResetTimecodeStateForNewVideo();

        // Check if this is an audio file (no video frames to cache)
        bool is_audio_file = false;
        if (project_manager) {
            // Use file extension to check if audio
            size_t dot_pos = new_file_path.find_last_of('.');
            if (dot_pos != std::string::npos) {
                std::string ext = new_file_path.substr(dot_pos);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                is_audio_file = (ext == ".wav" || ext == ".mp3" || ext == ".aac" ||
                                ext == ".flac" || ext == ".ogg" || ext == ".wma" || ext == ".m4a");
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
        // Resolve actual path from mf:// or exr:// URLs for image sequences
        if (annotation_manager) {
            std::string annotation_path = new_file_path;
            if (project_manager) {
                annotation_path = project_manager->GetAnnotationPathForMedia(new_file_path);
            }
            annotation_manager->LoadNotesForMedia(annotation_path);
            Debug::Log("Loaded annotations for: " + annotation_path);
        }

        // Trigger auto-play if enabled (with 500ms delay)
        TriggerAutoPlay();
    }

    std::string FormatTime(double seconds) {
        int hours = (int)(seconds / 3600);
        int minutes = (int)(fmod(seconds, 3600.0) / 60);
        int secs = (int)fmod(seconds, 60.0);

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, secs);
        return std::string(buffer);
    }

    void ShowAllPanels() {
        // Exit minimal view if active
        minimal_view_mode = false;

        // Show standard panels
        show_project_panel = true;
        show_inspector_panel = true;
        show_timeline_panel = true;
        show_annotation_panel = true;

        // Show color panels
        show_color_panels = true;

        // Show system stats bar
        show_system_stats_bar = true;

        first_time_setup = true;
        Debug::Log("All panels visible");
    }

    void SetDefaultView() {
        // Exit minimal view if active
        minimal_view_mode = false;

        // Standard panels visible, color panels and annotations hidden
        show_project_panel = true;
        show_inspector_panel = true;
        show_timeline_panel = true;
        show_annotation_panel = false;

        show_color_panels = false;
        first_time_setup = true;
        Debug::Log("Default view activated");
    }

    void ApplyBackgroundColor() {
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

    void ToggleMute() {
        if (!is_muted) {
            volume_before_mute = current_volume;
            current_volume = 0;
            video_player->SetVolume(0);
            is_muted = true;
            Debug::Log("Muted - stored volume: " + std::to_string(volume_before_mute));
        }
        else {
            current_volume = volume_before_mute;
            video_player->SetVolume(current_volume);
            is_muted = false;
            Debug::Log("Unmuted - restored volume: " + std::to_string(current_volume));
        }
    }

    void ToggleLoop() {
        bool current_loop = video_player->IsLooping();
        bool is_playlist_mode = project_manager && project_manager->IsInSequenceMode();

        video_player->SetLoop(!current_loop);
        video_player->SetLoopMode(is_playlist_mode);

        const char* mode = is_playlist_mode ? "Playlist" : "Single File";
        const char* state = !current_loop ? "ON" : "OFF";
        Debug::Log(std::string(mode) + " Loop toggled: " + state);
    }

    // Static members for window procedure
    static WNDPROC original_wndproc;
    static Application* app_instance;

    // Custom window procedure to intercept close button in fullscreen AND handle WM_COPYDATA
    static LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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

                        // Check if it's a ump:// URI
                        if (arg.substr(0, 7) == "ump:///") {
                            Debug::Log("Received ump:// URI - parsing and loading project");
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
                        else if (arg.find(".umproj") != std::string::npos) {
                            Debug::Log("Received project file - loading");
                            app_instance->project_manager->LoadProject(arg);
                        }
                        // Regular media file
                        else {
                            Debug::Log("Received media file - loading");
                            app_instance->project_manager->LoadSingleFileFromDrop(arg);
                        }
                    } else {
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

    void SetupSingleInstanceMessaging(HWND hwnd) {
        // Store a unique property on the window so other instances can identify it
        // This is a simple, reliable method that doesn't require changing window classes
        SetPropW(hwnd, L"ump_SingleInstanceWindow", (HANDLE)0x554D50);  // "UMP" in hex

        // Hook window procedure to handle WM_COPYDATA
        app_instance = this;
        if (!original_wndproc) {
            original_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);
        }

        Debug::Log("Single-instance messaging setup complete - window tagged for IPC");
    }

    void ToggleFullscreen() {
        static int saved_x, saved_y, saved_width, saved_height;
        static bool saved_decorated = true;

        is_fullscreen = !is_fullscreen;

        if (is_fullscreen) {
            Debug::Log("Entering fast fullscreen mode");

            // Save current window state
            glfwGetWindowPos(window, &saved_x, &saved_y);
            glfwGetWindowSize(window, &saved_width, &saved_height);

            // Customize title bar buttons for fullscreen - hide minimize and maximize
            HWND hwnd = glfwGetWin32Window(window);
            LONG style = GetWindowLong(hwnd, GWL_STYLE);
            SetWindowLong(hwnd, GWL_STYLE, style & ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX)); // Hide both buttons

            // Hook window procedure to intercept close button
            if (!original_wndproc) {
                original_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);
            }

            // Enable DWM composition and extend frame for transparent title bar
            DwmEnableBlurBehindWindow(hwnd, nullptr); // Enable DWM effects

            MARGINS margins = { 0, 0, 32, 0 }; // Extend glass 32px into client area (title bar height)
            HRESULT hr = DwmExtendFrameIntoClientArea(hwnd, &margins);
            if (FAILED(hr)) {
                Debug::Log("Failed to extend glass frame for transparent title bar");
            }

            // Change window title to indicate fullscreen mode
            glfwSetWindowTitle(window, "ump - Fullscreen");

            // Fast operations only: resize + position
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowSize(window, mode->width, mode->height - 32);
            glfwSetWindowPos(window, 0, 32);
        }
        else {
            Debug::Log("Exiting fast fullscreen mode");

            // Restore title bar buttons and window procedure
            HWND hwnd = glfwGetWin32Window(window);
            LONG style = GetWindowLong(hwnd, GWL_STYLE);
            SetWindowLong(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX | WS_MAXIMIZEBOX); // Restore both buttons

            // Restore normal title bar (remove glass extension)
            MARGINS margins = { 0, 0, 0, 0 }; // Reset glass extension
            DwmExtendFrameIntoClientArea(hwnd, &margins);

            // Restore original window procedure
            if (original_wndproc) {
                SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)original_wndproc);
                original_wndproc = nullptr;
            }

            // Restore original window title
            glfwSetWindowTitle(window, "ump");

            // Fast restore: size + position only
            glfwSetWindowSize(window, saved_width > 0 ? saved_width : 1914, saved_height > 0 ? saved_height : 1060);
            glfwSetWindowPos(window, saved_x, saved_y);
        }
    }

    // Timecode

    std::string GetTimecodeOffset() const {
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

    double ParseTimecodeToSeconds(const std::string& timecode_str, double fps = 23.976) {
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

    void ResetTimecodeState() {
        timecode_state = NOT_CHECKED;
        start_timecode_checked = false;
        timecode_mode_enabled = false;
        cached_start_timecode = "";
        Debug::Log("Timecode state reset for new file");
    }

    void CheckStartTimecodeAvailability() {

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
        const ump::ProjectManager::CombinedMetadata* cached_meta =
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

                    if (cached_meta->adobe_meta->HasAnyTimecode()) {
                        // Get the first available timecode as our reference
                        if (!cached_meta->adobe_meta->qt_start_timecode.empty()) {
                            cached_start_timecode = cached_meta->adobe_meta->qt_start_timecode;
                            timecode_state = AVAILABLE;
                            Debug::Log("SUCCESS: Found QT StartTimecode in cache: " + cached_start_timecode);
                        }
                        else if (!cached_meta->adobe_meta->qt_timecode.empty()) {
                            cached_start_timecode = cached_meta->adobe_meta->qt_timecode;
                            timecode_state = AVAILABLE;
                            Debug::Log("SUCCESS: Found QT TimeCode in cache: " + cached_start_timecode);
                        }
                        else if (!cached_meta->adobe_meta->xmp_alt_timecode_time_value.empty()) {
                            cached_start_timecode = cached_meta->adobe_meta->xmp_alt_timecode_time_value;
                            timecode_state = AVAILABLE;
                            Debug::Log("SUCCESS: Found XMP AltTimecodeTimeValue in cache: " + cached_start_timecode);
                        }
                        else if (!cached_meta->adobe_meta->xmp_alt_timecode.empty()) {
                            cached_start_timecode = cached_meta->adobe_meta->xmp_alt_timecode;
                            timecode_state = AVAILABLE;
                            Debug::Log("SUCCESS: Found XMP AltTimecode in cache: " + cached_start_timecode);
                        }
                        else if (!cached_meta->adobe_meta->mxf_start_timecode.empty()) {
                            cached_start_timecode = cached_meta->adobe_meta->mxf_start_timecode;
                            timecode_state = AVAILABLE;
                            Debug::Log("SUCCESS: Found MXF StartTimecode in cache: " + cached_start_timecode);
                        }
                        else {
                            timecode_state = NOT_AVAILABLE;
                            Debug::Log("Has timecode fields but all are empty");
                        }
                    }
                    else {
                        timecode_state = NOT_AVAILABLE;
                        Debug::Log("No timecode found in metadata");
                    }
                    start_timecode_checked = true;
                }
                else {
                    Debug::Log("Adobe metadata exists but is_loaded = FALSE");
                    timecode_state = CHECKING;
                }
            }
            else {
                Debug::Log("Cached metadata exists but adobe_meta is NULL");
                timecode_state = CHECKING;
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

    std::string FormatCurrentTimecodeWithOffset(double current_seconds) {
        // Get actual FPS from video player
        double fps = video_player ? video_player->GetFrameRate() : 23.976;

        if (!timecode_mode_enabled || timecode_state != AVAILABLE || cached_start_timecode.empty()) {
            // Fallback to regular time format
            int hours = (int)(current_seconds / 3600);
            int minutes = (int)(fmod(current_seconds, 3600.0) / 60);
            int secs = (int)fmod(current_seconds, 60.0);
            int frames = (int)round((current_seconds - (int)current_seconds) * fps);

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

        // Use proper rounding instead of truncation for frame calculation
        int frames = (int)round((absolute_timecode_seconds - (int)absolute_timecode_seconds) * fps);

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
        return std::string(buffer);
    }

    void ToggleTimecodeMode() {
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

    void CopyToClipboard(const std::string& text) {
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

    void OpenFileInExplorer(const std::string& file_path) {
        if (file_path.empty()) return;

#ifdef _WIN32
        std::string windows_path = file_path;
        std::replace(windows_path.begin(), windows_path.end(), '/', '\\');

        std::string command = "explorer /select,\"" + windows_path + "\"";

        system(command.c_str());
#else
        // Placeholder for other platforms
#endif
    }

    void RenderPathWithButtons(const std::string& path, const std::string& id, bool show_open_button = true) {
        ImGui::BeginGroup();

        float button_width = 28.0f;
        int button_count = show_open_button ? 2 : 1;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float available_width = ImGui::GetContentRegionAvail().x - (button_width * button_count) - (spacing * button_count);

        // Path text
        if (font_mono) ImGui::PushFont(font_mono);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + available_width);
        ImGui::TextWrapped("%s", path.c_str());
        ImGui::PopTextWrapPos();
        if (font_mono) ImGui::PopFont();

        // Open button (only on Windows for Windows paths)
        if (show_open_button) {
            ImGui::SameLine();

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
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open in Explorer");
            }
        }

        // Copy button
        ImGui::SameLine();

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
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
            ImGui::SetTooltip("Copy to clipboard");
        }

        ImGui::EndGroup();
    }

    std::string GetFileName(const std::string& path) {
        size_t pos = path.find_last_of("/\\");
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }
};

// Static member definitions
WNDPROC Application::original_wndproc = nullptr;
Application* Application::app_instance = nullptr;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Helper function to find the existing ump instance window
// Uses window properties instead of class names for reliable identification
static HWND FindUmpWindow() {
    HWND found_hwnd = NULL;

    // Enumerate all top-level windows to find ours
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        // Check if this window has our unique property
        HANDLE prop = GetPropW(hwnd, L"ump_SingleInstanceWindow");
        if (prop == (HANDLE)0x554D50) {  // "UMP" in hex
            HWND* result = (HWND*)lParam;
            *result = hwnd;
            return FALSE;  // Stop enumeration
        }
        return TRUE;  // Continue enumeration
    }, (LPARAM)&found_hwnd);

    return found_hwnd;
}

// ============================================================================
// ENTRY POINTS
// ============================================================================
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
#ifdef _DEBUG
    if (AllocConsole()) {
        FILE* pCout;
        freopen_s(&pCout, "CONOUT$", "w", stdout);
        freopen_s(&pCout, "CONOUT$", "w", stderr);
    }
#endif

    Application app;

    if (!app.Initialize()) {
        return -1;
    }

    app.Run();
    app.Cleanup();

    return 0;
}
#endif

int main(int argc, char* argv[]) {
    // Set AppUserModelID for Windows 11 taskbar/start menu integration
    #ifdef _WIN32
    SetCurrentProcessExplicitAppUserModelID(L"cbkow.ump");
    #endif

    // Set working directory to executable's directory
    // This ensures assets are found regardless of how the app is launched
    try {
        std::filesystem::path exe_path = std::filesystem::absolute(argv[0]);
        std::filesystem::path exe_dir = exe_path.parent_path();
        std::filesystem::current_path(exe_dir);
        Debug::Log("Set working directory to: " + exe_dir.string());
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to set working directory: " << e.what() << std::endl;
    }

    // Single instance enforcement using named mutex and window messaging
    // This prevents multiple instances from conflicting with RAM cache
    // AND allows new instances to pass files/URIs to the existing instance
    static HANDLE single_instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\ump_SingleInstanceMutex");

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Debug::Log("Another instance is already running - attempting to pass command to it");

        // Find the existing instance's window using our helper function
        HWND existing_window = FindUmpWindow();

        if (existing_window) {
            Debug::Log("Found existing instance window - sending command");
            // Collect file paths from command-line arguments
            std::string args_to_send;
            for (int i = 1; i < argc; i++) {
                if (!args_to_send.empty()) args_to_send += "|";  // Use | as separator
                args_to_send += argv[i];
            }

            if (!args_to_send.empty()) {
                Debug::Log("Sending command to existing instance: " + args_to_send);

                // Use WM_COPYDATA to send the file path(s) to existing instance
                COPYDATASTRUCT cds;
                cds.dwData = 1;  // Message ID
                cds.cbData = static_cast<DWORD>(args_to_send.length() + 1);
                cds.lpData = (PVOID)args_to_send.c_str();

                SendMessageW(existing_window, WM_COPYDATA, 0, (LPARAM)&cds);

                // Bring existing window to front
                SetForegroundWindow(existing_window);
                ShowWindow(existing_window, SW_RESTORE);

                Debug::Log("Command sent successfully - exiting");
            } else {
                // No files to open, just bring existing window to front
                SetForegroundWindow(existing_window);
                ShowWindow(existing_window, SW_RESTORE);
                Debug::Log("No files to send - just bringing window to front");
            }
        } else {
            Debug::Log("ERROR: Could not find existing instance window");
            MessageBoxW(
                NULL,
                L"ump is already running, but could not communicate with the existing instance.",
                L"Application Already Running",
                MB_OK | MB_ICONWARNING
            );
        }

        if (single_instance_mutex) {
            CloseHandle(single_instance_mutex);
        }
        return 0;  // Exit gracefully
    }

    Application app;

    // Collect file paths from command-line arguments for initial instance
    std::vector<std::string> initial_files;
    for (int i = 1; i < argc; i++) {  // Skip argv[0] (executable path)
        initial_files.push_back(argv[i]);
    }

    if (!app.Initialize(initial_files)) {
        return -1;
    }

    app.Run();
    app.Cleanup();

    return 0;
}