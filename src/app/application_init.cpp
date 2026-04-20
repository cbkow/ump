// application_init.cpp - Application::Initialize()

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <VersionHelpers.h>
#include <shobjidl.h>
#include <shlobj.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

#include <glad/gl.h>
#ifdef QCVIEW_USE_METAL
// Native macOS — no GLFW
#include "macos_app_delegate.h"
#else
#ifdef QCVIEW_USE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#endif // QCVIEW_USE_METAL

#include <imgui.h>
#ifdef QCVIEW_USE_METAL
#include <imgui_impl_osx.h>
#else
#include <imgui_impl_glfw.h>
#endif
#ifdef QCVIEW_USE_VULKAN
#include <imgui_impl_vulkan.h>
#include "gpu/vulkan_device.h"
#include "gpu/vulkan_texture_pool.h"
#include "annotations/vulkan_annotation_renderer.h"
#elif defined(QCVIEW_USE_METAL)
#include "gpu/metal_device_manager.h"
#include "gpu/metal_texture_pool.h"
#include "hdr/metal_hdr_swapchain.h"
#include "annotations/metal_annotation_renderer.h"
#include "app/macos_menu_bar.h"
#else
#include <imgui_impl_opengl3.h>
#endif
#include <imgui_internal.h>
#include <implot.h>
#include <nfd.h>
#include "../external/glfw/deps/stb_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#include "app/application.h"
#include "app/app_config.h"
#include "app/app_icons.h"
#include "app/drawing_utils.h"
#include "utils/debug_utils.h"
#include "utils/asset_path.h"
#include "utils/system_pressure_monitor.h"
#include "utils/frame_indexing.h"
#include "player/video_player.h"
#include "player/thumbnail_cache.h"
#include "player/shared_memory_pool.h"
#include "player/hw_context_manager.h"
#include "player/video_decoder_factory.h"
#include "project/project_manager.h"
#include "project/media_item.h"
#include "imnodes/imnodes.h"
#include "color/ocio_config_manager.h"
#include "color/ocio_pipeline.h"
#include "ui/node_editor_theme.h"
#include "ui/timeline_manager.h"
#include "nodes/node_manager.h"
#include "overlay/safety_overlay_system.h"
#include "annotations/annotation_manager.h"
#include "ui/annotation_panel.h"
#include "annotations/viewport_annotator.h"
#include "annotations/annotation_toolbar.h"
#include "annotations/annotation_renderer.h"
#include "annotations/nanovg_context.h"
#include "annotations/annotation_exporter.h"
#include "integrations/frameio_url_parser.h"
#include "integrations/frameio_client.h"
#include "integrations/frameio_converter.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "timeline/timeline_cache.h"
#include "timeline/timeline_thumbnail_cache.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_selection.h"
#include "timeline/media_linker.h"
#include "audio/audio_player.h"
#include "audio/audio_mixer.h"
#include "audio/audio_decoder.h"
#include "transcode/transcode_settings.h"
#include "hdr/hdr_color_utils.h"
#ifdef _WIN32
#include "hdr/hdr_output_manager.h"
#include "gpu/d3d11_video_interop.h"
#endif
#include "annotations/annotation_serializer.h"
#include "annotations/annotated_thumbnail.h"
#include "annotations/stroke_smoother.h"
#include "app/shortcut_manager.h"

// Externs for globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_italic;
extern ImFont* font_mono;
extern ImFont* font_icons;
extern float g_font_scale;
extern bool use_windows_accent_color;
extern int custom_accent_color_index;
extern std::filesystem::path g_exe_dir;
extern std::unique_ptr<OCIOConfigManager> ocio_manager;
extern CacheSettings cache_settings;
extern TranscodeSettings transcode_settings;
extern bool cache_enabled;
extern bool otio_timeline_mode;
extern bool otio_dual_view_mode;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern std::unique_ptr<qcview::MediaLinker> media_linker;
extern std::unique_ptr<qcview::TimelinePlaybackController> scratch_timeline_controller;
extern std::unique_ptr<qcview::TimelineCommandManager> timeline_command_manager;
extern std::unique_ptr<qcview::TimelineThumbnailCache> timeline_thumbnail_cache;
extern std::unique_ptr<qcview::SystemPressureMonitor> pressure_monitor;
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
extern bool auto_play_buffering;
extern std::chrono::steady_clock::time_point auto_play_buffer_start;
extern bool pending_seek_cache_start;
extern std::chrono::steady_clock::time_point seek_cache_start_timer;
extern bool show_transcode_progress;
extern std::atomic<int> transcode_current_frame;
extern std::atomic<int> transcode_total_frames;
extern std::string transcode_status_message;
extern std::mutex transcode_status_mutex;

// Free functions defined in main.cpp
void AutoConfigureEXRThreading(CacheSettings& settings);
qcview::DirectEXRCacheConfig GetCurrentEXRCacheConfig();
qcview::TimelineCacheConfig GetCurrentTimelineCacheConfig();
qcview::ThumbnailConfig GetCurrentThumbnailConfig();
std::string GetAssetPath(const std::string& relative_path);

// Accent color helpers (declared in app_style.cpp)
ImVec4 GetWindowsAccentColor();
ImVec4 GetDefaultAccentColor();

// More globals from main.cpp
extern std::string current_timeline_path;
extern std::string current_timeline_id;
extern qcview::ProjectManager* g_project_manager;
extern bool auto_relink_in_progress;
extern bool show_auto_relink_progress;
extern int auto_relink_current;
extern int auto_relink_total;
extern std::string auto_relink_status;
extern std::string auto_relink_timeline_id;
extern qcview::LinkSummary last_link_summary;
extern bool show_auto_relink_results;
extern bool show_link_media_popup;
extern bool timeline_thumbnail_cache_clear_deferred;
extern bool mouse_is_idle;
extern ImVec2 last_mouse_pos;
extern std::chrono::steady_clock::time_point last_mouse_move_time;

// Free functions from main.cpp
void SaveLinksToCache(const std::string& timeline_id = "");
int RestoreLinksFromCache(const std::string& timeline_id = "");

    // ------------------------------------------------------------------------
    // CORE LIFECYCLE METHODS
    // ------------------------------------------------------------------------
bool Application::Initialize(const std::vector<std::string>& initial_files) {
#ifdef _WIN32
        // Set DPI awareness for accurate window positioning on Windows
        SetProcessDPIAware();
#endif

        // Load user settings BEFORE creating window
        LoadSettings();

        // Initialize customizable keyboard shortcuts
        InitShortcutDefaults();
        LoadShortcuts();

#ifdef QCVIEW_USE_METAL
        // Native macOS window — no GLFW
        {
            int wx = (has_saved_window_settings && saved_window_x >= 0) ? saved_window_x : -1;
            int wy = (has_saved_window_settings && saved_window_y >= 0) ? saved_window_y : -1;
            if (!MacOS_CreateWindow(saved_window_width, saved_window_height, wx, wy, "QCView v1.1.6")) {
                std::cerr << "Failed to create native macOS window" << std::endl;
                return false;
            }
            // Store opaque window pointer for code that needs it
            window = MacOS_GetNSWindow();
        }

        SetupDragDrop();
#else
        // GLFW window (Linux/Windows)
#ifdef __APPLE__
        // Suppress GLFW's default macOS menu bar — we build our own NSMenu
        glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_FALSE);
#endif

        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW" << std::endl;
            return false;
        }

#ifdef QCVIEW_USE_VULKAN
        // Vulkan manages its own surface — no OpenGL context
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
        // OpenGL context configuration
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        // 10-bit framebuffer for HDR output (10-10-10-2 format)
        glfwWindowHint(GLFW_RED_BITS, 10);
        glfwWindowHint(GLFW_GREEN_BITS, 10);
        glfwWindowHint(GLFW_BLUE_BITS, 10);
        glfwWindowHint(GLFW_ALPHA_BITS, 2);

        // Stencil buffer required for NanoVG anti-aliased annotation rendering
        glfwWindowHint(GLFW_STENCIL_BITS, 8);
#endif

        // Set app ID for Wayland and X11 WM_CLASS so desktop entry matches
#ifdef __linux__
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "app.qcview.QCView");
        glfwWindowHintString(GLFW_X11_CLASS_NAME, "qcview");
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "qcview");
#endif

        // Create window (use saved size if available)
        window = glfwCreateWindow(saved_window_width, saved_window_height, "QCView v1.1.6", nullptr, nullptr);
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
        std::wstring icon_path = (g_exe_dir / "assets/icons/qcview.ico").wstring();
        HICON hIcon = (HICON)LoadImageW(NULL, icon_path.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        }

        // Register window class for single-instance communication
        SetupSingleInstanceMessaging(hwnd);
#elif defined(__linux__)
        // Set window icon on Linux using stb_image + GLFW
        {
            std::string icon_path = GetAssetPath("assets/icons/qcview_256.png");
            int w, h, channels;
            unsigned char* pixels = stbi_load(icon_path.c_str(), &w, &h, &channels, 4);
            if (pixels) {
                GLFWimage icon;
                icon.width = w;
                icon.height = h;
                icon.pixels = pixels;
                glfwSetWindowIcon(window, 1, &icon);
                stbi_image_free(pixels);
            }
        }
#elif defined(__APPLE__)
        // macOS icon is set via Info.plist (CFBundleIconFile)
        // No need for glfwSetWindowIcon on macOS
#endif

        SetupDragDrop();

#ifdef _WIN32
        EnableDarkModeWindow(window);
#endif
#endif // QCVIEW_USE_METAL (end of GLFW window creation path)

#ifdef QCVIEW_USE_VULKAN
        // ============================================================
        // Vulkan initialization path (Linux)
        // ============================================================

        // Initialize Vulkan device manager
        if (!qcview::VulkanDeviceManager::Instance().Initialize()) {
            std::cerr << "Failed to initialize Vulkan device" << std::endl;
            glfwTerminate();
            return false;
        }
        Debug::Log("Vulkan device initialized: " +
                   qcview::VulkanDeviceManager::Instance().GetDeviceName());

        // Initialize Vulkan texture pool for frame display
        if (!qcview::VulkanTexturePool::Instance().Initialize()) {
            Debug::Log("WARNING: Failed to initialize VulkanTexturePool");
        } else {
            Debug::Log("VulkanTexturePool initialized (max " +
                       std::to_string(qcview::VulkanTexturePool::Instance().GetMaxMemory() / (1024*1024)) + " MB)");
        }

        // Initialize Vulkan thumbnail texture pool (separate pool for timeline thumbnails)
        if (!qcview::VulkanTexturePool::ThumbnailInstance().Initialize(512ULL * 1024 * 1024)) {
            Debug::Log("WARNING: Failed to initialize VulkanTexturePool (thumbnails)");
        } else {
            Debug::Log("VulkanTexturePool (thumbnails) initialized (max " +
                       std::to_string(qcview::VulkanTexturePool::ThumbnailInstance().GetMaxMemory() / (1024*1024)) + " MB)");
        }

        // Initialize Vulkan annotation renderer (replaces NanoVG GL3 backend)
        vulkan_annotation_renderer_ = std::make_unique<qcview::Annotations::VulkanAnnotationRenderer>();
        if (vulkan_annotation_renderer_->Initialize()) {
            Debug::Log("VulkanAnnotationRenderer: Initialized");
        } else {
            Debug::Log("WARNING: Failed to initialize VulkanAnnotationRenderer");
            vulkan_annotation_renderer_.reset();
        }

