// application_run.cpp - Application::Run()

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/application.h"
#include "app/app_config.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/timecode.h"
#include "utils/debug_utils.h"
#include "utils/system_pressure_monitor.h"
#include "player/video_player.h"
#include "project/project_manager.h"
#include "project/media_item.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "timeline/timeline_cache.h"
#include "timeline/timeline_thumbnail_cache.h"
#include "timeline/timeline_commands.h"
#include "annotations/annotation_manager.h"
#include "annotations/viewport_annotator.h"
#include "annotations/annotation_renderer.h"
#include "annotations/nanovg_context.h"
#include "annotations/annotation_serializer.h"
#include "ui/annotation_panel.h"
#include "ui/timeline_manager.h"
#include "audio/audio_player.h"
#include "player/thumbnail_cache.h"
#include "player/shared_memory_pool.h"
#include "player/hw_context_manager.h"
#ifdef _WIN32
#include "hdr/hdr_output_manager.h"
#include "gpu/d3d11_video_interop.h"
#endif

// Externs for globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern float g_font_scale;
extern bool cache_enabled;
extern CacheSettings cache_settings;
extern bool otio_timeline_mode;
extern bool otio_dual_view_mode;
extern bool otio_dual_view_split_mode;
extern float otio_dual_view_split_pos;
extern bool otio_dual_view_left_audio;
extern bool otio_dual_view_right_audio;
extern bool g_skip_viewport_render_frame;
extern bool g_force_reload_pending;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern std::unique_ptr<qcview::MediaLinker> media_linker;
extern std::unique_ptr<qcview::TimelinePlaybackController> scratch_timeline_controller;
extern qcview::TimelinePlaybackController::DualViewTextures cached_dual_view_textures;
extern std::vector<GLuint> g_pending_texture_deletions;
extern std::unique_ptr<qcview::TimelineCommandManager> timeline_command_manager;
extern std::unique_ptr<qcview::TimelineThumbnailCache> timeline_thumbnail_cache;
extern bool timeline_thumbnail_cache_clear_deferred;
extern std::unique_ptr<qcview::SystemPressureMonitor> pressure_monitor;
extern bool auto_play_buffering;
extern std::chrono::steady_clock::time_point auto_play_buffer_start;
extern bool pending_seek_cache_start;
extern std::chrono::steady_clock::time_point seek_cache_start_timer;
extern std::unique_ptr<OCIOConfigManager> ocio_manager;
extern bool show_transcode_progress;
extern std::atomic<int> transcode_current_frame;
extern std::atomic<int> transcode_total_frames;

// Free functions defined in main.cpp
qcview::DirectEXRCacheConfig GetCurrentEXRCacheConfig();
qcview::TimelineCacheConfig GetCurrentTimelineCacheConfig();

// Accent color helpers
ImVec4 GetWindowsAccentColor();

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
extern ColorCorrectedTextureCache g_color_corrected_cache;

// More globals from main.cpp
extern bool mouse_is_idle;
extern ImVec2 last_mouse_pos;
extern std::chrono::steady_clock::time_point last_mouse_move_time;
extern bool in_emergency_mode;
extern qcview::SystemPressureStatus last_pressure_status;
extern bool exr_cache_was_active;
extern std::string exr_video_path_before_shutdown;
extern std::string stats_bar_notification_message;
extern bool show_notification_permanent;
extern std::chrono::steady_clock::time_point notification_start_time;
extern float notification_timeout_seconds;
extern bool show_pressure_critical_dialog;
extern std::string current_timeline_path;
extern std::string current_timeline_id;
extern std::chrono::steady_clock::time_point g_last_frame_time;

// Frame rate limiter constants (defined in main.cpp)
constexpr bool g_limit_imgui_fps = true;
constexpr double g_target_frame_time = 1.0 / 60.00;

// Free functions
qcview::DirectEXRCacheConfig GetCurrentEXRCacheConfig();
qcview::TimelineCacheConfig GetCurrentTimelineCacheConfig();
std::string GetAssetPath(const std::string& relative_path);

