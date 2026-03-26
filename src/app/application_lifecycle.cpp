// application_lifecycle.cpp - Constructor, destructor, Cleanup, ForceReload, etc.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>  // _exit()
#else
#include <unistd.h>   // _exit()
#endif

#include <glad/gl.h>
#ifndef QCVIEW_USE_METAL
#include <GLFW/glfw3.h>
#endif

#include <imgui.h>
#ifdef QCVIEW_USE_METAL
#include "app/macos_app_delegate.h"
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
#include "annotations/metal_annotation_renderer.h"
#include "app/macos_menu_bar.h"
#else
#include <imgui_impl_opengl3.h>
#endif
#include <implot.h>
#include <nfd.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/application.h"
#include "utils/debug_utils.h"
#include "utils/system_pressure_monitor.h"
#include "player/video_player.h"
#include "project/project_manager.h"
#include "project/media_item.h"
#include "imnodes/imnodes.h"
#include "nodes/node_manager.h"
#include "color/ocio_config_manager.h"
#include "color/ocio_pipeline.h"
#include "ui/timeline_manager.h"
#include "annotations/annotation_manager.h"
#include "ui/annotation_panel.h"
#include "annotations/viewport_annotator.h"
#include "annotations/annotation_toolbar.h"
#include "annotations/annotation_renderer.h"
#include "annotations/annotation_serializer.h"
#include "app/shortcut_manager.h"
#include "annotations/nanovg_context.h"
#include "annotations/annotation_exporter.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "timeline/timeline_cache.h"
#include "timeline/timeline_thumbnail_cache.h"
#include "timeline/timeline_commands.h"
#include "timeline/media_linker.h"
#include "audio/audio_player.h"
#include "audio/audio_mixer.h"
#include "player/hw_context_manager.h"
#include "player/shared_memory_pool.h"
#include "hdr/hdr_color_utils.h"
#ifdef _WIN32
#include "hdr/hdr_output_manager.h"
#include "gpu/d3d11_video_interop.h"
#endif

// ColorCorrectedTextureCache struct (defined in main.cpp)
struct ColorCorrectedTextureCache {
    GLuint left_texture = 0;
    GLuint right_texture = 0;
    int left_width = 0, left_height = 0;
    int right_width = 0, right_height = 0;
    GLuint cached_frame_texture = 0;
    int cached_frame_width = 0, cached_frame_height = 0;
    GLuint cached_frame_source_id = 0;
    GLuint composite_texture = 0;
    int composite_width = 0, composite_height = 0;
    GLuint prev_left_texture = 0, prev_right_texture = 0;
    GLuint prev_cached_frame_texture = 0, prev_composite_texture = 0;
    GLsync upload_fence = nullptr;
    bool current_ready = true;
    GLuint GetLeftTexture() const { return current_ready ? left_texture : prev_left_texture; }
    GLuint GetRightTexture() const { return current_ready ? right_texture : prev_right_texture; }
    GLuint GetCachedFrameTexture() const { return current_ready ? cached_frame_texture : prev_cached_frame_texture; }
    GLuint GetCompositeTexture() const { return current_ready ? composite_texture : prev_composite_texture; }
};

// Externs for globals defined in main.cpp
extern std::unique_ptr<OCIOConfigManager> ocio_manager;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern std::unique_ptr<qcview::MediaLinker> media_linker;
extern std::unique_ptr<qcview::TimelinePlaybackController> scratch_timeline_controller;
extern qcview::TimelinePlaybackController::DualViewTextures cached_dual_view_textures;
extern std::vector<GLuint> g_pending_texture_deletions;
extern ColorCorrectedTextureCache g_color_corrected_cache;
extern std::unique_ptr<qcview::TimelineCommandManager> timeline_command_manager;
extern std::unique_ptr<qcview::TimelineThumbnailCache> timeline_thumbnail_cache;
extern std::unique_ptr<qcview::SystemPressureMonitor> pressure_monitor;
extern bool otio_timeline_mode;
extern bool otio_dual_view_mode;
extern bool cache_enabled;
extern bool g_skip_viewport_render_frame;
extern bool g_clear_cache_on_exit;
extern std::string current_timeline_path;
extern std::string current_timeline_id;