#elif defined(QCVIEW_USE_METAL)
        // ============================================================
        // Metal initialization path (macOS)
        // ============================================================

        // Initialize Metal device manager (uses MTKView's device and CAMetalLayer)
        if (!qcview::MetalDeviceManager::Instance().Initialize(MacOS_GetNSView())) {
            std::cerr << "Failed to initialize Metal device" << std::endl;
            return false;
        }
        Debug::Log("Metal device initialized: " +
                   qcview::MetalDeviceManager::Instance().GetDeviceName());

        // Initialize Metal texture pools — separate pools for frames and thumbnails
        if (!qcview::MetalTexturePool::Instance().Initialize()) {
            Debug::Log("WARNING: Failed to initialize MetalTexturePool");
        } else {
            Debug::Log("MetalTexturePool initialized (max " +
                       std::to_string(qcview::MetalTexturePool::Instance().GetMaxMemory() / (1024*1024)) + " MB)");
        }
        if (!qcview::MetalTexturePool::ThumbnailInstance().Initialize(512ULL * 1024 * 1024)) {
            Debug::Log("WARNING: Failed to initialize MetalTexturePool (thumbnails)");
        } else {
            Debug::Log("MetalTexturePool (thumbnails) initialized (max " +
                       std::to_string(qcview::MetalTexturePool::ThumbnailInstance().GetMaxMemory() / (1024*1024)) + " MB)");
        }

        // Initialize Metal annotation renderer (replaces NanoVG GL3 backend)
        metal_annotation_renderer_ = std::make_unique<qcview::Annotations::MetalAnnotationRenderer>();
        if (metal_annotation_renderer_->Initialize()) {
            Debug::Log("MetalAnnotationRenderer: Initialized");
        } else {
            Debug::Log("WARNING: Failed to initialize MetalAnnotationRenderer");
            metal_annotation_renderer_.reset();
        }

        // Initialize native macOS menu bar (replaces GLFW default + File/Edit/View/Help ImGui menus)
        qcview::InitNativeMenuBar();

        // Populate native Recent Files menu with persisted list from settings
        if (!recent_files.empty()) {
            qcview::UpdateNativeRecentFiles(recent_files);
        }

#else
        // ============================================================
        // OpenGL initialization path (Windows)
        // ============================================================

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // vsync 0,1,2

        // Initialize GLAD
        if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
            std::cerr << "Failed to initialize GLAD" << std::endl;
            return false;
        }

#ifdef _WIN32
        // Initialize D3D11 HDR swapchain for HDR output support
        {
            int fb_width, fb_height;
            glfwGetFramebufferSize(window, &fb_width, &fb_height);
            HWND hwnd = glfwGetWin32Window(window);
            if (qcview::HDROutputManager::Instance().Initialize(hwnd, fb_width, fb_height)) {
                if (qcview::HDROutputManager::Instance().IsHDRActive()) {
                    Debug::Log("HDR output enabled via D3D11 swapchain");
                } else {
                    Debug::Log("D3D11 swapchain initialized (SDR mode)");
                }
            } else {
                Debug::Log("D3D11 HDR swapchain initialization failed - using OpenGL presentation");
            }
        }
#endif

        // Initialize NanoVG for annotation rendering
        if (!qcview::Annotations::NanoVGContext::Instance().Initialize()) {
            Debug::Log("WARNING: Failed to initialize NanoVG context for annotations");
        } else {
            Debug::Log("NanoVG context initialized successfully");
        }
#endif // QCVIEW_USE_VULKAN

        // Setup ImGui and OCIO
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        // Change docking tooltip since undocking is disabled
        static const ImGuiLocEntry loc_entries[] = {
            { ImGuiLocKey_DockingDragToUndockOrMoveNode, "Click and drag to reorder tabs" },
        };
        ImGui::LocalizeRegisterEntries(loc_entries, IM_ARRAYSIZE(loc_entries));

        ImPlot::CreateContext();
        ImNodes::CreateContext();
        NodeEditorTheme::ApplyDarkTheme();
        ocio_manager = std::make_unique<OCIOConfigManager>();
        ImGuiIO& io = ImGui::GetIO();

        // Disable automatic .ini file - we'll save layout manually to settings.qcv
        io.IniFilename = nullptr;
        Debug::Log("ImGui layout will be saved to settings.qcv (not imgui.ini)");

        // Load saved ImGui layout if we have one
        if (!saved_imgui_layout.empty()) {
            ImGui::LoadIniSettingsFromMemory(saved_imgui_layout.c_str(), saved_imgui_layout.size());
            Debug::Log("Loaded ImGui layout from settings");
        }

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#if !defined(QCVIEW_USE_VULKAN) && !defined(QCVIEW_USE_METAL)
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable multi-viewport (requires GL/D3D11)
#endif

        // Setup ImGui style
        LoadCustomFonts();
        SetupImGuiStyle();

        // Apply UI scale (loaded from settings, default 1.0)
        // User adjusts via View > UI Scale. Scales fonts, ImGui style, and all S() values.
        {
            extern float g_ui_scale;
            if (g_ui_scale != 1.0f) {
                ImGui::GetStyle().ScaleAllSizes(g_ui_scale);
            }
            io.FontGlobalScale = g_ui_scale;
            Debug::Log("UI scale: " + std::to_string(g_ui_scale) + "x");
        }

        // Setup Platform/Renderer backends
#ifdef QCVIEW_USE_METAL
        {
            // Metal ImGui init is done from a .mm helper (ObjC types can't appear in .cpp)
            extern bool InitMetalImGui(void* window, void* device);
            extern void SetMetalLayerForImGui(void* metal_layer);
            auto& mgr = qcview::MetalDeviceManager::Instance();
            // ImGui_ImplOSX_Init called from .mm bridge (ObjC types)
            extern bool InitImGuiOSXBackend(void* ns_view);
            InitImGuiOSXBackend(MacOS_GetNSView());
            if (!InitMetalImGui(MacOS_GetNSView(), mgr.GetDevice())) {
                std::cerr << "Failed to initialize ImGui Metal backend" << std::endl;
                return false;
            }

            // Set layer for ImGui NewFrame (needs valid texture for sampleCount)
            SetMetalLayerForImGui(MacOS_GetMetalLayer());

            // Initialize Metal HDR swapchain
            metal_swapchain_ = std::make_unique<qcview::MetalHDRSwapchain>();
            metal_swapchain_->Initialize(MacOS_GetNSWindow());

            // Register with cross-platform HDR color query system
            qcview::SetMetalHDRSwapchain(metal_swapchain_.get());

            // EDR UI scale defaults to 1.0 (SDR white); no nits mapping needed

            // Apply saved HDR preference if supported
            if (hdr_preferred_ && metal_swapchain_->IsHDRSupported()) {
                metal_swapchain_->SetHDREnabled(true);
                Debug::Log("Metal EDR: restored from saved preference");
            }

            Debug::Log("ImGui Metal backend initialized");
        }
#elif defined(QCVIEW_USE_VULKAN)
        {
            auto& vk = qcview::VulkanDeviceManager::Instance();

            // Initialize ImGui for Vulkan
            ImGui_ImplGlfw_InitForVulkan(window, true);

            // Create HDR-capable swapchain (manages surface, images, render pass, sync)
            vulkan_swapchain_ = std::make_unique<qcview::VulkanHDRSwapchain>();
            if (!vulkan_swapchain_->Initialize(window)) {
                std::cerr << "Failed to initialize Vulkan swapchain" << std::endl;
                return false;
            }

            // Register the swapchain for cross-platform HDR color queries
            qcview::SetVulkanHDRSwapchain(vulkan_swapchain_.get());

            // Initialize ImGui Vulkan backend
            ImGui_ImplVulkan_InitInfo init_info{};
            init_info.ApiVersion = VK_API_VERSION_1_2;
            init_info.Instance = vk.GetInstance();
            init_info.PhysicalDevice = vk.GetPhysicalDevice();
            init_info.Device = vk.GetDevice();
            init_info.QueueFamily = vk.GetGraphicsQueueFamily();
            init_info.Queue = vk.GetGraphicsQueue();
            init_info.PipelineCache = vk.GetPipelineCache();
            init_info.DescriptorPool = vk.GetImGuiDescriptorPool();
            init_info.MinImageCount = 2;
            init_info.ImageCount = vulkan_swapchain_->GetImageCount();
            init_info.PipelineInfoMain.RenderPass = vulkan_swapchain_->GetRenderPass();
            init_info.PipelineInfoMain.Subpass = 0;
            init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

            // Use HDR fragment shader (with PQ conversion support via push constants)
            if (vulkan_swapchain_->HasHDRFragShader()) {
                const auto& spirv = vulkan_swapchain_->GetHDRFragShaderSPIRV();
                init_info.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                init_info.CustomShaderFragCreateInfo.codeSize = spirv.size() * sizeof(uint32_t);
                init_info.CustomShaderFragCreateInfo.pCode = spirv.data();
                Debug::Log("ImGui using HDR fragment shader");
            }

            ImGui_ImplVulkan_Init(&init_info);

            Debug::Log("ImGui Vulkan backend initialized");
            Debug::Log("Swapchain: " + std::to_string(vulkan_swapchain_->GetExtent().width) + "x" +
                       std::to_string(vulkan_swapchain_->GetExtent().height) + " (" +
                       std::to_string(vulkan_swapchain_->GetImageCount()) + " images" +
                       (vulkan_swapchain_->IsHDRSupported() ? ", HDR supported" : "") + ")");

            // Apply saved HDR UI brightness and preference
            vulkan_swapchain_->SetUIBrightness(hdr_ui_nits_);
            if (hdr_preferred_ && vulkan_swapchain_->IsHDRSupported()) {
                if (vulkan_swapchain_->SetHDREnabled(true)) {
                    Debug::Log("HDR enabled from saved preference");
                }
            }
        }
#else
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 450");

#ifdef _WIN32
        // Apply saved HDR UI brightness
        qcview::SetHDRUIBrightness(hdr_ui_nits_);

        // Enable HDR mode in ImGui shader if HDR output is active
        if (qcview::HDROutputManager::Instance().IsHDRActive()) {
            ImGui_ImplOpenGL3_SetHDRMode(true, hdr_ui_nits_);
            Debug::Log("ImGui shader HDR mode enabled (sRGB->PQ conversion at " +
                       std::to_string((int)hdr_ui_nits_) + " nits)");
        }