void Application::Run() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // Process deferred fullscreen toggle AFTER all events are processed
            if (pending_fullscreen_toggle) {
                pending_fullscreen_toggle = false;
                ToggleFullscreen();
            }

            // Process auto-play: simplified logic for D3D11 video playback
            // - Video content (VIDEO_FILE, PLAYLIST, DUAL_VIEW with video): 200ms delay
            // - Image content with buffer wait enabled: wait for buffer fill (handled by Play())
            if (auto_play_buffering && cache_settings.auto_play_on_load) {
                auto* controller = timeline_view ? timeline_view->GetEffectivePlaybackController() : nullptr;

                if (controller && !controller->IsPlaying()) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - auto_play_buffer_start).count();

                    // Simple 200ms delay before auto-play (lets cache start filling)
                    if (elapsed >= 200) {
                        auto_play_buffering = false;
                        controller->Play();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(true);
                        }
                        Debug::Log("Auto-play: starting after 200ms delay");
                    }
                } else if (!controller) {
                    // No controller - cancel auto-play
                    auto_play_buffering = false;
                }
            }

            // Process delayed seek cache start (1000ms after video load, respects mouse activity)
            if (pending_seek_cache_start) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - seek_cache_start_timer).count();

                // Only start if enough time has passed AND mouse is idle (no user interaction)
                if (elapsed >= 1000 && mouse_is_idle) {
                    pending_seek_cache_start = false;
                    if (project_manager) {
                        FrameCache* frame_cache = project_manager->GetCurrentVideoCache();
                        if (frame_cache) {
                            // Start background extraction now (user is idle)
                            frame_cache->StartDelayedBackgroundExtraction();
                            Debug::Log("Seek cache: Starting background extraction after 1s delay (mouse idle)");
                        }
                    }
                }
                // If user is still active after 1s, keep waiting (timer keeps running)
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
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                                Debug::Log("WARNING: Cannot restore EXR cache - sequence files empty");
                            }

                            exr_cache_was_active = false;
                            exr_video_path_before_shutdown.clear();
                        } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
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
                // NOTE: UpdateVideoTexture() is now called from ProcessPendingTextureUploads()
                // to consolidate all GL operations before ImGui::NewFrame()

                // Timeline range playback - handles both loop and play-once modes
                // Loop ON: cycle back to boundary start (In or 0)
                // Loop OFF: stop at boundary end (Out or timeline end)
                if (otio_timeline_mode && timeline_view && video_player->IsPlaying()) {
                    auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();

                    // Cache-based mode: use cache boundaries
                    {
                        auto* cache = playback_ctrl ? playback_ctrl->GetCache() : nullptr;

                        if (cache) {
                            double fps = timeline_view->GetFrameRate();
                            double tl_current = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;

                            if (fps > 0) {
                                int current_frame = static_cast<int>(std::round(tl_current * fps));
                                int boundary_start = cache->GetBoundaryStart();
                                int boundary_end = cache->GetBoundaryEnd();
                                bool is_looping = timeline_view->IsTimelineLooping();

                                // Check if at or past boundary end
                                if (current_frame >= boundary_end) {
                                    if (is_looping) {
                                        // Loop back to boundary start
                                        double start_time = boundary_start / fps;
                                        video_player->Seek(start_time);

                                        Debug::Log("Timeline loop: jumped to frame " + std::to_string(boundary_start));
                                    } else {
                                        // Stop at boundary end
                                        video_player->Pause();
                                        Debug::Log("Timeline play-once: stopped at frame " + std::to_string(boundary_end));
                                    }
                                }
                            }
                        }
                    }
                }

                // Sync loop boundaries to TimelineCache for circular cache window
                // Boundaries are ALWAYS defined: [In,Out] if set, otherwise [0,total-1]
                // IMPORTANT: In dual view mode, BOTH caches need the same boundaries
                // Use the MAX of both cache durations to ensure all frames are valid
                if (otio_timeline_mode && timeline_view) {
                    auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();
                    auto* left_cache = playback_ctrl ? playback_ctrl->GetCache() : nullptr;
                    auto* right_cache = playback_ctrl ? playback_ctrl->GetRightCache() : nullptr;

                    if (left_cache) {
                        double fps = timeline_view->GetFrameRate();
                        if (fps > 0) {
                            int in_frame, out_frame;
                            if (timeline_view->HasTimelineInOutPoints()) {
                                in_frame = static_cast<int>(timeline_view->GetTimelineInPoint() * fps);
                                out_frame = static_cast<int>(timeline_view->GetTimelineOutPoint() * fps);
                            } else {
                                // No In/Out points - use full timeline as boundaries
                                // For dual view: use MAX of both cache durations to cover all content
                                in_frame = 0;
                                int left_frames = left_cache->GetStats().total_timeline_frames;
                                int right_frames = right_cache ? right_cache->GetStats().total_timeline_frames : 0;
                                out_frame = std::max(left_frames, right_frames) - 1;
                            }

                            if (out_frame > 0) {
                                // Set boundaries on LEFT cache
                                left_cache->SetLoopBoundaries(in_frame, out_frame);

                                // Set boundaries on RIGHT cache too (dual view mode)
                                if (right_cache) {
                                    right_cache->SetLoopBoundaries(in_frame, out_frame);
                                }
                            }
                        }
                    }
                }

                // Process pending thumbnail uploads (async -> GL texture upload on main thread)
                if (video_player->HasThumbnailCache()) {
                    video_player->GetThumbnailCache()->ProcessPendingUploads();
                }

                // Process timeline thumbnail cache uploads (for trim/slip preview)
                if (timeline_thumbnail_cache) {
                    // Handle deferred clear - must happen BEFORE ProcessPendingUploads
                    // This ensures textures deleted at end of previous frame are cleared
                    // before any new texture operations, avoiding font atlas corruption
                    if (timeline_thumbnail_cache_clear_deferred) {
                        timeline_thumbnail_cache->Clear();
                        timeline_thumbnail_cache_clear_deferred = false;
                    }
                    timeline_thumbnail_cache->ProcessPendingUploads();
                }

                // Update timeline manager first to handle cache logic
                if (timeline_manager) {
                    timeline_manager->Update(video_player.get());
                }

                // Only update fast seeking if timeline manager isn't using cached frames
                // In timeline mode, skip this check since TimelineCache handles frames independently
                bool skip_cache_check = video_player && video_player->IsInTimelineMode();
                if (!timeline_manager || skip_cache_check || !timeline_manager->IsHoldingCachedFrame()) {
                    video_player->UpdateFastSeek();
                }

                // Process video texture uploads BEFORE ImGui frame to avoid GL state corruption
                video_player->ProcessPendingTextureUploads();
            }

            // Process deferred reload BEFORE any texture operations
            // This ensures clean GL state when switching pipeline modes
            if (g_force_reload_pending) {
                g_force_reload_pending = false;
                ForceReloadCurrentMedia();
                // Skip the rest of texture processing this frame - new controller needs to initialize
                g_skip_viewport_render_frame = true;
            }

            // Process scratch timeline texture uploads (for dual view, etc.)
            if (scratch_timeline_controller) {
                scratch_timeline_controller->ProcessPendingUploads();
                // Pre-render dual view textures to avoid GL ops during ImGui render
                if (scratch_timeline_controller->IsDualViewMode()) {
                    cached_dual_view_textures = scratch_timeline_controller->UpdateDualView();
                }
            }

            // Pre-render color-corrected textures with GPU fence sync
            // Double-buffered: keep previous frame as fallback if GPU isn't done with new textures
            if (video_player && video_player->HasColorPipeline()) {
                // Step 1: Check if previous frame's fence completed (non-blocking)
                if (g_color_corrected_cache.upload_fence) {
                    GLenum result = glClientWaitSync(g_color_corrected_cache.upload_fence, 0, 0);
                    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
                        // GPU finished - current textures are ready, delete previous
                        g_color_corrected_cache.current_ready = true;
                        glDeleteSync(g_color_corrected_cache.upload_fence);
                        g_color_corrected_cache.upload_fence = nullptr;

                        // Now safe to delete previous frame's textures
                        if (g_color_corrected_cache.prev_left_texture != 0) {
                            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_left_texture);
                            g_color_corrected_cache.prev_left_texture = 0;
                        }
                        if (g_color_corrected_cache.prev_right_texture != 0) {
                            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_right_texture);
                            g_color_corrected_cache.prev_right_texture = 0;
                        }
                        if (g_color_corrected_cache.prev_cached_frame_texture != 0) {
                            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_cached_frame_texture);
                            g_color_corrected_cache.prev_cached_frame_texture = 0;
                        }
                        if (g_color_corrected_cache.prev_composite_texture != 0) {
                            g_pending_texture_deletions.push_back(g_color_corrected_cache.prev_composite_texture);
                            g_color_corrected_cache.prev_composite_texture = 0;
                        }
                    } else {
                        // GPU still working - use previous frame's textures
                        g_color_corrected_cache.current_ready = false;
                        // Skip creating new textures this frame to avoid memory buildup
                        goto skip_color_correction;
                    }
                }

                // Step 2: Move current textures to previous (don't delete yet - they're our fallback)
                g_color_corrected_cache.prev_left_texture = g_color_corrected_cache.left_texture;
                g_color_corrected_cache.prev_right_texture = g_color_corrected_cache.right_texture;
                g_color_corrected_cache.prev_cached_frame_texture = g_color_corrected_cache.cached_frame_texture;
                g_color_corrected_cache.prev_composite_texture = g_color_corrected_cache.composite_texture;
                g_color_corrected_cache.left_texture = 0;
                g_color_corrected_cache.right_texture = 0;
                g_color_corrected_cache.cached_frame_texture = 0;
                g_color_corrected_cache.composite_texture = 0;

                // Step 3: Create new color-corrected textures
                // Pre-render dual view color-corrected textures
                if (scratch_timeline_controller && scratch_timeline_controller->IsDualViewMode()) {
                    if (cached_dual_view_textures.is_unified) {
                        // Unified composite mode: apply OCIO to the single composite texture
                        if (cached_dual_view_textures.composite_texture != 0 &&
                            cached_dual_view_textures.composite_width > 0 &&
                            cached_dual_view_textures.composite_height > 0) {
                            g_color_corrected_cache.composite_texture = video_player->CreateColorCorrectedTexture(
                                cached_dual_view_textures.composite_texture,
                                cached_dual_view_textures.composite_width,
                                cached_dual_view_textures.composite_height,
                                cached_dual_view_textures.composite_width,
                                cached_dual_view_textures.composite_height
                            );
                            g_color_corrected_cache.composite_width = cached_dual_view_textures.composite_width;
                            g_color_corrected_cache.composite_height = cached_dual_view_textures.composite_height;
                        }
                    } else {
                        // Legacy mode: apply OCIO to left/right textures separately
                        if (cached_dual_view_textures.left_texture != 0 &&
                            cached_dual_view_textures.left_width > 0 &&
                            cached_dual_view_textures.left_height > 0) {
                            g_color_corrected_cache.left_texture = video_player->CreateColorCorrectedTexture(
                                cached_dual_view_textures.left_texture,
                                cached_dual_view_textures.left_width,
                                cached_dual_view_textures.left_height,
                                cached_dual_view_textures.left_width,
                                cached_dual_view_textures.left_height
                            );
                            g_color_corrected_cache.left_width = cached_dual_view_textures.left_width;
                            g_color_corrected_cache.left_height = cached_dual_view_textures.left_height;
                        }
                        if (cached_dual_view_textures.right_texture != 0 &&
                            cached_dual_view_textures.right_width > 0 &&
                            cached_dual_view_textures.right_height > 0) {
                            g_color_corrected_cache.right_texture = video_player->CreateColorCorrectedTexture(
                                cached_dual_view_textures.right_texture,
                                cached_dual_view_textures.right_width,
                                cached_dual_view_textures.right_height,
                                cached_dual_view_textures.right_width,
                                cached_dual_view_textures.right_height
                            );
                            g_color_corrected_cache.right_width = cached_dual_view_textures.right_width;
                            g_color_corrected_cache.right_height = cached_dual_view_textures.right_height;
                        }
                    }
                }

                // Pre-render cached frame color-corrected texture (for gap placeholders)
                if (timeline_manager && timeline_manager->IsHoldingCachedFrame()) {
                    GLuint cached_id = 0;
                    int cached_w = 0, cached_h = 0;
                    double current_time = timeline_manager->GetUIPosition();
                    if (timeline_manager->GetCachedFrameForScrubbing(current_time, cached_id, cached_w, cached_h) &&
                        cached_id != 0 && cached_w > 0 && cached_h > 0) {
                        g_color_corrected_cache.cached_frame_texture = video_player->CreateColorCorrectedTexture(
                            cached_id, cached_w, cached_h, cached_w, cached_h
                        );
                        g_color_corrected_cache.cached_frame_width = cached_w;
                        g_color_corrected_cache.cached_frame_height = cached_h;
                        g_color_corrected_cache.cached_frame_source_id = cached_id;
                    }
                }

                // Step 4: Create fence after all texture uploads
                g_color_corrected_cache.upload_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                g_color_corrected_cache.current_ready = false;  // Will be checked next frame

                skip_color_correction:;
            }

            // Process pending texture deletions BEFORE ImGui frame (avoid GL ops during render)
            if (!g_pending_texture_deletions.empty()) {
                glDeleteTextures(static_cast<GLsizei>(g_pending_texture_deletions.size()),
                                 g_pending_texture_deletions.data());
                g_pending_texture_deletions.clear();
            }

            // Frame rate limiter - skip ImGui rendering if not enough time has passed
            // This limits UI updates while video processing continues at full rate
            bool should_render_frame = true;
            if (g_limit_imgui_fps) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - g_last_frame_time).count();
                if (elapsed < g_target_frame_time) {
                    should_render_frame = false;
                    // Small sleep to avoid busy-waiting (saves CPU)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            if (should_render_frame) {
                g_last_frame_time = std::chrono::steady_clock::now();

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                // Process keyboard shortcuts after ImGui state is updated
                // (fixes spacebar triggering multiple times when frames are skipped)
                HandleKeyboardShortcuts();

                CreateDockingLayout();

                // Check if any popup is open (e.g., color picker) before rendering
                // Popups render in ImGui layer, so we need to skip NanoVG when they're open
                // to prevent NanoVG from rendering on top of them
                // Exception: when modal edit is open, we still want NanoVG to render strokes on the modal image
                {
                    bool any_popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                    bool modal_edit = annotation_panel && annotation_panel->IsEditModalOpen();
                    nvg_popup_open_ = any_popup && !modal_edit;
                }

                ImGui::Render();
                int display_w, display_h;
                glfwGetFramebufferSize(window, &display_w, &display_h);

#ifdef _WIN32
                // HDR presentation: render to D3D11-backed FBO, present via DXGI
                auto& hdr = qcview::HDROutputManager::Instance();

                // Handle resize BEFORE BeginFrame to avoid invalidating the FBO mid-frame
                // This prevents crashes when maximizing/restoring the window
                if (display_w > 0 && display_h > 0) {
                    hdr.OnResize(display_w, display_h);
                }

                // Single-pass HDR rendering (video and UI on same surface)
                // UI colors are adjusted for HDR brightness via ImGui style conversion
                GLuint hdr_fbo = hdr.BeginFrame();  // Returns FBO or 0 if bypass/unavailable

                if (hdr_fbo != 0) {
                    // Rendering to HDR swapchain FBO
                    glViewport(0, 0, display_w, display_h);
                    ApplyBackgroundColor();
                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                    // Render NanoVG annotations on top of ImGui (deferred rendering)
                    // Skip when popup is open so color picker renders above annotations
                    if (pending_nvg_render_ && annotation_renderer && !nvg_strokes_to_render_.empty() && !nvg_popup_open_) {
                        // Convert from screen coordinates to window-relative coordinates
                        // nvg_display_pos_ is in absolute screen coords, NanoVG renders to window framebuffer
                        int win_x, win_y;
                        glfwGetWindowPos(window, &win_x, &win_y);

                        // Scale to framebuffer physical pixels
                        ImGuiIO& nvg_io = ImGui::GetIO();
                        float dpi_scale = nvg_io.DisplayFramebufferScale.x;
                        float scaled_pos_x = (nvg_display_pos_.x - win_x) * dpi_scale;
                        float scaled_pos_y = (nvg_display_pos_.y - win_y) * dpi_scale;
                        float scaled_size_x = nvg_display_size_.x * dpi_scale;
                        float scaled_size_y = nvg_display_size_.y * dpi_scale;

                        // Calculate line width scale: viewport size / video size
                        // This makes strokes scale proportionally with the viewport
                        float line_width_scale = (nvg_video_width_ > 0) ? (scaled_size_x / nvg_video_width_) : 1.0f;

                        // When modal is open, clip strokes to the modal image area
                        bool modal_scissor = annotation_panel && annotation_panel->IsEditModalOpen();
                        if (modal_scissor) {
                            glEnable(GL_SCISSOR_TEST);
                            glScissor((GLint)scaled_pos_x, display_h - (GLint)(scaled_pos_y + scaled_size_y),
                                      (GLsizei)scaled_size_x, (GLsizei)scaled_size_y);
                        }

                        // Pass DPI scale as devicePixelRatio for proper anti-aliasing
                        annotation_renderer->BeginFrame((float)display_w, (float)display_h, dpi_scale);
                        for (const auto& stroke : nvg_strokes_to_render_) {
                            annotation_renderer->RenderActiveStroke(
                                stroke, scaled_pos_x, scaled_pos_y,
                                scaled_size_x, scaled_size_y, true, line_width_scale
                            );
                        }
                        annotation_renderer->EndFrame();

                        if (modal_scissor) {
                            glDisable(GL_SCISSOR_TEST);
                        }
                        pending_nvg_render_ = false;
                    }

                    // Multi-viewport support (secondary windows still use OpenGL)
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                        GLFWwindow* backup_current_context = glfwGetCurrentContext();
                        ImGui::UpdatePlatformWindows();
                        ImGui::RenderPlatformWindowsDefault();
                        glfwMakeContextCurrent(backup_current_context);
                    }

                    hdr.EndFrame(true);  // Present with vsync
                } else {
                    // Fallback: standard OpenGL presentation
                    glViewport(0, 0, display_w, display_h);
                    ApplyBackgroundColor();
                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                    // Render NanoVG annotations on top of ImGui (deferred rendering)
                    // Skip when popup is open so color picker renders above annotations
                    if (pending_nvg_render_ && annotation_renderer && !nvg_strokes_to_render_.empty() && !nvg_popup_open_) {
                        // Convert from screen coordinates to window-relative coordinates
                        int win_x, win_y;
                        glfwGetWindowPos(window, &win_x, &win_y);

                        // Scale to framebuffer physical pixels
                        ImGuiIO& nvg_io = ImGui::GetIO();
                        float dpi_scale = nvg_io.DisplayFramebufferScale.x;
                        float scaled_pos_x = (nvg_display_pos_.x - win_x) * dpi_scale;
                        float scaled_pos_y = (nvg_display_pos_.y - win_y) * dpi_scale;
                        float scaled_size_x = nvg_display_size_.x * dpi_scale;
                        float scaled_size_y = nvg_display_size_.y * dpi_scale;

                        // Calculate line width scale: viewport size / video size
                        float line_width_scale = (nvg_video_width_ > 0) ? (scaled_size_x / nvg_video_width_) : 1.0f;

                        // When modal is open, clip strokes to the modal image area
                        bool modal_scissor = annotation_panel && annotation_panel->IsEditModalOpen();
                        if (modal_scissor) {
                            glEnable(GL_SCISSOR_TEST);
                            glScissor((GLint)scaled_pos_x, display_h - (GLint)(scaled_pos_y + scaled_size_y),
                                      (GLsizei)scaled_size_x, (GLsizei)scaled_size_y);
                        }

                        // Pass DPI scale as devicePixelRatio for proper anti-aliasing
                        annotation_renderer->BeginFrame((float)display_w, (float)display_h, dpi_scale);
                        for (const auto& stroke : nvg_strokes_to_render_) {
                            annotation_renderer->RenderActiveStroke(
                                stroke, scaled_pos_x, scaled_pos_y,
                                scaled_size_x, scaled_size_y, true, line_width_scale
                            );
                        }
                        annotation_renderer->EndFrame();

                        if (modal_scissor) {
                            glDisable(GL_SCISSOR_TEST);
                        }
                        pending_nvg_render_ = false;
                    }

                    ImGuiIO& io = ImGui::GetIO();
                    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                        GLFWwindow* backup_current_context = glfwGetCurrentContext();
                        ImGui::UpdatePlatformWindows();
                        ImGui::RenderPlatformWindowsDefault();
                        glfwMakeContextCurrent(backup_current_context);
                    }

                    glfwSwapBuffers(window);
                }