// ------------------------------------------------------------------------
// CONSTRUCTOR & DESTRUCTOR
// ------------------------------------------------------------------------
Application::Application() : window(nullptr), video_player(nullptr), first_time_setup(false),
        show_project_panel(true), show_inspector_panel(true),
        show_timeline_panel(true), show_transport_controls(true),
        show_status_bar(true), show_color_panels(false) {

        app_instance = this; // Set static pointer for window procedure
        node_manager = std::make_unique<qcview::NodeManager>();
        timeline_manager = std::make_unique<TimelineManager>();
        annotation_manager = std::make_unique<qcview::AnnotationManager>();
        annotation_panel = std::make_unique<qcview::AnnotationPanel>();
        annotation_exporter = std::make_unique<qcview::Annotations::AnnotationExporter>();
        viewport_annotator = std::make_unique<qcview::Annotations::ViewportAnnotator>();
        annotation_toolbar = std::make_unique<qcview::Annotations::AnnotationToolbar>();
        annotation_renderer = std::make_unique<qcview::Annotations::AnnotationRenderer>();

        node_manager->on_connections_changed = [this]() {
            Debug::Log("Connections changed - updating color pipeline");
            UpdateColorPipeline();
        };

        ImNodesEditorContext* nodes_editor_context = nullptr;

    }

void Application::RefreshCurrentFrame() {
        if (!video_player) return;

        // Get current position and re-seek to it to refresh the frame with new color pipeline
        // This matches what timeline scrubbing does and is the most reliable way to refresh
        double current_position = video_player->GetPosition();
        video_player->Seek(current_position);
        Debug::Log("Refreshed frame at position: " + std::to_string(current_position) + "s");
    }