#endif
#endif // QCVIEW_USE_VULKAN

        // Initialize NFD (Native File Dialog) - required for file dialogs to work properly
        if (NFD_Init() != NFD_OKAY) {
            Debug::Log("WARNING: NFD_Init failed - file dialogs may not work correctly");
        } else {
            Debug::Log("NFD initialized successfully");
        }

        // Initialize video player
        video_player = std::make_unique<VideoPlayer>();
        if (!video_player->Initialize()) {
            std::cerr << "Failed to initialize video player" << std::endl;
            return false;
        }

        // Apply disk cache settings from loaded config (custom path, retention, etc.)
        video_player->SetCacheSettings(g_custom_cache_path, g_cache_retention_days,
                                       g_transcode_cache_max_gb, g_clear_cache_on_exit);
        Debug::Log("Applied initial disk cache settings from config");

        // Initialize OTIO timeline view (default view for all media)
        timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
        timeline_view->SetProjectManager(project_manager.get());  // Enable playlist media resolution
        Debug::Log("TimelineView created at startup (OTIO timeline is default view)");

        // Initialize project manager after video player
        project_manager = std::make_unique<qcview::ProjectManager>(
            video_player.get(),  // VideoPlayer*
            &current_file_path,  // std::string*
            &show_inspector_panel, // bool*
            cache_enabled        // bool - user's cache preference
        );

        // Set global pointer for static helper functions
        g_project_manager = project_manager.get();

        project_manager->SetVideoChangeCallback([this](const std::string& file_path) {
            this->OnVideoChanged(file_path);
            });

        // Set up pre-video change callback - caches playhead position BEFORE loading new media
        project_manager->SetPreVideoChangeCallback([this](const std::string& old_path) {
            double playhead = video_player ? video_player->GetPosition() : 0.0;
            Debug::Log("PreVideoChangeCallback: Caching playhead for: " + old_path + " at " + std::to_string(playhead));
            project_manager->CacheCurrentViewState(old_path, 1.0f, 0.0f, playhead);  // Dummy zoom/pan values
        });

        // Set up view state callback - restores playhead when loading media from project
        project_manager->SetViewStateCallback([this](float /*zoom*/, float /*scroll*/, double playhead) {
            // Restore playhead position (seek to saved time)
            if (video_player && playhead > 0.0) {
                video_player->Seek(playhead);
            }
            Debug::Log("ViewStateCallback: Restored playhead=" + std::to_string(playhead));
        });

        // Set up color preset callback for auto 1-2-1 detection
        project_manager->SetColorPresetCallback([this](const std::string& nclc_tag) {
#ifdef _WIN32
            // Skip auto 1-2-1 in HDR mode - color conversion handled by HDR pipeline
            if (qcview::HDROutputManager::Instance().IsHDRActive()) {
                Debug::Log("Auto 1-2-1: Skipped (HDR mode active)");
                return;
            }
#endif
            if (cache_settings.auto_121_enabled && nclc_tag == "1-2-1") {
                Debug::Log("Auto 1-2-1: Applying Rec.709 -> sRGB preset");
                ApplyPreset(R"({
                    "name": "Rec.709 to sRGB Standard",
                    "ocio_config": "Blender",
                    "nodes": [
                        {"type": "INPUT_COLORSPACE", "data": "Rec.1886", "position": [100, 100]},
                        {"type": "OUTPUT_DISPLAY", "display": "sRGB", "view": "Standard", "position": [400, 100]}
                    ],
                    "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
                })");
            }
        });

        // Set up auto-play request callback - respects app-wide auto_play_on_load setting
        project_manager->SetAutoPlayRequestCallback([this]() {
            TriggerAutoPlay(qcview::MediaType::VIDEO);  // Use VIDEO type for playlists (not image sequences)
        });

        // Set up recent file callback - adds loaded files to File > Recent Files
        project_manager->SetRecentFileCallback([this](const std::string& path) {
            AddToRecentFiles(path);
        });

        // Set up loading modal callback - shows overlay before blocking operations
        project_manager->SetLoadingModalCallback([this](const std::string& message, std::function<void()> callback) {
            ScheduleLoadingOperation(message, callback);
        });

        // Set up timeline editor callback - opens timeline in OTIO editor mode
        // Can receive either:
        // - A file path to an EDL/OTIO file (for imported timelines)
        // - A timeline_id (for scratch/new timelines where path is empty)
        project_manager->SetTimelineEditorCallback([this](const std::string& path_or_id) {
            Debug::Log("Opening timeline in editor: " + path_or_id);

            // ========================================================================
            // RESOURCE CLEANUP - Stop other playback modes before entering timeline
            // ========================================================================

            // 1. Stop video playback and release resources
            if (video_player) {
                if (video_player->IsPlaying()) {
                    video_player->Pause();
                    Debug::Log("Timeline entry: Paused video playback");
                }

                // 2. Clean up EXR/image sequence cache if active
                // Must use ClearEXRCache() to properly shutdown DirectEXRCache threads
                if (video_player->IsInEXRMode()) {
                    video_player->ClearEXRCache();
                    Debug::Log("Timeline entry: Cleaned up EXR/image sequence cache");
                }

            }

            // 4. Clear current file path (timeline uses its own playback system)
            if (!current_file_path.empty()) {
                Debug::Log("Timeline entry: Clearing current file path: " + current_file_path);
                current_file_path.clear();
            }

            // ========================================================================

            // IMPORTANT: Store edits from current timeline BEFORE switching to new one
            // This preserves edits when switching between timelines
            // BUT: Skip caching if we're viewing a nested timeline - that's transient state
            if (timeline_view && !timeline_view->IsViewingNestedTimeline()) {
                // Try to find current timeline by path first, then by ID (for scratch timelines)
                std::string lookup_key = !current_timeline_path.empty() ? current_timeline_path : current_timeline_id;
                if (!lookup_key.empty()) {
                    qcview::MediaItem* current_item = project_manager->GetTimelineItem(lookup_key);
                    if (current_item) {
                        current_item->cached_tracks = timeline_view->GetTracks();
                        current_item->has_cached_edits = true;
                        // Also save duration (may have changed due to edits extending timeline)
                        current_item->duration = timeline_view->GetDuration();
                        Debug::Log("Stored " + std::to_string(current_item->cached_tracks.size()) +
                                   " edited tracks before switching from: " + lookup_key +
                                   " (duration=" + std::to_string(current_item->duration) + "s)");
                    }
                }
            } else if (timeline_view && timeline_view->IsViewingNestedTimeline()) {
                Debug::Log("Skipping cache - currently viewing nested timeline");
            }

            // Create/update TimelineView if needed
            if (!timeline_view) {
                timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
            }

            // Check if this is a scratch timeline (by looking up the timeline_id)
            qcview::MediaItem* timeline_item = project_manager->GetTimelineItem(path_or_id);
            bool is_scratch_timeline = (timeline_item && timeline_item->timeline_format == "scratch");

            if (is_scratch_timeline) {
                // Scratch timeline - initialize with stored dimensions
                Debug::Log("Opening scratch timeline: " + timeline_item->name);

                // Create/reset TimelineView for UI rendering (required even for scratch timelines)
                if (!timeline_view) {
                    timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
                } else {
                    // Destroy thumbnail cache before playback shutdown
                    timeline_thumbnail_cache.reset();  // Full destroy — stops worker threads
                    // Shutdown existing playback from previous timeline (file-based or scratch)
                    timeline_view->ShutdownPlayback();
                }

                // Cleanup previous scratch timeline controller if exists
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Shutdown();
                    scratch_timeline_controller.reset();
                }

                // Initialize as empty scratch timeline with stored properties
                timeline_view->InitializeForScratch(
                    timeline_item->name,
                    timeline_item->duration,
                    timeline_item->frame_rate,
                    timeline_item->timeline_width,
                    timeline_item->timeline_height
                );

                // Track whether we need to restore cached tracks (done after playback init)
                bool has_cached_tracks_to_restore = timeline_item->has_cached_edits && !timeline_item->cached_tracks.empty();

                // Switch to timeline editor mode
                otio_timeline_mode = true;
                current_timeline_path = "";  // No file for scratch timelines
                current_timeline_id = timeline_item->timeline_id;  // Track by ID for scratch timelines

                // Initialize command manager for editing if needed
                if (!timeline_command_manager) {
                    timeline_command_manager = std::make_unique<qcview::TimelineCommandManager>();
                }

                // Initialize timeline thumbnail cache for trim/slip preview
                if (!timeline_thumbnail_cache) {
                    timeline_thumbnail_cache = std::make_unique<qcview::TimelineThumbnailCache>();
                    timeline_thumbnail_cache->Initialize(timeline_view ? timeline_view->GetFrameRate() : 24.0);
                }

                // Initialize scratch timeline playback using virtual timeline (no dummy video)
                if (video_player) {
                    // Create new controller (stored in unique_ptr for lifetime management)
                    scratch_timeline_controller = std::make_unique<qcview::TimelinePlaybackController>();

                    // Set pipeline mode from project settings BEFORE initialization
                    qcview::TimelinePlaybackConfig config = scratch_timeline_controller->GetConfig();
                    config.pipeline_mode = project_manager ? project_manager->GetProjectPipelineMode() : PipelineMode::NORMAL;
                    scratch_timeline_controller->SetConfig(config);
                    Debug::Log("Scratch timeline: Set pipeline mode to " + std::string(PipelineModeToString(config.pipeline_mode)));

                    if (scratch_timeline_controller->InitializeForVirtualScratchTimeline(
                            video_player.get(),
                            timeline_item->timeline_width,
                            timeline_item->timeline_height,
                            timeline_item->frame_rate)) {
                        Debug::Log("Virtual scratch timeline playback initialized: " +
                                   std::to_string(timeline_item->timeline_width) + "x" +
                                   std::to_string(timeline_item->timeline_height) + " @ " +
                                   std::to_string(timeline_item->frame_rate) + "fps");
                        video_player->SetTimelineMode(true, scratch_timeline_controller.get());

                        // Connect the external controller to TimelineView so that
                        // SyncFlattenerAndInvalidate() can update the cache when clips are added
                        timeline_view->SetExternalPlaybackController(scratch_timeline_controller.get());

                        // Connect thumbnail cache for playback state notifications
                        if (timeline_thumbnail_cache) {
                            scratch_timeline_controller->SetThumbnailCache(timeline_thumbnail_cache.get());
                        }

                        // Apply user settings for buffer wait and adaptive speed
                    } else {
                        Debug::Log("Failed to initialize virtual scratch timeline playback");
                        scratch_timeline_controller.reset();
                        // Still enable timeline mode for UI (gap texture will be created)
                        video_player->SetTimelineMode(true, nullptr);
                        timeline_view->SetExternalPlaybackController(nullptr);
                    }
                } else {
                    // No video player - still enable timeline mode for UI
                    video_player->SetTimelineMode(true, nullptr);
                    timeline_view->SetExternalPlaybackController(nullptr);
                }

                // NOW restore cached tracks (after playback controller is connected)
                // This ensures SyncFlattenerAndInvalidate() can initialize the cache
                if (has_cached_tracks_to_restore) {
                    auto& tracks = timeline_view->GetTracks();
                    tracks = timeline_item->cached_tracks;
                    timeline_view->RecalculateDuration();
                    timeline_view->SyncFlattenerAndInvalidate();
                    Debug::Log("Restored " + std::to_string(timeline_item->cached_tracks.size()) +
                               " cached tracks for scratch timeline: " + timeline_item->name);
                }

                // Request auto-fit zoom on next render (when we know the actual visible width)
                timeline_view->RequestFitZoomOnNextRender();

                // Update annotation availability for scratch timeline
                if (annotation_manager && annotation_panel) {
                    if (project_manager && !project_manager->GetProjectPath().empty()) {
                        annotation_panel->SetAvailability(qcview::AnnotationAvailability::AVAILABLE);
                        std::string annotation_path = project_manager->GetAnnotationPathForMedia(timeline_item ? timeline_item->path : "");
                        if (!annotation_path.empty()) {
                            annotation_manager->LoadNotesForMedia(annotation_path);
                            Debug::Log("Loaded scratch timeline annotations for: " + annotation_path);
                        } else {
                            annotation_manager->ClearNotes();
                        }
                    } else {
                        annotation_panel->SetAvailability(qcview::AnnotationAvailability::NO_PROJECT_SAVED);
                        annotation_manager->ClearNotes();
                        Debug::Log("Annotations disabled for scratch timeline - project not saved");
                    }
                }
            } else {
                // File-based timeline (EDL/OTIO)
                std::string file_path = path_or_id;

                // If path_or_id is a timeline_id, get the actual file path
                if (timeline_item && !timeline_item->path.empty()) {
                    file_path = timeline_item->path;
                }

                // Cleanup previous scratch timeline controller if exists
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Shutdown();
                    scratch_timeline_controller.reset();
                }
                // Clear external controller reference (file-based timelines use internal controller)
                timeline_view->SetExternalPlaybackController(nullptr);

                // Check if we have cached edits for this timeline
                // If so, use them instead of re-parsing the EDL/OTIO file
                bool loaded = false;
                bool used_cached_edits = false;

                if (timeline_item && timeline_item->has_cached_edits && !timeline_item->cached_tracks.empty()) {
                    // Restore from cached edits with full timeline properties
                    timeline_view->SetTracks(
                        timeline_item->cached_tracks,
                        timeline_item->frame_rate,
                        timeline_item->name,
                        timeline_item->path
                    );
                    loaded = true;
                    used_cached_edits = true;
                    Debug::Log("Restored timeline from cached edits (" +
                               std::to_string(timeline_item->cached_tracks.size()) + " tracks, " +
                               std::to_string(timeline_item->frame_rate) + " fps)");

                    // Re-probe clips that are missing source_duration
                    // Needed for tape timecode detection in TimelineToSource
                    auto& tracks = timeline_view->GetTracks();
                    bool needs_sync = false;
                    for (auto& track : tracks) {
                        for (auto& clip : track.clips) {
                            if (clip.is_linked && !clip.linked_path.empty() && clip.source_duration <= 0) {
                                // Re-probe the file
                                if (qcview::MediaLinker::IsVideoFile(clip.linked_path)) {
                                    qcview::VideoProbeResult probe = qcview::MediaLinker::ProbeVideoFile(clip.linked_path);
                                    if (probe.valid) {
                                        clip.source_duration = probe.duration;
                                        clip.source_fps = probe.fps;
                                        clip.source_width = probe.width;
                                        clip.source_height = probe.height;
                                        clip.has_audio = probe.has_audio;
                                        needs_sync = true;
                                        Debug::Log("Re-probed clip '" + clip.name + "': duration=" +
                                                   std::to_string(probe.duration) + "s, has_audio=" +
                                                   (probe.has_audio ? "yes" : "no"));
                                    }
                                }
                            }
                        }
                    }
                    if (needs_sync) {
                        timeline_view->SyncFlattenerAndInvalidate();
                    }
                } else {
                    Debug::Log("Cannot load timeline from file - create a scratch timeline instead.");
                }

                if (loaded) {
                    // Switch to timeline editor mode
                    otio_timeline_mode = true;
                    current_timeline_path = file_path;
                    current_timeline_id = timeline_item ? timeline_item->timeline_id : "";
                    Debug::Log("Timeline loaded, switched to editor mode" +
                               std::string(used_cached_edits ? " (from cache)" : " (from file)"));

                    // Set canvas dimensions from timeline settings (for multi-resolution support)
                    // This must happen before InitializePlayback() to ensure consistent output dimensions
                    if (timeline_view && timeline_item) {
                        timeline_view->SetCanvasDimensions(timeline_item->timeline_width, timeline_item->timeline_height);
                        Debug::Log("Timeline canvas dimensions: " +
                                   std::to_string(timeline_item->timeline_width) + "x" +
                                   std::to_string(timeline_item->timeline_height));
                    }

                    // Restore timeline view state (zoom, scroll, playhead, in/out points)
                    if (timeline_view && project_manager) {
                        std::string lookup_key = !file_path.empty() ? file_path : (timeline_item ? timeline_item->timeline_id : "");
                        float tz = 50.0f, ts = 0.0f;
                        double tp = 0.0, ti = -1.0, to = -1.0;
                        if (project_manager->GetTimelineViewState(lookup_key, tz, ts, tp, ti, to)) {
                            timeline_view->SetZoomLevel(tz);
                            timeline_view->SetScrollOffset(ts);
                            if (ti >= 0) timeline_view->SetTimelineInPoint(ti);
                            if (to >= 0) timeline_view->SetTimelineOutPoint(to);
                            // Seek to restored playhead position after playback is initialized
                            if (tp > 0.0 && timeline_view->HasPlaybackController()) {
                                auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();
                                if (playback_ctrl) {
                                    playback_ctrl->Seek(tp);
                                }
                            }
                            Debug::Log("Restored timeline view state: zoom=" + std::to_string(tz) +
                                       ", scroll=" + std::to_string(ts) +
                                       ", playhead=" + std::to_string(tp));
                        } else {
                            // No saved state - request auto-fit zoom on next render
                            // (when we know the actual visible width)
                            timeline_view->RequestFitZoomOnNextRender();
                        }
                    }

                    // Initialize command manager for editing if needed
                    if (!timeline_command_manager) {
                        timeline_command_manager = std::make_unique<qcview::TimelineCommandManager>();
                    }

                    // Initialize timeline thumbnail cache for trim/slip preview
                    if (!timeline_thumbnail_cache) {
                        timeline_thumbnail_cache = std::make_unique<qcview::TimelineThumbnailCache>();
                        timeline_thumbnail_cache->Initialize(timeline_view ? timeline_view->GetFrameRate() : 24.0);
                    }

                    // Get timeline_id for link caching
                    std::string timeline_id = timeline_item ? timeline_item->timeline_id : "";
                    auto_relink_timeline_id = timeline_id;

                    // Restore cached media links if available (from session cache or project file)
                    // Note: If we used cached edits, links are already in the tracks
                    int restored_links = used_cached_edits ? 1 : RestoreLinksFromCache(timeline_id);

                    // If no links were restored, trigger auto-relink from timeline source directory
                    if (restored_links == 0 && timeline_view) {
                        std::string search_dir = timeline_view->GetSourceDirectory();
                        if (!search_dir.empty()) {
                            Debug::Log("No cached links found - triggering auto-relink from: " + search_dir);

                            // Create media linker if needed
                            if (!media_linker) {
                                media_linker = std::make_unique<qcview::MediaLinker>();
                            }

                            // Set up progress callback
                            media_linker->SetProgressCallback([](int current, int total, const std::string& status) {
                                auto_relink_current = current;
                                auto_relink_total = total;
                                auto_relink_status = status;
                            });

                            auto_relink_in_progress = true;

                            // Perform linking with depth limit of 6
                            qcview::LinkOptions options;
                            options.recursive = true;
                            options.max_depth = 6;
                            options.fuzzy_match = true;

                            auto& tracks = timeline_view->GetTracks();
                            last_link_summary = media_linker->LinkMediaInDirectory(tracks, search_dir, options);

                            // Linking complete
                            auto_relink_in_progress = false;

                            // Update flattener and invalidate cache (media changed)
                            timeline_view->SyncFlattenerAndInvalidate();

                            // Save links to cache
                            SaveLinksToCache(timeline_id);

                            // Show results dialog
                            show_auto_relink_results = true;
                            restored_links = last_link_summary.linked_count;

                            Debug::Log("Auto-relink complete: " + std::to_string(last_link_summary.linked_count) +
                                       "/" + std::to_string(last_link_summary.total_clips) + " clips linked");
                        }
                    }

                    // If links were restored, initialize timeline playback immediately
                    if (restored_links > 0 && timeline_view && video_player) {
                        Debug::Log("Auto-initializing timeline playback after restoring " +
                                   std::to_string(restored_links) + " cached links");

                        if (!timeline_view->HasPlaybackController()) {
                            // Set pipeline mode from project settings BEFORE initialization
                            PipelineMode project_mode = project_manager ? project_manager->GetProjectPipelineMode() : PipelineMode::NORMAL;
                            timeline_view->SetPendingPipelineMode(project_mode);
                            Debug::Log("OTIO timeline: Set pending pipeline mode to " + std::string(PipelineModeToString(project_mode)));

                            if (timeline_view->InitializePlayback()) {
                                Debug::Log("Timeline playback initialized from cached links");
                                video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());

                                // Ensure timeline starts paused (don't auto-play when switching)
                                timeline_view->GetPlaybackController()->Pause();
                                timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                                // Apply audio sync settings
                                if (auto* mixer = timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                    const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                                    float latency_ms = (cache_settings.display_latency_preset == 5)
                                        ? cache_settings.custom_display_latency_ms
                                        : preset_latencies_ms[cache_settings.display_latency_preset];
                                    mixer->SetDisplayLatency(latency_ms / 1000.0);
                                    mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                                    // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                                    qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                                    if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                                        source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                                        mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                                    } else {
                                        mixer->SetPipelineLatency(0.0);
                                    }
                                }

                                // Ensure playback controller and cache have the correct duration
                                // This is the same update that happens in SyncFlattenerAndInvalidate()
                                // when the timeline is extended during editing - ensures the dummy
                                // video and cache progress bar use the correct extended duration
                                double duration = timeline_view->GetDuration();
                                timeline_view->GetPlaybackController()->UpdateDuration(duration);
                                if (timeline_view->GetPlaybackController()->GetCache()) {
                                    timeline_view->GetPlaybackController()->GetCache()->UpdateDuration(duration);
                                }
                            } else {
                                Debug::Log("Failed to initialize timeline playback from cached links");
                                // Still enable timeline mode even if playback init fails
                                // This ensures gap texture is created for UI consistency
                                video_player->SetTimelineMode(true, nullptr);
                            }
                        } else {
                            // Playback controller already exists - re-enable timeline mode
                            // (returning to timeline after playing solo video)
                            Debug::Log("Returning to existing timeline");
                            video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                        }
                    } else if (timeline_view && video_player) {
                        // No links restored - but imported AAF/XML/OTIO may have valid file_path values
                        // Try to initialize playback anyway (InitializePlayback checks for valid paths)
                        Debug::Log("No links restored - attempting playback init with imported paths");

                        if (!timeline_view->HasPlaybackController()) {
                            if (timeline_view->InitializePlayback()) {
                                Debug::Log("Timeline playback initialized from imported file paths");
                                video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());

                                // Ensure timeline starts paused (don't auto-play when switching)
                                timeline_view->GetPlaybackController()->Pause();
                                timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                                // Apply audio sync settings
                                if (auto* mixer = timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                    const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                                    float latency_ms = (cache_settings.display_latency_preset == 5)
                                        ? cache_settings.custom_display_latency_ms
                                        : preset_latencies_ms[cache_settings.display_latency_preset];
                                    mixer->SetDisplayLatency(latency_ms / 1000.0);
                                    mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                                    // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                                    qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                                    if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                                        source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                                        mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                                    } else {
                                        mixer->SetPipelineLatency(0.0);
                                    }
                                }

                                double duration = timeline_view->GetDuration();
                                timeline_view->GetPlaybackController()->UpdateDuration(duration);
                                if (timeline_view->GetPlaybackController()->GetCache()) {
                                    timeline_view->GetPlaybackController()->GetCache()->UpdateDuration(duration);
                                }
                            } else {
                                // No valid paths found - enable timeline mode for UI only
                                Debug::Log("No usable media paths - enabling timeline mode for UI only");
                                video_player->SetTimelineMode(true, nullptr);
                            }
                        } else {
                            video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                        }
                    }
                    // Update annotation availability for timeline
                    if (annotation_manager && annotation_panel) {
                        if (project_manager && !project_manager->GetProjectPath().empty()) {
                            annotation_panel->SetAvailability(qcview::AnnotationAvailability::AVAILABLE);
                            std::string annotation_path = project_manager->GetAnnotationPathForMedia(timeline_item ? timeline_item->path : file_path);
                            if (!annotation_path.empty()) {
                                annotation_manager->LoadNotesForMedia(annotation_path);
                                Debug::Log("Loaded timeline annotations for: " + annotation_path);
                            } else {
                                annotation_manager->ClearNotes();
                            }
                        } else {
                            annotation_panel->SetAvailability(qcview::AnnotationAvailability::NO_PROJECT_SAVED);
                            annotation_manager->ClearNotes();
                            Debug::Log("Annotations disabled for timeline - project not saved");
                        }
                    }
                } else {
                    Debug::Log("Failed to load timeline file");
                }
            }
        });

        // Set up exit timeline mode callback - exits timeline editor when loading non-timeline media
        // NOTE: This does NOT exit dual view mode - dual view handles its own video loading
        project_manager->SetExitTimelineModeCallback([this]() {
            Debug::Log("Exiting timeline editor mode (loading non-timeline media)");

            // Store edited tracks and view state to MediaItem before exiting
            // This preserves edits and view position when user switches away from timeline
            // Skip if viewing nested timeline - that's transient state
            if (timeline_view && !timeline_view->IsViewingNestedTimeline()) {
                std::string lookup_key = !current_timeline_path.empty() ? current_timeline_path : current_timeline_id;
                if (!lookup_key.empty()) {
                    // Try to find as regular timeline, dual view, or media item (image/EXR sequences)
                    qcview::MediaItem* timeline_item = project_manager->GetTimelineItem(lookup_key);
                    if (!timeline_item) {
                        timeline_item = project_manager->GetDualViewItem(lookup_key);
                    }
                    if (!timeline_item) {
                        timeline_item = project_manager->GetMediaItem(lookup_key);
                    }
                    if (timeline_item) {
                        timeline_item->cached_tracks = timeline_view->GetTracks();
                        timeline_item->has_cached_edits = true;
                        // Also save duration and frame_rate (may have changed due to edits)
                        timeline_item->duration = timeline_view->GetDuration();
                        timeline_item->frame_rate = timeline_view->GetFrameRate();
                        Debug::Log("Stored " + std::to_string(timeline_item->cached_tracks.size()) +
                                   " edited tracks for timeline/dual view: " + lookup_key +
                                   " (duration=" + std::to_string(timeline_item->duration) + "s, fps=" +
                                   std::to_string(timeline_item->frame_rate) + ")");
                    }

                    // Cache timeline view state (zoom, scroll, playhead, in/out points)
                    double timeline_playhead_time = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
                    project_manager->CacheTimelineViewState(
                        lookup_key,
                        timeline_view->GetZoomLevel(),
                        timeline_view->GetScrollOffset(),
                        timeline_playhead_time,
                        timeline_view->GetTimelineInPoint(),
                        timeline_view->GetTimelineOutPoint()
                    );
                }
            }

            // Clear current timeline tracking
            current_timeline_path.clear();
            current_timeline_id.clear();

            // Exit dual view mode if active (normal media loading should not be in dual view)
            otio_dual_view_mode = false;

            // Clear ACTIVE state from all dual views in project manager
            project_manager->ClearDualViewActiveStates();

            // Clean up the previous timeline's state (otio_timeline_mode stays true as default view)

            // Stop timeline audio mixer and clear external controller reference
            if (timeline_view) {
                // Stop audio mixer to prevent audio from previous timeline continuing
                if (auto* ctrl = timeline_view->GetPlaybackController()) {
                    if (auto* mixer = ctrl->GetAudioMixer()) {
                        mixer->Stop();
                    }
                }
                timeline_view->SetExternalPlaybackController(nullptr);
                Debug::Log("[MEDIA_SWITCH] ExitTimeline: destroying thumbnail cache...");
                timeline_thumbnail_cache.reset();  // Full destroy — stops worker threads
                Debug::Log("[MEDIA_SWITCH] ExitTimeline: thumbnail cache destroyed");
                Debug::Log("[MEDIA_SWITCH] ExitTimeline: shutting down playback...");
                timeline_view->ShutdownPlayback();
                Debug::Log("[MEDIA_SWITCH] ExitTimeline: playback shut down");
                timeline_view->ResetSourceMode();
            }

            // Shutdown scratch timeline controller (dual view, scratch timelines)
            if (scratch_timeline_controller) {
                Debug::Log("[MEDIA_SWITCH] ExitTimeline: shutting down scratch controller...");
                scratch_timeline_controller->Shutdown();
                scratch_timeline_controller.reset();
                Debug::Log("[MEDIA_SWITCH] ExitTimeline: scratch controller destroyed");
            }

            // Clear tracks
            if (timeline_view) {
                timeline_view->GetTracks().clear();
                timeline_view->GetFlattener().SetTracks({});
            }

            // Disable video player timeline mode (will be re-enabled when new timeline loads)
            if (video_player) {
                video_player->SetTimelineMode(false);
            }

            // Note: thumbnail cache already destroyed above via .reset()
            // Do NOT set deferred flag — it would destroy the newly-created cache on next frame
        });

        // Set up flush timeline edits callback - called before SaveProject() to capture current edits
        project_manager->SetFlushTimelineEditsCallback([this]() {
            // Store edited tracks to MediaItem before saving
            // This ensures slip/trim/cut edits are captured even without switching timelines
            // Skip if viewing nested timeline - that's transient state
            if (timeline_view && otio_timeline_mode && !timeline_view->IsViewingNestedTimeline()) {
                auto source_mode = timeline_view->GetSourceMode();

                // Handle IMAGE_SEQUENCE mode - save audio track edits to the source MediaItem
                if (source_mode == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                    qcview::MediaItem* source_item = timeline_view->GetSourceMediaItem();
                    if (source_item) {
                        source_item->cached_tracks = timeline_view->GetTracks();
                        source_item->has_cached_edits = true;
                        // Save extended timeline duration if audio clips extend past sequence
                        double timeline_dur = timeline_view->GetDuration();
                        if (timeline_dur > source_item->duration) {
                            source_item->cached_timeline_duration = timeline_dur;
                            Debug::Log("FlushTimelineEdits: Timeline extended to " +
                                       std::to_string(timeline_dur) + "s (sequence was " +
                                       std::to_string(source_item->duration) + "s)");
                        }
                        Debug::Log("FlushTimelineEdits: Captured " + std::to_string(source_item->cached_tracks.size()) +
                                   " tracks for IMAGE_SEQUENCE: " + source_item->name);
                    }
                    return;
                }

                // Handle TIMELINE and DUAL_VIEW modes
                std::string lookup_key = !current_timeline_path.empty() ? current_timeline_path : current_timeline_id;
                if (!lookup_key.empty()) {
                    // Try to find as regular timeline, dual view, or media item (image/EXR sequences)
                    qcview::MediaItem* timeline_item = project_manager->GetTimelineItem(lookup_key);
                    if (!timeline_item) {
                        timeline_item = project_manager->GetDualViewItem(lookup_key);
                    }
                    if (!timeline_item) {
                        timeline_item = project_manager->GetMediaItem(lookup_key);
                    }
                    if (timeline_item) {
                        timeline_item->cached_tracks = timeline_view->GetTracks();
                        timeline_item->has_cached_edits = true;
                        // Also save duration and frame_rate (may have changed due to edits)
                        timeline_item->duration = timeline_view->GetDuration();
                        timeline_item->frame_rate = timeline_view->GetFrameRate();
                        Debug::Log("FlushTimelineEdits: Captured " + std::to_string(timeline_item->cached_tracks.size()) +
                                   " tracks for timeline/dual view: " + lookup_key +
                                   " (duration=" + std::to_string(timeline_item->duration) + "s, fps=" +
                                   std::to_string(timeline_item->frame_rate) + ")");
                    }
                }
            }
        });

        // Set up image sequence timeline callback - loads image sequences via timeline view
        project_manager->SetImageSequenceTimelineCallback([this](qcview::MediaItem* item) {
            if (!item) {
                Debug::Log("ImageSequenceTimelineCallback: Invalid item");
                return;
            }

            // Create TimelineView if it doesn't exist (first time entering timeline mode)
            if (!timeline_view) {
                Debug::Log("ImageSequenceTimelineCallback: Creating TimelineView for first use");
                timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
            } else {
                // STOP playback first before any cleanup
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();
                }
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Pause();
                }

                // Shutdown existing playback to properly clean up previous decoder/cache
                // This is critical when switching from video (especially D3D11VA HDR) to image sequence
                Debug::Log("ImageSequenceTimelineCallback: Shutting down existing playback before loading new sequence");
                timeline_view->ShutdownPlayback();

                // Also shutdown scratch controller if it exists (dual view, etc.)
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Shutdown();
                    scratch_timeline_controller.reset();
                }

                // Clean up VideoDisplayComponent's EXR cache if active (legacy path)
                // Must use ClearEXRCache() to properly shutdown DirectEXRCache threads
                if (video_player && video_player->IsInEXRMode()) {
                    video_player->ClearEXRCache();
                    Debug::Log("ImageSequenceTimelineCallback: Cleared EXR cache");
                }