#else
                glViewport(0, 0, display_w, display_h);
                ApplyBackgroundColor();
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                // Render NanoVG annotations on top of ImGui (deferred rendering)
                // Skip when popup is open so color picker renders above annotations
                if (pending_nvg_render_ && annotation_renderer && !nvg_strokes_to_render_.empty() && !nvg_popup_open_) {
                    // Convert from screen coordinates to window-relative coordinates
                    int win_x, win_y;
                    glfwGetWindowPos(window, &win_x, &win_y);

                    // Scale to framebuffer physical pixels
                    ImGuiIO& nvg_io = ImGui::GetIO();
                    float dpi_scale = nvg_io.DisplayFramebufferScale.x;
                    float scaled_pos_x = (nvg_display_pos_.x - win_x) * dpi_scale;
                    float scaled_pos_y = (nvg_display_pos_.y - win_y) * dpi_scale;
                    float scaled_size_x = nvg_display_size_.x * dpi_scale;
                    float scaled_size_y = nvg_display_size_.y * dpi_scale;

                    // Calculate line width scale: viewport size / video size
                    float line_width_scale = (nvg_video_width_ > 0) ? (scaled_size_x / nvg_video_width_) : 1.0f;

                    // When modal is open, clip strokes to the modal image area
                    bool modal_scissor = annotation_panel && annotation_panel->IsEditModalOpen();
                    if (modal_scissor) {
                        glEnable(GL_SCISSOR_TEST);
                        glScissor((GLint)scaled_pos_x, display_h - (GLint)(scaled_pos_y + scaled_size_y),
                                  (GLsizei)scaled_size_x, (GLsizei)scaled_size_y);
                    }

                    // Pass DPI scale as devicePixelRatio for proper anti-aliasing
                    annotation_renderer->BeginFrame((float)display_w, (float)display_h, dpi_scale);
                    for (const auto& stroke : nvg_strokes_to_render_) {
                        annotation_renderer->RenderActiveStroke(
                            stroke, scaled_pos_x, scaled_pos_y,
                            scaled_size_x, scaled_size_y, true, line_width_scale
                        );
                    }
                    annotation_renderer->EndFrame();

                    if (modal_scissor) {
                        glDisable(GL_SCISSOR_TEST);
                    }
                    pending_nvg_render_ = false;
                }

                ImGuiIO& io = ImGui::GetIO();
                if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                    GLFWwindow* backup_current_context = glfwGetCurrentContext();
                    ImGui::UpdatePlatformWindows();
                    ImGui::RenderPlatformWindowsDefault();
                    glfwMakeContextCurrent(backup_current_context);
                }

                glfwSwapBuffers(window);
#endif
            }

            // Handle pending loading operation AFTER frame is fully rendered
            if (IsLoadingMedia()) {
                if (loading_frames_ == 0) {
                    // Just scheduled - wait one frame for overlay to show
                    loading_frames_ = 1;
                } else {
                    loading_frames_--;
                    if (loading_frames_ == 0) {
                        // Overlay has been shown - execute the callback
                        ExecuteLoadingCallback();
                    }
                }
            }
        }
    }