void Application::UpdateColorPipeline() {
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
            qcview::NodeBase* node = node_manager->GetNode(node_id);
            if (!node) continue;

            switch (node->GetType()) {
            case qcview::NodeType::INPUT_COLORSPACE: {
                auto* csNode = dynamic_cast<qcview::InputColorSpaceNode*>(node);
                if (csNode) {
                    src_colorspace = csNode->GetColorSpace();
                    Debug::Log("Found Input ColorSpace: " + src_colorspace);
                }
                break;
            }
            case qcview::NodeType::LOOK: {
                auto* lookNode = dynamic_cast<qcview::LookNode*>(node);
                if (lookNode && !lookNode->GetLook().empty()) {  // Note: GetLook() not GetLookName()
                    if (!looks.empty()) looks += ", ";
                    looks += lookNode->GetLook();
                    Debug::Log("Found Look: " + lookNode->GetLook());
                }
                break;
            }
            case qcview::NodeType::OUTPUT_DISPLAY: {
                auto* displayNode = dynamic_cast<qcview::OutputDisplayNode*>(node);
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

    // Schedule a loading operation to happen after the current frame completes
    // This allows the overlay to render before the blocking operation
void Application::ScheduleLoadingOperation(const std::string& message, std::function<void()> callback) {
        loading_message_ = message;
        loading_callback_ = callback;
        loading_frames_ = 0;  // Will be set to 1 in the update loop
    }

bool Application::IsLoadingMedia() const { return !loading_message_.empty(); }
const std::string& Application::GetLoadingMessage() const { return loading_message_; }

    void Application::ExecuteLoadingCallback() {
        if (!loading_callback_) return;
        auto callback = std::move(loading_callback_);
        loading_message_.clear();
        loading_callback_ = nullptr;
        callback();
    }

    // Convenience wrapper for timeline imports (backwards compatibility)
    void Application::ScheduleImport(const std::string& path, const std::string& message) {
        ScheduleLoadingOperation(message, [this, path]() {
            if (project_manager) {
                project_manager->ImportTimeline(path);
            }
        });
    }

    void Application::Cleanup() {
        Debug::Log("=== CLEANUP STARTED ===");

        // =====================================================================
        // Phase 1: Save persistent state (must complete before anything else)
        // =====================================================================
        SaveSettings();
        SaveShortcuts();

        // =====================================================================
        // Phase 2: Release GPU resources (Metal/Vulkan textures leak if not
        //          explicitly freed — the OS does NOT reclaim them on exit)
        // =====================================================================

#ifdef QCVIEW_USE_VULKAN
        auto& vk_dev = qcview::VulkanDeviceManager::Instance();
        vkDeviceWaitIdle(vk_dev.GetDevice());
        ImGui_ImplVulkan_Shutdown();
#elif defined(QCVIEW_USE_METAL)
        // HDR swapchain calls ImGui_ImplMetal_SetHDRMode — must go before ImGui teardown
        metal_swapchain_.reset();

        qcview::ShutdownNativeMenuBar();
        {
            extern void ShutdownMetalImGui();
            ShutdownMetalImGui();
        }
#else
        ImGui_ImplOpenGL3_Shutdown();
#endif

#ifdef QCVIEW_USE_METAL
        extern void ShutdownImGuiOSXBackend();
        ShutdownImGuiOSXBackend();
#else
        ImGui_ImplGlfw_Shutdown();
#endif
        ImNodes::DestroyContext();
        qcview::Annotations::NanoVGContext::Instance().Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();

#ifdef QCVIEW_USE_VULKAN
        qcview::SetVulkanHDRSwapchain(nullptr);
        vulkan_swapchain_.reset();
        if (vulkan_annotation_renderer_) {
            vulkan_annotation_renderer_->Shutdown();
            vulkan_annotation_renderer_.reset();
        }
        qcview::VulkanTexturePool::Instance().Shutdown();
        vk_dev.Shutdown();
#elif defined(QCVIEW_USE_METAL)
        if (metal_annotation_renderer_) {
            metal_annotation_renderer_->Shutdown();
            metal_annotation_renderer_.reset();
        }
        qcview::MetalTexturePool::ThumbnailInstance().Shutdown();
        qcview::MetalTexturePool::Instance().Shutdown();
        qcview::MetalDeviceManager::Instance().Shutdown();
#endif

        // =====================================================================
        // Phase 3: Tear down window system
        // =====================================================================
#ifdef QCVIEW_USE_METAL
        MacOS_DestroyWindow();
#else
        glfwDestroyWindow(window);
        glfwTerminate();
#endif

#ifdef _WIN32
        qcview::HDROutputManager::Instance().Shutdown();
#endif

        // =====================================================================
        // Phase 4: Fast exit — skip thread joins entirely.
        //
        // Background threads (DirectEXRCache I/O, thumbnail workers, frame
        // cache, audio mixer) are all operating on process-private CPU memory.
        // The OS reclaims all of it on _exit(). GPU resources were already
        // released above. This avoids 1-3 seconds of blocking thread joins.
        // =====================================================================
        Debug::Log("=== CLEANUP COMPLETED (fast exit) ===");
        Debug::ShutdownLogging();
        _exit(0);
    }

    // ------------------------------------------------------------------------
    // FORCE RELOAD CURRENT MEDIA
    // Comprehensive cleanup and reload for pipeline/range changes.
    // Brute-force approach: fully exit all modes, then reload as if double-clicked.
    // ------------------------------------------------------------------------
    void Application::ForceReloadCurrentMedia() {
        Debug::Log("ForceReloadCurrentMedia: Starting comprehensive cleanup and reload");

        // Get current media item before cleanup
        qcview::MediaItem* current_item = project_manager ? project_manager->GetCurrentPlayingMediaItem() : nullptr;

        // If no item found via path, check for playlist using current_timeline_id
        // (playlists don't have a file path, so GetCurrentPlayingMediaItem won't find them)
        if (!current_item && project_manager && !current_timeline_id.empty()) {
            current_item = project_manager->GetPlaylistItem(current_timeline_id);
            if (current_item) {
                Debug::Log("ForceReloadCurrentMedia: Found playlist via current_timeline_id: " + current_item->name);
            }
        }

        if (!current_item) {
            Debug::Log("ForceReloadCurrentMedia: No current media item to reload");
            return;
        }

        // Store info we need after cleanup (timeline_id is used after clearing current_timeline_id)
        qcview::MediaType media_type = current_item->type;
        std::string item_name = current_item->name;
        std::string item_timeline_id = current_item->timeline_id;
        Debug::Log("ForceReloadCurrentMedia: Will reload " + item_name + " (type=" + std::to_string(static_cast<int>(media_type)) + ")");

        // === STEP 1: Clear mode flags FIRST to prevent callbacks from caching stale state ===
        otio_timeline_mode = false;
        otio_dual_view_mode = false;
        current_timeline_path.clear();
        current_timeline_id.clear();

        // === STEP 1.5: STOP all playback before any cleanup ===
        if (timeline_view) {
            if (auto* controller = timeline_view->GetPlaybackController()) {
                controller->Pause();
            }
        }
        if (scratch_timeline_controller) {
            scratch_timeline_controller->Pause();
        }

        // === STEP 2: Tell video_player it's no longer in timeline mode ===
        if (video_player) {
            video_player->SetTimelineMode(false, nullptr);
        }

        // === STEP 2.5: Clear all texture references BEFORE shutdown ===
        // This prevents stale texture IDs from being used after they're deleted.
        // The controllers own the textures - we must clear our references first.
        cached_dual_view_textures = {};

        // Queue color-corrected textures for deferred deletion
#if !defined(QCVIEW_USE_VULKAN) && !defined(QCVIEW_USE_METAL)
        // OpenGL path: queue GL texture deletions for main thread
        if (g_color_corrected_cache.left_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.left_texture);
        }
        if (g_color_corrected_cache.right_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.right_texture);
        }
        if (g_color_corrected_cache.cached_frame_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.cached_frame_texture);
        }
        if (g_color_corrected_cache.composite_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.composite_texture);
        }
        if (g_color_corrected_cache.prev_left_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_left_texture);
        }
        if (g_color_corrected_cache.prev_right_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_right_texture);
        }
        if (g_color_corrected_cache.prev_cached_frame_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_cached_frame_texture);
        }
        if (g_color_corrected_cache.prev_composite_texture != 0) {
            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_composite_texture);
        }