#ifdef _WIN32
                // Process pending GL texture deletions from destroyed decoders before creating new ones
                qcview::D3D11VideoInterop::ProcessPendingGLDeletions();
#endif

            }

            Debug::Log("ImageSequenceTimelineCallback: Loading " + item->name + " into timeline view");

            // Load the image sequence as a timeline
            if (timeline_view->LoadImageSequenceAsTimeline(item)) {
                // Set pipeline mode from PROJECT setting BEFORE initializing playback
                // This ensures the cache is created with the correct mode from the start
                // Project-level setting ensures all media in the project uses the same mode
                PipelineMode project_mode = project_manager->GetProjectPipelineMode();
                timeline_view->SetPendingPipelineMode(project_mode);
                Debug::Log("ImageSequenceTimelineCallback: Set pending pipeline mode to " +
                           std::string(PipelineModeToString(project_mode)) + " (from project)");

                // Initialize playback (will use the pending pipeline mode)
                timeline_view->InitializePlayback();

                // Ensure timeline starts paused (don't auto-play when switching)
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();

                    // Apply audio sync settings
                    if (auto* mixer = controller->GetAudioMixer()) {
                        const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                        float latency_ms = (cache_settings.display_latency_preset == 5)
                            ? cache_settings.custom_display_latency_ms
                            : preset_latencies_ms[cache_settings.display_latency_preset];
                        mixer->SetDisplayLatency(latency_ms / 1000.0);
                        mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                        // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                        qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                        if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                            source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                            mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                        } else {
                            mixer->SetPipelineLatency(0.0);
                        }
                    }
                }
                timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                // Initialize timeline thumbnail cache for clip thumbnails
                if (!timeline_thumbnail_cache) {
                    timeline_thumbnail_cache = std::make_unique<qcview::TimelineThumbnailCache>();
                    timeline_thumbnail_cache->Initialize(timeline_view ? timeline_view->GetFrameRate() : 24.0);
                }

                // Initialize command manager for editing (enables drag-drop onto audio tracks)
                if (!timeline_command_manager) {
                    timeline_command_manager = std::make_unique<qcview::TimelineCommandManager>();
                }

                // Enter timeline mode with playback controller
                otio_timeline_mode = true;
                if (video_player) {
                    video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                }

                // Request auto-fit zoom on next render (when we know the actual visible width)
                timeline_view->RequestFitZoomOnNextRender();

                // Store reference for tracking
                current_timeline_path.clear();  // No OTIO file path
                current_timeline_id = item->id;

                Debug::Log("Image sequence loaded into timeline view successfully");

                // Enable annotations for IMAGE_SEQUENCE mode (uses media-relative path)
                if (annotation_manager && annotation_panel) {
                    annotation_panel->SetAvailability(qcview::AnnotationAvailability::AVAILABLE);
                    std::string annotation_path = item->path;
                    if (project_manager) {
                        annotation_path = project_manager->GetAnnotationPathForMedia(item->path);
                    }
                    annotation_manager->LoadNotesForMedia(annotation_path);
                    Debug::Log("Loaded annotations for image sequence: " + annotation_path);
                }

                // Trigger auto-play if enabled (uses buffer-wait flow)
                TriggerAutoPlay();
            } else {
                Debug::Log("ERROR: Failed to load image sequence into timeline view");
                // Don't exit OTIO mode on failure - it's the default view
                // The empty state will be shown instead
            }
        });

        // Set up video file timeline callback - loads video files via timeline view
        project_manager->SetVideoFileTimelineCallback([this](qcview::MediaItem* item) {
            if (!item) {
                Debug::Log("VideoFileTimelineCallback: Invalid item");
                return;
            }

            // Create TimelineView if it doesn't exist (first time entering timeline mode)
            if (!timeline_view) {
                Debug::Log("VideoFileTimelineCallback: Creating TimelineView for first use");
                timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
            } else {
                // STOP playback first before any cleanup
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();
                }
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Pause();
                }

                // Shutdown existing playback before loading new video
                // This is critical when switching between videos to reset resolution/framerate
                Debug::Log("VideoFileTimelineCallback: Shutting down existing playback before loading new video");
                timeline_view->ShutdownPlayback();

                // Also shutdown scratch controller if it exists (dual view, etc.)
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Shutdown();
                    scratch_timeline_controller.reset();
                }

                // Clean up VideoDisplayComponent's EXR cache if active (legacy path)
                // Must use ClearEXRCache() to properly shutdown DirectEXRCache threads
                if (video_player && video_player->IsInEXRMode()) {
                    video_player->ClearEXRCache();
                    Debug::Log("VideoFileTimelineCallback: Cleared EXR cache");
                }

#ifdef _WIN32
                // Process pending GL texture deletions from destroyed decoders before creating new ones
                qcview::D3D11VideoInterop::ProcessPendingGLDeletions();
#endif

            }

            Debug::Log("VideoFileTimelineCallback: Loading " + item->name + " into timeline view");

            // Load the video file as a timeline
            if (timeline_view->LoadVideoFileAsTimeline(item)) {
                // Set pipeline mode from PROJECT setting BEFORE initializing playback
                // This ensures the cache is created with the correct mode from the start
                // Project-level setting ensures all video in the project uses the same mode
                PipelineMode project_mode = project_manager->GetProjectPipelineMode();
                timeline_view->SetPendingPipelineMode(project_mode);
                Debug::Log("VideoFileTimelineCallback: Set pending pipeline mode to " +
                           std::string(PipelineModeToString(project_mode)) + " (from project)");

                // Set video codec from cached metadata (for D3D11VA HDR eligibility - HEVC only)
                const auto* cached_meta = project_manager->GetCachedMetadata(item->path);
                if (cached_meta && cached_meta->video_meta) {
                    timeline_view->SetPendingVideoCodec(cached_meta->video_meta->video_codec);
                    Debug::Log("VideoFileTimelineCallback: Set video codec to '" +
                               cached_meta->video_meta->video_codec + "' for D3D11VA HDR check");

                    // Parse embedded timecode to get start frame offset (for D3D11VA HDR frame alignment)
                    if (cached_meta->video_meta->has_embedded_timecode &&
                        !cached_meta->video_meta->timecode_format.empty()) {
                        // Parse SMPTE timecode: HH:MM:SS:FF
                        int hours = 0, minutes = 0, seconds = 0, frames = 0;
                        if (sscanf(cached_meta->video_meta->timecode_format.c_str(),
                                   "%d:%d:%d:%d", &hours, &minutes, &seconds, &frames) == 4) {
                            // Convert to total frames (use video's fps)
                            double fps = cached_meta->video_meta->frame_rate;
                            if (fps <= 0) fps = 24.0;  // Fallback
                            int fps_int = static_cast<int>(fps + 0.5);
                            int start_frame = ((hours * 60 + minutes) * 60 + seconds) * fps_int + frames;
                            timeline_view->SetPendingTimecodeStartFrame(start_frame);
                            Debug::Log("VideoFileTimelineCallback: Embedded timecode '" +
                                       cached_meta->video_meta->timecode_format +
                                       "' -> start frame " + std::to_string(start_frame));
                        }
                    }
                }

                // Initialize playback (will use the pending pipeline mode and codec)
                timeline_view->InitializePlayback();

                // Ensure timeline starts paused (don't auto-play when switching)
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();
                    // Pipeline mode is now set via SetPendingPipelineMode() before InitializePlayback()

                    // Apply audio sync settings
                    if (auto* mixer = controller->GetAudioMixer()) {
                        const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                        float latency_ms = (cache_settings.display_latency_preset == 5)
                            ? cache_settings.custom_display_latency_ms
                            : preset_latencies_ms[cache_settings.display_latency_preset];
                        mixer->SetDisplayLatency(latency_ms / 1000.0);
                        mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                        // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                        qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                        if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                            source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                            mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                        } else {
                            mixer->SetPipelineLatency(0.0);
                        }
                    }
                }
                timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                // Initialize timeline thumbnail cache for clip thumbnails
                if (!timeline_thumbnail_cache) {
                    timeline_thumbnail_cache = std::make_unique<qcview::TimelineThumbnailCache>();
                    timeline_thumbnail_cache->Initialize(timeline_view ? timeline_view->GetFrameRate() : 24.0);
                }

                // Enter timeline mode with playback controller
                otio_timeline_mode = true;
                if (video_player) {
                    video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                }

                // Request auto-fit zoom on next render (when we know the actual visible width)
                timeline_view->RequestFitZoomOnNextRender();

                // Store reference for tracking
                current_timeline_path.clear();  // No OTIO file path
                current_timeline_id = item->id;

                Debug::Log("Video file loaded into timeline view successfully");

                // Enable annotations for VIDEO_FILE mode (uses media-relative path)
                if (annotation_manager && annotation_panel) {
                    annotation_panel->SetAvailability(qcview::AnnotationAvailability::AVAILABLE);
                    std::string annotation_path = item->path;
                    if (project_manager) {
                        annotation_path = project_manager->GetAnnotationPathForMedia(item->path);
                    }
                    annotation_manager->LoadNotesForMedia(annotation_path);
                    Debug::Log("Loaded annotations for video file: " + annotation_path);
                }

                // Trigger auto-play if enabled (uses buffer-wait flow)
                TriggerAutoPlay();
            } else {
                Debug::Log("ERROR: Failed to load video file into timeline view");
            }
        });

        // Set up audio file timeline callback - loads audio files via timeline view (A1 track only)
        project_manager->SetAudioFileTimelineCallback([this](qcview::MediaItem* item) {
            if (!item) {
                Debug::Log("AudioFileTimelineCallback: Invalid item");
                return;
            }

            // Create TimelineView if it doesn't exist (first time entering timeline mode)
            if (!timeline_view) {
                Debug::Log("AudioFileTimelineCallback: Creating TimelineView for first use");
                timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
            } else {
                // STOP playback first before any cleanup
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();
                }
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Pause();
                }

                // Shutdown existing playback to properly clean up previous decoder/cache
                Debug::Log("AudioFileTimelineCallback: Shutting down existing playback before loading new audio");
                timeline_view->ShutdownPlayback();

                // Also shutdown scratch controller if it exists (dual view, etc.)
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Shutdown();
                    scratch_timeline_controller.reset();
                }

                // Clean up VideoDisplayComponent's EXR cache if active (legacy path)
                // Must use ClearEXRCache() to properly shutdown DirectEXRCache threads
                if (video_player && video_player->IsInEXRMode()) {
                    video_player->ClearEXRCache();
                    Debug::Log("AudioFileTimelineCallback: Cleared EXR cache");
                }