#endif
        // Reset the cache struct (Vulkan/Metal don't use this for color correction)
        g_color_corrected_cache = ColorCorrectedTextureCache{};

        // === STEP 3: Shutdown all playback controllers ===

        // Shutdown scratch timeline controller (dual view, scratch timelines)
        if (scratch_timeline_controller) {
            Debug::Log("ForceReloadCurrentMedia: Shutting down scratch_timeline_controller");
            scratch_timeline_controller->Shutdown();
            scratch_timeline_controller.reset();
        }

        // FULLY destroy timeline_view to ensure clean state (like a fresh double-click)
        // This is more aggressive than ShutdownPlayback() but ensures no stale GL resources
        if (timeline_view) {
            Debug::Log("ForceReloadCurrentMedia: Destroying timeline_view for clean reload");
            timeline_view.reset();  // Destructor calls ShutdownPlayback internally
        }


#ifdef _WIN32
        // CRITICAL: Process pending GL texture deletions from destroyed decoders
        // The D3D11 interop queues GL deletions for main thread processing.
        // We MUST process these before creating new textures to avoid ID reuse issues.
        qcview::D3D11VideoInterop::ProcessPendingGLDeletions();
#endif

        Debug::Log("ForceReloadCurrentMedia: Cleanup complete, initiating reload");

        // === STEP 4: Reload based on media type ===
        // current_item pointer is still valid (MediaItem lives in project_manager's pool)
        if (media_type == qcview::MediaType::VIDEO && project_manager) {
            if (auto callback = project_manager->GetVideoFileTimelineCallback()) {
                Debug::Log("ForceReloadCurrentMedia: Reloading video via callback");
                callback(current_item);
            }
        } else if ((media_type == qcview::MediaType::IMAGE_SEQUENCE ||
                    media_type == qcview::MediaType::EXR_SEQUENCE) && project_manager) {
            if (auto callback = project_manager->GetImageSequenceTimelineCallback()) {
                Debug::Log("ForceReloadCurrentMedia: Reloading image sequence via callback");
                callback(current_item);
            }
        } else if (media_type == qcview::MediaType::DUAL_VIEW && project_manager) {
            // Use copied timeline_id
            Debug::Log("ForceReloadCurrentMedia: Reloading dual view: " + item_timeline_id);
            project_manager->OpenDualViewInEditor(item_timeline_id);
        } else if (media_type == qcview::MediaType::PLAYLIST && project_manager) {
            if (auto callback = project_manager->GetPlaylistTimelineCallback()) {
                Debug::Log("ForceReloadCurrentMedia: Reloading playlist via callback");
                callback(current_item);
            }
        } else {
            Debug::Log("ForceReloadCurrentMedia: Unknown media type or no callback available");
        }

        // Skip viewport rendering for this frame to avoid using stale/recycled texture IDs
        // The new controller needs one frame to initialize and decode before we can render
        g_skip_viewport_render_frame = true;

        Debug::Log("ForceReloadCurrentMedia: Reload complete (skipping viewport render this frame)");
    }

    // Auto-save and close annotation mode if the user seeks/scrubs away.
    // Called before any user-initiated seek so drawings aren't lost.
    void Application::AutoSaveAnnotationOnSeek() {
        if (!viewport_annotator || !viewport_annotator->IsAnnotationMode()) return;

        Debug::Log("Auto-saving annotation before seek");

        // Finalize any active stroke being drawn
        auto active_stroke = viewport_annotator->FinalizeStroke();
        if (active_stroke) {
            current_annotation_strokes_.push_back(*active_stroke);
        }

        // Serialize and save
        std::string json_data = qcview::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);
        if (annotation_manager && !current_editing_timecode_.empty()) {
            annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);
            Debug::Log("Auto-saved " + std::to_string(current_annotation_strokes_.size()) + " strokes");
        }

        // Clear editing state
        current_annotation_strokes_.clear();
        current_editing_timecode_.clear();
        annotation_undo_stack_.clear();
        annotation_redo_stack_.clear();

        // Exit annotation mode
        viewport_annotator->SetMode(qcview::Annotations::ViewportMode::PLAYBACK);
        viewport_annotator->SetAllowInputInPopup(false);
        if (annotation_toolbar) annotation_toolbar->SetVisible(false);

        // Close edit modal if open
        if (annotation_panel && annotation_panel->IsEditModalOpen()) {
            annotation_panel->CloseEditModal();
        }
    }