#ifdef _WIN32
                // Process pending GL texture deletions from destroyed decoders before creating new ones
                qcview::D3D11VideoInterop::ProcessPendingGLDeletions();
#endif

            }

            Debug::Log("AudioFileTimelineCallback: Loading " + item->name + " into timeline view");

            // Load the audio file as a timeline (A1 track only)
            if (timeline_view->LoadAudioFileAsTimeline(item)) {
                // Initialize playback
                timeline_view->InitializePlayback();

                // Ensure timeline starts paused (don't auto-play when switching)
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();

                    // Apply audio sync settings
                    if (auto* mixer = controller->GetAudioMixer()) {
                        const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                        float latency_ms = (cache_settings.display_latency_preset == 5)
                            ? cache_settings.custom_display_latency_ms
                            : preset_latencies_ms[cache_settings.display_latency_preset];
                        mixer->SetDisplayLatency(latency_ms / 1000.0);
                        mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                        // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                        qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                        if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                            source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                            mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                        } else {
                            mixer->SetPipelineLatency(0.0);
                        }
                    }
                }
                timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                // No thumbnail cache needed for audio-only mode

                // Enter timeline mode with playback controller
                otio_timeline_mode = true;
                if (video_player) {
                    video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                }

                // Request auto-fit zoom on next render (when we know the actual visible width)
                timeline_view->RequestFitZoomOnNextRender();

                // Store reference for tracking
                current_timeline_path.clear();  // No OTIO file path
                current_timeline_id = item->id;

                Debug::Log("Audio file loaded into timeline view successfully");

                // Enable annotations for AUDIO_FILE mode (uses media-relative path)
                if (annotation_manager && annotation_panel) {
                    annotation_panel->SetAvailability(qcview::AnnotationAvailability::AVAILABLE);
                    std::string annotation_path = item->path;
                    if (project_manager) {
                        annotation_path = project_manager->GetAnnotationPathForMedia(item->path);
                    }
                    annotation_manager->LoadNotesForMedia(annotation_path);
                    Debug::Log("Loaded annotations for audio file: " + annotation_path);
                }

                // Trigger auto-play if enabled (uses buffer-wait flow)
                TriggerAutoPlay();
            } else {
                Debug::Log("ERROR: Failed to load audio file into timeline view");
            }
        });

        // Set up playlist timeline callback - loads playlists via unified timeline view
        project_manager->SetPlaylistTimelineCallback([this](qcview::MediaItem* item) {
            if (!item) {
                Debug::Log("PlaylistTimelineCallback: Invalid item");
                return;
            }

            // Clear dual view mode flag (may be set if switching from dual view)
            otio_dual_view_mode = false;

            // Create TimelineView if it doesn't exist (first time entering timeline mode)
            if (!timeline_view) {
                Debug::Log("PlaylistTimelineCallback: Creating TimelineView for first use");
                timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
                // Set project manager for media resolution in playlist mode
                timeline_view->SetProjectManager(project_manager.get());
            } else {
                // STOP playback first before any cleanup
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();
                }
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Pause();
                }

                // Shutdown existing playback to properly clean up previous decoder/cache
                Debug::Log("PlaylistTimelineCallback: Shutting down existing playback before loading playlist");
                timeline_view->ShutdownPlayback();

                // Also shutdown scratch controller if it exists (dual view, etc.)
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Shutdown();
                    scratch_timeline_controller.reset();
                }

                // Clean up VideoDisplayComponent's EXR cache if active (legacy path)
                // Must use ClearEXRCache() to properly shutdown DirectEXRCache threads
                if (video_player && video_player->IsInEXRMode()) {
                    video_player->ClearEXRCache();
                    Debug::Log("PlaylistTimelineCallback: Cleared EXR cache");
                }

#ifdef _WIN32
                // Process pending GL texture deletions from destroyed decoders before creating new ones
                qcview::D3D11VideoInterop::ProcessPendingGLDeletions();
#endif

            }

            Debug::Log("PlaylistTimelineCallback: Loading " + item->name + " into timeline view");

            // Ensure project manager is set for media resolution in playlist mode
            timeline_view->SetProjectManager(project_manager.get());

            // Load the playlist as a timeline using unified playlist system
            // This creates a single-track timeline with ripple editing from the playlist items
            if (timeline_view->LoadPlaylistAsTimeline(item)) {
                // Set pipeline mode from PROJECT setting BEFORE initializing playback
                PipelineMode project_mode = project_manager->GetProjectPipelineMode();
                timeline_view->SetPendingPipelineMode(project_mode);
                Debug::Log("PlaylistTimelineCallback: Set pending pipeline mode to " +
                           std::string(PipelineModeToString(project_mode)) + " (from project)");

                // Initialize playback (will use the pending pipeline mode)
                timeline_view->InitializePlayback();

                // Ensure timeline starts paused (don't auto-play when switching)
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();

                    // Apply audio sync settings
                    if (auto* mixer = controller->GetAudioMixer()) {
                        const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                        float latency_ms = (cache_settings.display_latency_preset == 5)
                            ? cache_settings.custom_display_latency_ms
                            : preset_latencies_ms[cache_settings.display_latency_preset];
                        mixer->SetDisplayLatency(latency_ms / 1000.0);
                        mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                        // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                        qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                        if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                            source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                            mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                        } else {
                            mixer->SetPipelineLatency(0.0);
                        }
                    }
                }
                timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                // Initialize timeline thumbnail cache for clip thumbnails
                if (!timeline_thumbnail_cache) {
                    timeline_thumbnail_cache = std::make_unique<qcview::TimelineThumbnailCache>();
                    timeline_thumbnail_cache->Initialize(timeline_view ? timeline_view->GetFrameRate() : 24.0);
                }

                // Enter timeline mode with playback controller
                otio_timeline_mode = true;
                if (video_player) {
                    video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                }

                // Request auto-fit zoom on next render
                timeline_view->RequestFitZoomOnNextRender();

                // Store reference for tracking
                current_timeline_path.clear();  // No OTIO file path
                current_timeline_id = item->id;

                // Disable annotations for PLAYLIST mode
                if (annotation_manager && annotation_panel) {
                    annotation_panel->SetAvailability(qcview::AnnotationAvailability::PLAYLIST_DISABLED);
                    annotation_manager->ClearNotes();
                    Debug::Log("Annotations disabled for Playlist mode");
                }

                Debug::Log("Playlist loaded into unified timeline view successfully");

                // Set up drag-drop callback for adding items to playlist
                // Capture playlist ID by value (not pointer) to avoid dangling pointer if media_pool reallocates
                std::string playlist_id = item->id;
                timeline_view->SetDropMediaCallback([this, playlist_id](const std::vector<std::string>& media_ids) {
                    if (!project_manager) return;

                    Debug::Log("Adding " + std::to_string(media_ids.size()) + " items to playlist");

                    // Get the playlist MediaItem fresh (pointer may have changed due to vector reallocation)
                    qcview::MediaItem* playlist = project_manager->GetPlaylistItem(playlist_id);
                    if (!playlist) {
                        Debug::Log("ERROR: Playlist not found for drop");
                        return;
                    }

                    // Add each dropped item as a playlist entry
                    for (const auto& media_id : media_ids) {
                        qcview::MediaItem* media_item = project_manager->GetMediaItem(media_id);
                        if (!media_item) continue;

                        // Skip non-playable types
                        if (media_item->type == qcview::MediaType::PLAYLIST ||
                            media_item->type == qcview::MediaType::IMAGE ||
                            media_item->type == qcview::MediaType::DUAL_VIEW) {
                            continue;
                        }

                        qcview::PlaylistItemEntry entry;
                        entry.media_id = media_id;
                        entry.in_point = -1.0;  // Use full duration
                        entry.out_point = -1.0;
                        playlist->playlist_items.push_back(entry);

                        Debug::Log("Added '" + media_item->name + "' to playlist");
                    }

                    // Reload the playlist to show new clips
                    if (timeline_view && !playlist->playlist_items.empty()) {
                        timeline_view->LoadPlaylistAsTimeline(playlist);
                        // Re-initialize playback with updated clips
                        timeline_view->InitializePlayback();

                        // Ensure timeline mode is active
                        otio_timeline_mode = true;
                        if (video_player) {
                            video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                        }

                        if (auto* controller = timeline_view->GetPlaybackController()) {
                            controller->Pause();
                        }
                        timeline_view->RequestFitZoomOnNextRender();
                    }
                });

                // Trigger auto-play if enabled
                TriggerAutoPlay();
            } else {
                Debug::Log("ERROR: Failed to load playlist into timeline view");
            }
        });

        // Set up dual view editor callback - opens dual view in OTIO editor mode
        project_manager->SetDualViewEditorCallback([this](const std::string& dual_view_id) {
            Debug::Log("Opening dual view in editor: " + dual_view_id);

            // Get the dual view item
            qcview::MediaItem* dual_item = project_manager->GetDualViewItem(dual_view_id);
            if (!dual_item) {
                Debug::Log("ERROR: Dual view item not found: " + dual_view_id);
                return;
            }

            // ========================================================================
            // RESOURCE CLEANUP - Stop other playback modes before entering dual view
            // ========================================================================

            if (video_player) {
                if (video_player->IsPlaying()) {
                    video_player->Pause();
                    Debug::Log("Dual view entry: Paused video playback");
                }

                // Clean up EXR/image sequence state if active
                // Clean up EXR/image sequence cache if active
                // Must use ClearEXRCache() to properly shutdown DirectEXRCache threads
                if (video_player->IsInEXRMode()) {
                    video_player->ClearEXRCache();
                }
            }

            // Clear current file path
            if (!current_file_path.empty()) {
                current_file_path.clear();
            }

            // ========================================================================

            // Store edits from current timeline/dual view BEFORE switching
            if (timeline_view && !timeline_view->IsViewingNestedTimeline()) {
                std::string lookup_key = !current_timeline_path.empty() ? current_timeline_path : current_timeline_id;
                if (!lookup_key.empty()) {
                    // Try to find as regular timeline, dual view, or media item (image/EXR sequences)
                    qcview::MediaItem* current_item = project_manager->GetTimelineItem(lookup_key);
                    if (!current_item) {
                        current_item = project_manager->GetDualViewItem(lookup_key);
                    }
                    if (!current_item) {
                        current_item = project_manager->GetMediaItem(lookup_key);
                    }
                    if (current_item) {
                        current_item->cached_tracks = timeline_view->GetTracks();
                        current_item->has_cached_edits = true;
                        current_item->duration = timeline_view->GetDuration();
                        Debug::Log("Stored " + std::to_string(current_item->cached_tracks.size()) +
                                   " edited tracks before switching from: " + lookup_key);
                    }
                }
            }

            // Create TimelineView if needed
            if (!timeline_view) {
                timeline_view = std::make_unique<qcview::TimelineView>(video_player.get());
            } else {
                // STOP playback first before any cleanup
                if (auto* controller = timeline_view->GetPlaybackController()) {
                    controller->Pause();
                }
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Pause();
                }

                // Shutdown existing playback
                timeline_view->ShutdownPlayback();

                // Also shutdown scratch controller if it exists
                if (scratch_timeline_controller) {
                    scratch_timeline_controller->Shutdown();
                    scratch_timeline_controller.reset();
                }

#ifdef _WIN32
                // Process pending GL texture deletions from destroyed decoders before creating new ones
                qcview::D3D11VideoInterop::ProcessPendingGLDeletions();
#endif

            }

            // Initialize as dual view timeline
            timeline_view->InitializeForDualView(dual_item->name, dual_item->frame_rate);

            // Restore cached tracks if available
            if (dual_item->has_cached_edits && !dual_item->cached_tracks.empty()) {
                timeline_view->SetTracks(dual_item->cached_tracks);
                // SetTracks calls ResetSourceMode() which breaks dual view - restore it
                timeline_view->SetSourceMode(qcview::TimelineSourceMode::DUAL_VIEW);
                Debug::Log("Restored " + std::to_string(dual_item->cached_tracks.size()) +
                           " cached tracks for dual view");
            }

            // Switch to timeline editor mode
            otio_timeline_mode = true;
            otio_dual_view_mode = true;  // Enable dual view viewport rendering
            current_timeline_path.clear();  // No file for dual views
            current_timeline_id = dual_item->timeline_id;

            // Initialize command manager for editing
            if (!timeline_command_manager) {
                timeline_command_manager = std::make_unique<qcview::TimelineCommandManager>();
            }

            // Initialize timeline thumbnail cache for trim/slip preview
            if (!timeline_thumbnail_cache) {
                timeline_thumbnail_cache = std::make_unique<qcview::TimelineThumbnailCache>();
                timeline_thumbnail_cache->Initialize(timeline_view ? timeline_view->GetFrameRate() : 24.0);
            }

            // Initialize dual view playback with separate LEFT/RIGHT caches
            if (video_player) {
                scratch_timeline_controller = std::make_unique<qcview::TimelinePlaybackController>();

                // Set pipeline mode from project settings BEFORE initialization
                qcview::TimelinePlaybackConfig config = scratch_timeline_controller->GetConfig();
                config.pipeline_mode = project_manager ? project_manager->GetProjectPipelineMode() : PipelineMode::NORMAL;
                scratch_timeline_controller->SetConfig(config);
                Debug::Log("Dual view: Set pipeline mode to " + std::string(PipelineModeToString(config.pipeline_mode)));

                if (scratch_timeline_controller->InitializeForDualView(
                        timeline_view.get(),
                        video_player.get())) {
                    Debug::Log("Dual view playback initialized: " +
                               std::to_string(timeline_view->GetCanvasWidth()) + "x" +
                               std::to_string(timeline_view->GetCanvasHeight()) + " @ " +
                               std::to_string(timeline_view->GetFrameRate()) + "fps");
                    video_player->SetTimelineMode(true, scratch_timeline_controller.get());
                    timeline_view->SetExternalPlaybackController(scratch_timeline_controller.get());

                    // Connect thumbnail cache for playback state notifications
                    if (timeline_thumbnail_cache) {
                        scratch_timeline_controller->SetThumbnailCache(timeline_thumbnail_cache.get());
                    }

                } else {
                    Debug::Log("Failed to initialize dual view playback");
                    scratch_timeline_controller.reset();
                    video_player->SetTimelineMode(true, nullptr);
                    timeline_view->SetExternalPlaybackController(nullptr);
                }
            }

            // Request auto-fit zoom on next render
            timeline_view->RequestFitZoomOnNextRender();

            // Disable annotations for DUAL_VIEW mode
            if (annotation_manager && annotation_panel) {
                annotation_panel->SetAvailability(qcview::AnnotationAvailability::DUAL_VIEW_DISABLED);
                annotation_manager->ClearNotes();
                Debug::Log("Annotations disabled for Dual View mode");
            }

            Debug::Log("Dual view mode activated: " + dual_item->name);
        });

        // Playlists open directly in TimelineView via OpenPlaylistInTimelineEditor

        // Set up project saved callback - refreshes annotation availability for timelines
        // Note: Playlists have annotations disabled entirely, so no special handling needed
        project_manager->SetProjectSavedCallback([this]() {
            // Currently no action needed on project save for annotation refresh
            // Playlists and Dual View have annotations disabled
            // Other modes use media-relative annotation paths that don't depend on project path
        });

        // Connect node manager to project manager (for OCIO settings extraction during transcode)
        project_manager->SetNodeManager(node_manager.get());

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
            AutoSaveAnnotationOnSeek();
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

        // Wire annotation screenshot callbacks on video player
        video_player->SetGetAnnotationStrokesCallback([this]() -> std::vector<qcview::Annotations::ActiveStroke> {
            std::vector<qcview::Annotations::ActiveStroke> strokes;

            if (!cache_settings.bake_annotations_on_screenshots || !annotation_manager) {
                return strokes;
            }

            // If currently editing annotations, use live editing strokes
            if (viewport_annotator && viewport_annotator->GetMode() == qcview::Annotations::ViewportMode::ANNOTATION
                && !current_editing_timecode_.empty()) {
                strokes = current_annotation_strokes_;
                if (viewport_annotator->GetActiveStroke()) {
                    strokes.push_back(*viewport_annotator->GetActiveStroke());
                }
                return strokes;
            }

            // Look up saved annotations for current frame
            if (video_player) {
                double position = video_player->GetPosition();
                double fps = video_player->GetFrameRate();
                std::string timecode = VideoDisplayComponent::FormatTimecode(position, fps);
                auto* note = annotation_manager->GetNoteAtTimecode(timecode);
                if (note && !note->annotation_data.empty()) {
                    strokes = qcview::Annotations::AnnotationSerializer::JsonStringToStrokes(note->annotation_data);
                }
            }

            return strokes;
        });

        // GL/NanoVG annotation rendering callback (Windows)
        video_player->SetRenderAnnotationsToFBOCallback(
            [this](const std::vector<qcview::Annotations::ActiveStroke>& strokes, int width, int height) {
                if (!annotation_renderer) return;
                annotation_renderer->BeginFrame((float)width, (float)height, 1.0f);
                for (const auto& stroke : strokes) {
                    if (stroke.points.empty()) continue;
                    annotation_renderer->RenderActiveStroke(
                        stroke, 0.0f, 0.0f, (float)width, (float)height, true, 1.0f);
                }
                annotation_renderer->EndFrame();
            });

#ifdef QCVIEW_USE_VULKAN
        // Vulkan annotation rendering callback (Linux)
        video_player->SetVulkanRenderAnnotationsToImageCallback(
            [this](const std::vector<qcview::Annotations::ActiveStroke>& strokes,
                   VkImage target, VkImageView target_view, int w, int h) -> bool {
                if (!vulkan_annotation_renderer_ || !vulkan_annotation_renderer_->IsInitialized()) return false;
                return vulkan_annotation_renderer_->RenderAnnotationsToImage(
                    strokes, target, target_view, w, h,
                    0.0f, 0.0f, (float)w, (float)h, 1.0f, true);
            });
#elif defined(QCVIEW_USE_METAL)
        // Metal annotation rendering callback (macOS)
        video_player->SetMetalRenderAnnotationsToImageCallback(
            [this](const std::vector<qcview::Annotations::ActiveStroke>& strokes,
                   void* target_texture, int w, int h) {
                if (!metal_annotation_renderer_ || !metal_annotation_renderer_->IsInitialized()) return;
                metal_annotation_renderer_->RenderAnnotationsToImage(
                    strokes, target_texture, w, h, 1.0f);
            });
#endif

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

        // Pass annotation toolbar and viewport annotator for modal editing
        annotation_panel->SetAnnotationToolbar(annotation_toolbar.get());
        annotation_panel->SetViewportAnnotator(viewport_annotator.get());
        annotation_panel->SetCanUndoCallback([this]() { return !annotation_undo_stack_.empty(); });
        annotation_panel->SetCanRedoCallback([this]() { return !annotation_redo_stack_.empty(); });

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
                    std::string json_data = qcview::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);
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
                current_annotation_strokes_ = qcview::Annotations::AnnotationSerializer::JsonStringToStrokes(annotation_data);
                Debug::Log("Loaded " + std::to_string(current_annotation_strokes_.size()) + " existing strokes");
            } else {
                Debug::Log("No existing strokes - starting with blank canvas");
            }

            // Enter annotation mode
            viewport_annotator->SetMode(qcview::Annotations::ViewportMode::ANNOTATION);

            // Set freehand tool as default
            viewport_annotator->SetActiveTool(qcview::Annotations::DrawingTool::FREEHAND);

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
                std::string json_data = qcview::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);
                annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);
                Debug::Log("Saved annotation: " + current_editing_timecode_);

                // Generate annotated thumbnail
                auto* note = annotation_manager->GetNoteAtTimecode(current_editing_timecode_);
                if (note && !note->annotation_data.empty()) {
                    std::string images_folder = annotation_manager->GetImagesFolder();
                    std::string filename = note->image_path.substr(note->image_path.find_last_of('/') + 1);
                    std::string clean_path = images_folder + "/" + filename;
                    std::string annotated_path = qcview::Annotations::GetAnnotatedThumbnailPath(clean_path);
                    qcview::Annotations::GenerateAnnotatedThumbnail(clean_path, annotated_path, note->annotation_data);

                    if (annotation_panel) {
                        annotation_panel->InvalidateThumbnail(annotated_path);
                    }
                }
            }

            // Exit annotation mode
            viewport_annotator->SetMode(qcview::Annotations::ViewportMode::PLAYBACK);
            annotation_toolbar->SetVisible(false);
            current_editing_timecode_.clear();
            current_annotation_strokes_.clear();

            Debug::Log("Edit mode deactivated");
        });

        // Callback for entering modal edit mode (Edit button in annotation panel)
        annotation_panel->SetEnterModalEditCallback([this](const std::string& timecode, double timestamp, int frame, const std::string& annotation_data) {
            if (!video_player || !viewport_annotator || !annotation_toolbar) {
                Debug::Log("Cannot enter modal edit mode: Missing required components");
                return;
            }

            // Auto-save if switching from a different annotation
            if (viewport_annotator->IsAnnotationMode() && !current_editing_timecode_.empty() && current_editing_timecode_ != timecode) {
                auto active_stroke = viewport_annotator->FinalizeStroke();
                if (active_stroke) {
                    current_annotation_strokes_.push_back(*active_stroke);
                }
                if (annotation_manager) {
                    std::string json_data = qcview::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);
                    annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);
                }
            }

            current_editing_timecode_ = timecode;

            video_player->Pause();
            video_player->Seek(timestamp);

            current_annotation_strokes_.clear();
            annotation_undo_stack_.clear();
            annotation_redo_stack_.clear();

            if (!annotation_data.empty()) {
                current_annotation_strokes_ = qcview::Annotations::AnnotationSerializer::JsonStringToStrokes(annotation_data);
            }

            viewport_annotator->SetMode(qcview::Annotations::ViewportMode::ANNOTATION);
            viewport_annotator->SetActiveTool(qcview::Annotations::DrawingTool::FREEHAND);
            viewport_annotator->SetAllowInputInPopup(true);
            annotation_toolbar->SetVisible(true);

            Debug::Log("Modal edit mode activated for: " + timecode);
        });

        // Callback for saving modal edit (called when modal closes)
        annotation_panel->SetSaveModalEditCallback([this]() {
            if (!viewport_annotator || !annotation_toolbar) return;

            auto active_stroke = viewport_annotator->FinalizeStroke();
            if (active_stroke) {
                current_annotation_strokes_.push_back(*active_stroke);
            }

            if (annotation_manager && !current_editing_timecode_.empty()) {
                std::string json_data = qcview::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);
                annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);

                // Generate annotated thumbnail
                auto* note = annotation_manager->GetNoteAtTimecode(current_editing_timecode_);
                if (note && !note->annotation_data.empty()) {
                    std::string images_folder = annotation_manager->GetImagesFolder();
                    std::string filename = note->image_path.substr(note->image_path.find_last_of('/') + 1);
                    std::string clean_path = images_folder + "/" + filename;
                    std::string annotated_path = qcview::Annotations::GetAnnotatedThumbnailPath(clean_path);
                    qcview::Annotations::GenerateAnnotatedThumbnail(clean_path, annotated_path, note->annotation_data);

                    // Invalidate cached thumbnail so panel reloads it
                    if (annotation_panel) {
                        annotation_panel->InvalidateThumbnail(annotated_path);
                    }
                }
            }

            viewport_annotator->SetMode(qcview::Annotations::ViewportMode::PLAYBACK);
            viewport_annotator->SetAllowInputInPopup(false);
            annotation_toolbar->SetVisible(false);
            current_editing_timecode_.clear();
            current_annotation_strokes_.clear();
            annotation_undo_stack_.clear();
            annotation_redo_stack_.clear();

            Debug::Log("Modal edit saved and closed");
        });

        // Callback for deleting a note from modal
        annotation_panel->SetDeleteNoteCallback([this](const std::string& timecode) {
            if (viewport_annotator) {
                viewport_annotator->SetMode(qcview::Annotations::ViewportMode::PLAYBACK);
                viewport_annotator->SetAllowInputInPopup(false);
            }
            if (annotation_toolbar) {
                annotation_toolbar->SetVisible(false);
            }
            current_editing_timecode_.clear();
            current_annotation_strokes_.clear();
            annotation_undo_stack_.clear();
            annotation_redo_stack_.clear();

            if (annotation_manager) {
                annotation_manager->DeleteNote(timecode);
            }
            Debug::Log("Note deleted from modal: " + timecode);
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
            extern bool g_nfd_dialog_open;
            if (g_nfd_dialog_open) return;
            g_nfd_dialog_open = true;

            nfdu8char_t* out_path = nullptr;
            nfdresult_t result = NFD_PickFolderU8(&out_path, nullptr);
            ImGui::GetIO().ClearInputKeys();
            g_nfd_dialog_open = false;

            if (result != NFD_OKAY) {
                if (result == NFD_ERROR) {
                    Debug::Log("Export cancelled: File picker error");
                }
                return;
            }

            std::string export_directory = out_path;
            NFD_FreePathU8(out_path);

            // Determine export format
            qcview::Annotations::AnnotationExporter::ExportFormat export_format;
            if (format == "markdown") {
                export_format = qcview::Annotations::AnnotationExporter::ExportFormat::MARKDOWN;
            } else if (format == "html") {
                export_format = qcview::Annotations::AnnotationExporter::ExportFormat::HTML;
            } else if (format == "pdf") {
                export_format = qcview::Annotations::AnnotationExporter::ExportFormat::PDF;
            } else {
                Debug::Log("Unknown export format: " + format);
                return;
            }

            // Set up export options
            qcview::Annotations::AnnotationExporter::ExportOptions options;
            std::string current_path = current_file_path;
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

        // Playlists are handled in TimelineView with ripple editing

        // Note: Cache limits are now managed per-video using seconds-based windows

        // Process command-line file arguments (if any)
        if (!initial_files.empty()) {
            Debug::Log("Processing " + std::to_string(initial_files.size()) + " file(s) from command-line");

            if (initial_files.size() == 1) {
                std::string arg = initial_files[0];

                // Check if it's a qcview:// URI
                if (arg.substr(0, 10) == "qcview:///") {
                    Debug::Log("Detected qcview:// URI from command-line");
                    std::string project_path = ParseProjectURI(arg);
                    if (!project_path.empty()) {
                        Debug::Log("Parsed project path from URI: " + project_path);
                        project_manager->LoadProject(project_path);

                        // Show project panels for context
                        show_project_panel = true;
                        show_inspector_panel = true;
                        Debug::Log("Opened Project Manager and Inspector panels");
                    } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                        Debug::Log("ERROR: Failed to parse URI: " + arg);
                    }
                }
                // Direct project file
                else if (arg.find(".qcvproj") != std::string::npos || arg.find(".umproj") != std::string::npos) {
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
        // macOS only: not started — all metrics are Windows-only (#ifdef _WIN32),
        // and macOS handles memory pressure gracefully (compression + swap).
#ifndef __APPLE__
        pressure_monitor = std::make_unique<qcview::SystemPressureMonitor>();
        pressure_monitor->SetRAMCriticalThreshold(0.90f);  // 90% critical
        pressure_monitor->SetRAMWarningThreshold(0.80f);   // 80% warning
        pressure_monitor->SetCPUWarningThreshold(0.85f);   // 85% CPU warning
        pressure_monitor->SetPollInterval(3.0);            // Poll every 3 seconds
        pressure_monitor->Start();
        Debug::Log("System pressure monitor started");
#endif

        return true;
    }
