#include "frame_cache.h"
#include "media_background_extractor.h"
#include "video_player.h"
#include "../metadata/video_metadata.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <cmath>
#ifdef _WIN32
#include <Windows.h>
#endif

// FFmpeg time base constant
#ifndef AV_TIME_BASE
#define AV_TIME_BASE 1000000
#endif

// Removed: Disk cache using statements (simplified to RAM-only cache)


// GPU conversion strategy removed - no longer needed with background extractor

// ============================================================================
// CachedFrame Implementation
// ============================================================================

CachedFrame::~CachedFrame() {
    ReleaseTexture();
}

CachedFrame::CachedFrame(CachedFrame&& other) noexcept
    : texture_id(other.texture_id)
    , width(other.width)
    , height(other.height)
    , timestamp(other.timestamp)
    // Removed: memory_size copying (memory-based eviction removed)
    , last_accessed(other.last_accessed)
    , is_valid(other.is_valid)
    , pixel_data(std::move(other.pixel_data))
    , texture_created(other.texture_created)
    , pipeline_mode(other.pipeline_mode)
{
    other.texture_id = 0;
    other.width = 0;
    other.height = 0;
    // Removed: memory_size reset (memory-based eviction removed)
    other.is_valid = false;
    other.texture_created = false;
}

CachedFrame& CachedFrame::operator=(CachedFrame&& other) noexcept {
    if (this != &other) {
        ReleaseTexture();
        
        texture_id = other.texture_id;
        width = other.width;
        height = other.height;
        timestamp = other.timestamp;
        // Removed: memory_size assignment (memory-based eviction removed)
        last_accessed = other.last_accessed;
        is_valid = other.is_valid;
        pixel_data = std::move(other.pixel_data);
        texture_created = other.texture_created;
        pipeline_mode = other.pipeline_mode;

        other.texture_id = 0;
        other.width = 0;
        other.height = 0;
        // Removed: memory_size reset (memory-based eviction removed)
        other.is_valid = false;
        other.texture_created = false;
    }
    return *this;
}

void CachedFrame::CreateTexture(int w, int h, const void* data, PipelineMode pipeline_mode) {
    ReleaseTexture(); // Clean up any existing texture

    // Get pipeline-specific format configuration
    auto it = PIPELINE_CONFIGS.find(pipeline_mode);
    if (it == PIPELINE_CONFIGS.end()) {
        pipeline_mode = PipelineMode::NORMAL;  // Fallback
        it = PIPELINE_CONFIGS.find(pipeline_mode);
    }

    const PipelineConfig& config = it->second;

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Use pipeline-appropriate texture format
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, w, h, 0, GL_RGBA, config.data_type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    width = w;
    height = h;
    // Removed: memory_size calculation (memory-based eviction removed)
    is_valid = true;
    last_accessed = std::chrono::steady_clock::now();
}

bool CachedFrame::EnsureTextureCreated() {
    if (texture_created && texture_id != 0) {
        return true; // Already created
    }
    
    if (pixel_data.empty()) {
        // Debug removed
        return false;
    }
    
    // Debug::Log("CachedFrame: Creating texture on main thread " + 
    //            std::to_string(width) + "x" + std::to_string(height) + 
    //            " from " + std::to_string(pixel_data.size()) + " bytes");
    
    // Create texture from stored pixel data
    CreateTexture(width, height, pixel_data.data(), pipeline_mode);
    
    // Check for errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        // Debug removed
        return false;
    }
    
    if (texture_id == 0) {
        // Debug removed
        return false;
    }
    
    texture_created = true;
    
    // Clear pixel data to save memory now that texture is created
    pixel_data.clear();
    pixel_data.shrink_to_fit();
    
    // Debug::Log("CachedFrame: Successfully created texture " + std::to_string(texture_id) + " on main thread");
    return true;
}

void CachedFrame::ReleaseTexture() {
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
        texture_id = 0;
    }
    width = 0;
    height = 0;
    // Removed: memory_size reset (memory-based eviction removed)
    is_valid = false;
}

// ============================================================================
// FrameCache Implementation
// ============================================================================

FrameCache::FrameCache(const CacheConfig& cfg) : config(cfg) {
    // IMMEDIATE debug output to confirm constructor is called
    // Debug::Log("*** FrameCache CONSTRUCTOR CALLED ***");
    // CacheDebugLog("Initializing FrameCache with " + std::to_string(config.max_cache_size_mb) + "MB cache");

    // Create MediaBackgroundExtractor for all media types (video + image sequences)
    MediaBackgroundExtractor::ExtractorConfig extractor_config;
    extractor_config.pipeline_mode = config.pipeline_mode;
    extractor_config.max_batch_size = config.max_batch_size;
    extractor_config.max_concurrent_batches = config.max_concurrent_batches;
    extractor_config.pause_during_playback = config.pause_during_playback;
    extractor_config.hw_config.mode = config.enable_nvidia_decode ? HardwareDecodeMode::NVDEC : HardwareDecodeMode::D3D11VA;

    background_extractor = std::make_unique<MediaBackgroundExtractor>(this, extractor_config);
    Debug::Log("FrameCache: Created MediaBackgroundExtractor");

    // Removed: Disk cache initialization (simplified to RAM-only cache)

    // Start background caching thread
    background_thread_active = true;
    background_thread = std::thread(&FrameCache::BackgroundCacheWorker, this);

    // Set thread priority (Windows-specific)
#ifdef _WIN32
    if (config.background_thread_priority < 0) {
        SetThreadPriority(background_thread.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
    }
#endif
}


FrameCache::~FrameCache() {
    Debug::Log("FrameCache: Destructor - stopping background thread");

    // Stop background thread
    Debug::Log("FrameCache: Setting shutdown_requested flag");
    shutdown_requested = true;

    if (background_thread.joinable()) {
        Debug::Log("FrameCache: Waiting for background thread to join...");
        background_thread.join();
        Debug::Log("FrameCache: Background thread joined successfully");
    } else {
        Debug::Log("FrameCache: Background thread was not joinable");
    }

    // Clean up MediaBackgroundExtractor (has worker threads)
    Debug::Log("FrameCache: Cleaning up MediaBackgroundExtractor...");
    if (background_extractor) {
        background_extractor.reset();  // Destructor calls Shutdown()
        Debug::Log("FrameCache: MediaBackgroundExtractor destroyed");
    }

    // Clean up all cached frames
    Debug::Log("FrameCache: Clearing cached frames...");
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        int scrub_count = static_cast<int>(scrub_cache.size());
        int keyframe_count = static_cast<int>(keyframe_cache.size());
        scrub_cache.clear();
        keyframe_cache.clear();
        Debug::Log("FrameCache: Cleared " + std::to_string(scrub_count) + " scrub frames and " + std::to_string(keyframe_count) + " keyframes");
    }

    Debug::Log("FrameCache: Destructor complete");
}

bool FrameCache::GetCachedFrame(double timestamp, GLuint& texture_id, int& width, int& height) {
    std::lock_guard<std::mutex> lock(cache_mutex);

    if (cached_video_player == nullptr) {
        cache_misses++;
        return false;
    }

    double fps = cached_video_player->GetFrameRate();
    int target_frame = TimestampToFrameNumber(timestamp, fps);

    // RAM Cache - Try exact match
    if (GetFrameFromRAM(target_frame, texture_id, width, height)) {
        //Debug::Log("GetCachedFrame: RAM CACHE HIT for frame " + std::to_string(target_frame));
        cache_hits++;
        return true;
    }

    // Look for nearest cached frame in RAM within tolerance for smooth scrubbing
    int tolerance = 5; // Look within ±5 frames
    for (int offset = 1; offset <= tolerance; ++offset) {
        // Check frame before
        if (GetFrameFromRAM(target_frame - offset, texture_id, width, height)) {
            //Debug::Log("GetCachedFrame: RAM CACHE HIT (nearby) for frame " + std::to_string(target_frame - offset));
            cache_hits++;
            return true;
        }
        // Check frame after
        if (GetFrameFromRAM(target_frame + offset, texture_id, width, height)) {
            //Debug::Log("GetCachedFrame: RAM CACHE HIT (nearby) for frame " + std::to_string(target_frame + offset));
            cache_hits++;
            return true;
        }
    }
    
    // Check keyframe cache for approximate match
    if (config.enable_keyframe_cache) {
        // Find nearest keyframe (simplified - would need actual keyframe detection)
        int keyframe_stride = static_cast<int>(fps * 2.0); // Every 2 seconds
        int nearest_keyframe = (target_frame / keyframe_stride) * keyframe_stride;
        
        auto keyframe_it = keyframe_cache.find(nearest_keyframe);
        if (keyframe_it != keyframe_cache.end() && keyframe_it->second->is_valid) {
            texture_id = keyframe_it->second->texture_id;
            width = keyframe_it->second->width;
            height = keyframe_it->second->height;
            keyframe_it->second->last_accessed = std::chrono::steady_clock::now();
            cache_hits++;
            return true;
        }
    }
    
    cache_misses++;
    // CacheDebugLog("CACHE MISS! No cached frame for " + std::to_string(target_frame) + 
    //              " (timestamp " + std::to_string(timestamp) + "s)");
    return false;
}

void FrameCache::UpdateScrubPosition(double timestamp, VideoPlayer* video_player) {
    double previous_position = current_scrub_position.load();
    current_scrub_position = timestamp;

    // Notify background extractor of playhead position change
    if (background_extractor && std::abs(timestamp - previous_position) > 0.1) { // >0.1 second change for responsive timeline
        background_extractor->SetPlayheadPosition(timestamp);
    }

    // Only update cached_video_player if a valid one is provided
    if (video_player != nullptr) {
        cached_video_player = video_player;
        // CacheDebugLog("UpdateScrubPosition: Updated cached_video_player at timestamp " + std::to_string(timestamp)); // Commented out to reduce debug spam
    } else {
        // CacheDebugLog("UpdateScrubPosition: Updated position to " + std::to_string(timestamp) + " (keeping existing video_player)"); // Commented out to reduce debug spam
    }
    
    // NOTE: Individual memory limits removed - eviction handled globally by VideoCache
}

void FrameCache::BackgroundCacheWorker() {
    Debug::Log("FrameCache: Background thread started (EXR pattern - permanent until shutdown)");

    // EXR PATTERN: Loop only checks shutdown_requested, thread runs permanently
    while (!shutdown_requested) {
        // Check if caching is disabled
        if (!caching_enabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // EXR PATTERN: Wait if no video loaded (like EXR cache line 649)
        if (cached_video_player == nullptr || current_video_path.empty() ||
            !background_extractor || !background_extractor->IsInitialized()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Much more aggressive throttling during playback to prevent stutter
        bool is_playing = main_player_is_playing.load();

        if (is_playing) {
            // During playback: pause caching completely to avoid interference
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Only cache when not playing - reasonable batch size to allow memory checks
        int max_extractions = 100; // Reasonable batch size - memory checks will control total
        
        double current_pos = current_scrub_position.load();
        double fps = cached_video_player->GetFrameRate();
        
        if (fps <= 0) {
            // CacheDebugLog("Invalid FPS: " + std::to_string(fps) + ", skipping cache cycle");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Faster response
            continue;
        }
        
        // CacheDebugLog("Background cache cycle - video_path: '" + current_video_path +
        //              "', extractor_initialized: " + (background_extractor->IsInitialized() ? "YES" : "NO")); // Commented out to reduce debug spam
        
        // No rate limiting - only RAM limit should constrain caching
        
        // Cache based on strategy: centered around seekbar or full video sequential
        int center_frame = TimestampToFrameNumber(current_pos, fps);
        int window_frames;
        int frame_step = 1; // Cache every frame for maximum smoothness

        bool use_centered_caching = config.use_centered_caching;
        bool use_full_video_sequential = !config.use_centered_caching;

        // Debug: Log which caching mode we're using
        static int debug_counter = 0;
        if (debug_counter++ % 100 == 0) { // Log every 100 cycles to avoid spam
            // Debug removed - caching mode logging
        }
        
        // Background extractor is initialized separately - just check if ready
        bool extractor_ready = false;
        if (background_extractor) {
            extractor_ready = background_extractor->IsInitialized();
        }

        if (!extractor_ready && !current_video_path.empty()) {
            // Background extractor initialization happens in SetVideoFile
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        if (use_full_video_sequential) {
            // Cache entire video duration sequentially (videos only)
            // For videos, use duration-based calculation
            double duration = background_extractor ? background_extractor->GetDuration() : 0.0;
            if (duration <= 0.0) {
                //("Invalid video duration: " + std::to_string(duration) + ", using fallback window");
                window_frames = static_cast<int>(30.0f * fps); // Fallback to 30 second window
            } else {
                window_frames = static_cast<int>(std::round(duration * fps)); // Round instead of truncate
            }
        } else {
            // Centered caching: use seconds-based limit instead of infinite window
            window_frames = static_cast<int>(config.max_cache_seconds * fps);
         /*   Debug::Log("FrameCache: Using seconds-based caching window: " +
                      std::to_string(config.max_cache_seconds) + "s = " +
                      std::to_string(window_frames) + " frames");*/
        }
        
        bool extracted_any = false;
        int extractions_this_cycle = 0;
        
        // Build frame list based on caching strategy
        std::vector<int> frame_numbers;

        if (use_full_video_sequential) {
            // Check if sequential caching is already complete
            if (sequential_cache_complete.load()) {
                // CacheDebugLog("Sequential caching already complete - background thread idling"); // Commented out to reduce debug spam
                // Sleep longer when caching is complete to reduce CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(2000)); // 2 second sleep
                continue;
            } else {
                // Full video caching: simple sequential approach
                int start_frame = sequential_cache_position.load();
                int frames_added = 0;
                
                // CacheDebugLog("Sequential caching starting at frame " + std::to_string(start_frame) + 
                //              " of " + std::to_string(window_frames) + " total frames");
                
                // Cache frames starting from current position
                {
                    std::lock_guard<std::mutex> lock(cache_mutex);
                    // CacheDebugLog("Loop: start_frame=" + std::to_string(start_frame) + 
                    //              ", window_frames=" + std::to_string(window_frames) + 
                    //              ", max_extractions=" + std::to_string(max_extractions));
                    
                    for (int frame = start_frame; frame < window_frames && frame_numbers.size() < max_extractions; frame++) {
                        bool frame_exists = (scrub_cache.find(frame) != scrub_cache.end());
                        if (!frame_exists) {
                            frame_numbers.push_back(frame);
                            frames_added++;
                            // CacheDebugLog("  Adding frame " + std::to_string(frame) + " to extraction queue"); // Commented out to reduce debug spam
                        } else {
                            // CacheDebugLog("  Frame " + std::to_string(frame) + " already cached, skipping"); // Commented out to reduce debug spam
                        }
                    }
                    
                    // EXPLICIT CHECK: Ensure the very last frame gets queued if missing
                    int final_frame = window_frames - 1;
                    if (start_frame <= final_frame && frame_numbers.size() < max_extractions) {
                        bool final_frame_exists = (scrub_cache.find(final_frame) != scrub_cache.end());
                        if (!final_frame_exists) {
                            // Check if final frame was already added in the loop above
                            bool already_queued = std::find(frame_numbers.begin(), frame_numbers.end(), final_frame) != frame_numbers.end();
                            if (!already_queued) {
                                frame_numbers.push_back(final_frame);
                                frames_added++;
                                // CacheDebugLog("  *** EXPLICIT FINAL FRAME *** Adding frame " + std::to_string(final_frame) + " to extraction queue");
                            }
                        }
                    }
                    
                    // Check specifically for frame 178 if we're near the end
                    if (start_frame >= 150) {
                        bool frame_178_exists = (scrub_cache.find(178) != scrub_cache.end());
                        double frame_178_timestamp = FrameNumberToTimestamp(178, fps);
                        double video_duration = background_extractor->GetDuration();
                        // CacheDebugLog("Frame 178 status: " + std::string(frame_178_exists ? "EXISTS" : "MISSING") +
                        //              ", timestamp: " + std::to_string(frame_178_timestamp) +
                        //              "s, video duration: " + std::to_string(video_duration) + "s");
                    }
                }
                
                // Update position for next cycle (don't check completion yet)
                int next_position = start_frame + max_extractions;
                if (next_position >= window_frames) {
                    sequential_cache_position.store(0); // Wrap around for another pass
                    // CacheDebugLog("Sequential cache reached end, wrapping to beginning");
                } else {
                    sequential_cache_position.store(next_position);
                    // CacheDebugLog("Sequential cache continuing from frame " + std::to_string(next_position));
                }
                
                // CacheDebugLog("Sequential caching: added " + std::to_string(frames_added) + 
                //              " frames, next position: " + std::to_string(sequential_cache_position.load()));
            }
        } else {
            // Centered caching: gap-aware spiral pattern that continues expanding outward
            // until RAM limit is reached, not restarting from center each cycle

            // Get video bounds
            int max_frame;
            // For videos, use duration-based calculation
            double duration = background_extractor->GetDuration();
            max_frame = (duration > 0) ? static_cast<int>(duration * fps) - 1 : 0;

            // Calculate sliding window bounds to always cache 20 seconds (window_frames total)
            int window_start, window_end;

            // Ideally center the window, but slide it to stay within video bounds
            int ideal_start = center_frame - (window_frames / 2);
            int ideal_end = center_frame + (window_frames / 2);

            if (ideal_start < 0) {
                // Near beginning: slide window right to fit 20 seconds from start
                window_start = 0;
                window_end = std::min(window_frames - 1, max_frame);
            } else if (ideal_end > max_frame) {
                // Near end: slide window left to fit 20 seconds before end
                window_end = max_frame;
                window_start = std::max(0, max_frame - window_frames + 1);
            } else {
                // Middle: use ideal centered window
                window_start = ideal_start;
                window_end = ideal_end;
            }

            // Find all missing frames within the sliding window and prioritize by distance from center
            std::vector<std::pair<int, int>> missing_frames; // (frame_number, distance_from_center)

            {
                std::lock_guard<std::mutex> lock(cache_mutex);

                // Scan the sliding window for missing frames
                for (int frame = window_start; frame <= window_end; frame++) {
                    if (scrub_cache.find(frame) == scrub_cache.end()) {
                        int distance_from_center = std::abs(frame - center_frame);
                        missing_frames.emplace_back(frame, distance_from_center);
                    }
                }
            }

            // Sort by distance from center (closest first) to maintain spiral priority
            std::sort(missing_frames.begin(), missing_frames.end(),
                [](const auto& a, const auto& b) {
                    return a.second < b.second;
                });

            // Take up to max_extractions frames for this cycle
            for (size_t i = 0; i < missing_frames.size() && frame_numbers.size() < max_extractions; ++i) {
                frame_numbers.push_back(missing_frames[i].first);
            }
        }
        
        for (int frame_number : frame_numbers) {
            if (extractions_this_cycle >= max_extractions) break;
            if (shutdown_requested) break;
            
            // Validate frame bounds
            if (frame_number < 0) continue;

            // Also check upper bound to prevent invalid frame extraction
            int max_frame;
            // For videos, use duration-based calculation
            double duration = background_extractor ? background_extractor->GetDuration() : 0.0;
            max_frame = (duration > 0) ? static_cast<int>(duration * fps) - 1 : 0;

            // Additional validation using video player's duration if available
            if (cached_video_player) {
                double player_duration = cached_video_player->GetDuration();
                if (player_duration > 0 && player_duration < duration) {
                    // Use the more conservative (shorter) duration
                    duration = player_duration;
                    max_frame = static_cast<int>(duration * fps) - 1;

                    // Log duration mismatch only once per video
                    static std::string last_logged_video_path;
                    if (last_logged_video_path != current_video_path) {
                        Debug::Log("FrameCache: Using video player duration " + std::to_string(duration) +
                                  "s instead of FFmpeg duration for frame bounds");
                        last_logged_video_path = current_video_path;
                    }
                }
            }

            if (frame_number > max_frame) {
                double duration_final = background_extractor ? background_extractor->GetDuration() : 0.0;
                Debug::Log("Skipping frame " + std::to_string(frame_number) + " beyond video end (" +
                          std::to_string(max_frame) + ", duration: " + std::to_string(duration_final) + "s)");
                continue;
            }
            
            // Check if frame is already cached
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                if (scrub_cache.find(frame_number) != scrub_cache.end()) {
                    continue; // Already cached
                }
                
                // NOTE: Individual memory limit checks removed - global limit enforced by VideoCache
            }
            
            // Double-check we're not playing before extracting
            if (main_player_is_playing.load()) {
                // CacheDebugLog("Playback started, aborting extraction cycle");
                break;
            }
            
            // Extract frame
            double frame_timestamp = FrameNumberToTimestamp(frame_number, fps);
            ExtractFrameAtPosition(frame_timestamp, cached_video_player);
            extractions_this_cycle++;
            extracted_any = true;
            
            // No artificial pauses - let FFmpeg extract at maximum speed
        }
        
        if (extracted_any) {
            // CacheDebugLog("Completed caching cycle, extracted " + std::to_string(extractions_this_cycle) + 
            //              " frames. Total cached: " + std::to_string(scrub_cache.size()));
        } else {
            // CacheDebugLog("Caching cycle completed with no extractions (all frames already cached or failed)");
        }
        
        // Check for completion AFTER extraction cycle completes (only for full video sequential mode)
        if (use_full_video_sequential && !sequential_cache_complete.load()) {
            std::lock_guard<std::mutex> lock(cache_mutex);
            bool all_cached = true;
            int total_frames;

            // For videos, use duration-based calculation
            double fps = cached_video_player->GetFrameRate();
            double duration = background_extractor->GetDuration();
            total_frames = static_cast<int>(duration * fps);

            for (int frame = 0; frame < total_frames && all_cached; frame++) {
                if (scrub_cache.find(frame) == scrub_cache.end()) {
                    all_cached = false;
                }
            }

            if (all_cached) {
                sequential_cache_complete.store(true);
                // CacheDebugLog("*** COMPLETION CHECK AFTER EXTRACTION *** All " + std::to_string(total_frames) +
                //              " frames cached! Sequential caching complete - background thread will idle");
            }
        }

        // NEW: Periodic seconds-based eviction for centered caching
        if (use_centered_caching && !scrub_cache.empty()) {
            static int eviction_counter = 0;
            if (++eviction_counter % 10 == 0) { // Every 10 cycles
                double current_pos = current_scrub_position.load();
                EvictFramesBeyondSeconds(current_pos, config.max_cache_seconds);
            }
        }

        // Very short rest between cycles for maximum cache speed
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    //("Background caching thread stopped");
}

void FrameCache::ExtractFrameAtPosition(double timestamp, VideoPlayer* video_player) {
    if (!video_player || current_video_path.empty()) {
        return;
    }

    if (!background_extractor) {
        return;
    }

    // With background extractor, we just request the frame
    // The extractor handles all the complex extraction in background threads
    if (!background_extractor->IsInitialized()) {
        return; // Background extractor initialization happens in SetVideoFile
    }

    double fps = background_extractor->GetFrameRate();
    int frame_number = TimestampToFrameNumber(timestamp, fps);

    // Request frame from background extractor with high priority (immediate need)
    background_extractor->RequestFrame(frame_number, timestamp, 1000);
}

// DISABLED: Opportunistic caching - using only spiral background caching
bool FrameCache::TryCacheCurrentFrame(VideoPlayer* video_player) {
    if (!video_player) {
        return false;
    }

    if (background_extractor && background_extractor->IsInitialized()) {
        // Only process completed frames from background extraction
        // Do NOT request new individual frames - let spiral caching handle everything
        background_extractor->ProcessCompletedFrames();
        return false;
    }

    return false; // No opportunistic requests made
}

// Removed: RequestFrameCaching() method (opportunistic caching no longer used)

void FrameCache::AddExtractedFrame(int frame_number, double timestamp, GLuint texture_id, int width, int height) {
    std::lock_guard<std::mutex> lock(cache_mutex);

    // Check if frame already exists
    if (scrub_cache.find(frame_number) != scrub_cache.end()) {
        // Frame already cached, release the provided texture
        if (texture_id != 0) {
            glDeleteTextures(1, &texture_id);
        }
        return;
    }

    // Create cached frame entry
    auto cached_frame = std::make_unique<CachedFrame>();
    cached_frame->timestamp = timestamp;
    cached_frame->width = width;
    cached_frame->height = height;
    // Removed: memory_size tracking (memory-based eviction removed)
    cached_frame->is_valid = true;
    cached_frame->last_accessed = std::chrono::steady_clock::now();
    cached_frame->texture_id = texture_id;
    cached_frame->texture_created = true;

    // Removed: memory usage tracking (memory-based eviction removed)

    // Add to cache
    scrub_cache[frame_number] = std::move(cached_frame);

    //Debug::Log("FrameCache: Added extracted frame " + std::to_string(frame_number) +
    //           " (" + std::to_string(timestamp) + "s) to cache");
}

void FrameCache::AddExtractedFrame(int frame_number, double timestamp, const std::vector<uint8_t>& pixel_data, int width, int height, bool from_native_image) {
    std::lock_guard<std::mutex> lock(cache_mutex);

    // Check if frame already exists
    if (scrub_cache.find(frame_number) != scrub_cache.end()) {
        return;
    }

    // Create texture from pixel data on main thread (correct OpenGL context)
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);

    if (texture_id == 0) {
        Debug::Log("FrameCache: Failed to create texture for extracted frame " + std::to_string(frame_number));
        return;
    }

    // Get pipeline-specific format configuration
    auto it = PIPELINE_CONFIGS.find(config.pipeline_mode);
    if (it == PIPELINE_CONFIGS.end()) {
        it = PIPELINE_CONFIGS.find(PipelineMode::NORMAL);  // Fallback
    }
    const PipelineConfig& pipeline_config = it->second;

    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Ensure proper alignment for texture upload
    // RGBA data (4 channels) is naturally 4-byte aligned, so use default alignment
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // DIAGNOSTIC: Log first non-zero pixel for texture upload verification
    if (config.pipeline_mode == PipelineMode::HIGH_RES && pixel_data.size() >= 16) {
        const uint16_t* pixels16 = reinterpret_cast<const uint16_t*>(pixel_data.data());
        // Find first non-zero pixel
        bool found_nonzero = false;
        for (size_t i = 0; i < std::min(pixel_data.size() / 8, size_t(100)); i++) {
            if (pixels16[i*4] != 0 || pixels16[i*4+1] != 0 || pixels16[i*4+2] != 0) {
                Debug::Log("FrameCache: First non-zero pixel at offset " + std::to_string(i) +
                           ": R=" + std::to_string(pixels16[i*4]) +
                           " G=" + std::to_string(pixels16[i*4+1]) +
                           " B=" + std::to_string(pixels16[i*4+2]) +
                           " A=" + std::to_string(pixels16[i*4+3]) +
                           " (format=GL_RGBA internal=" + std::to_string(pipeline_config.internal_format) +
                           " type=" + std::to_string(pipeline_config.data_type) + ")");
                found_nonzero = true;
                break;
            }
        }
        if (!found_nonzero) {
            Debug::Log("FrameCache: WARNING - First 100 pixels are all zeros");
        }
    }

    glTexImage2D(GL_TEXTURE_2D, 0, pipeline_config.internal_format, width, height, 0,
                 GL_RGBA, pipeline_config.data_type, pixel_data.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    // Create cached frame entry
    auto cached_frame = std::make_unique<CachedFrame>();
    cached_frame->timestamp = timestamp;
    cached_frame->width = width;
    cached_frame->height = height;
    // Removed: memory_size tracking (memory-based eviction removed)
    cached_frame->is_valid = true;
    cached_frame->last_accessed = std::chrono::steady_clock::now();
    cached_frame->texture_id = texture_id;
    cached_frame->texture_created = true;
    cached_frame->pipeline_mode = config.pipeline_mode;  // Store pipeline mode for consistency

    // Removed: memory usage tracking (memory-based eviction removed)

    // Add to cache
    scrub_cache[frame_number] = std::move(cached_frame);

    //Debug::Log("FrameCache: Added extracted frame " + std::to_string(frame_number) +
    //           " (" + std::to_string(timestamp) + "s) with texture " + std::to_string(texture_id));
}

bool FrameCache::IsFrameCached(int frame_number) const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    return scrub_cache.find(frame_number) != scrub_cache.end();
}

int FrameCache::TimestampToFrameNumber(double timestamp, double fps) const {
    // ACCURACY FIX: Use consistent frame rate source and account for start time offset
    if (!background_extractor || !background_extractor->IsInitialized()) {
        return static_cast<int>(std::round(timestamp * fps));
    }

    // For videos: Use FFmpeg's detected frame rate for consistency with frame extraction
    double effective_fps = background_extractor->GetFrameRate();
    if (effective_fps <= 0) {
        effective_fps = fps; // Fallback to provided fps
    }

    // DEBUG: Log FPS mismatch issue
    if (std::abs(effective_fps - fps) > 0.1) {
        Debug::Log("FrameCache::TimestampToFrameNumber FPS MISMATCH: provided=" + std::to_string(fps) +
                   ", extractor=" + std::to_string(effective_fps) +
                   ", timestamp=" + std::to_string(timestamp));
    }

    // Account for start time offset in MP4s and other containers
    double adjusted_timestamp = timestamp;
    if (background_extractor) {
        // Get start time from FFmpeg (in seconds)
        int64_t start_time_av = background_extractor->GetStartTime(); // AVStream start_time in timebase units
        if (start_time_av > 0) {
            // Convert from AV_TIME_BASE units to seconds
            double start_time_seconds = static_cast<double>(start_time_av) / AV_TIME_BASE;
            adjusted_timestamp = timestamp - start_time_seconds;
            if (adjusted_timestamp < 0) adjusted_timestamp = 0;
        }
    }

    // Use more precise calculation to avoid cumulative rounding errors
    double precise_frame = adjusted_timestamp * effective_fps;
    int result_frame = static_cast<int>(std::round(precise_frame));

    // Timestamp to frame calculation debugging removed

    return result_frame;
}

double FrameCache::FrameNumberToTimestamp(int frame_number, double fps) const {
    // ACCURACY FIX: Use consistent frame rate source and account for start time offset
    if (!background_extractor || !background_extractor->IsInitialized()) {
        return frame_number / fps;
    }

    // For videos: Use FFmpeg's detected frame rate for consistency
    double effective_fps = background_extractor->GetFrameRate();
    if (effective_fps <= 0) {
        effective_fps = fps; // Fallback to provided fps
    }

    // DEBUG: Log FPS mismatch issue
    if (std::abs(effective_fps - fps) > 0.1) {
        Debug::Log("FrameCache::FrameNumberToTimestamp FPS MISMATCH: provided=" + std::to_string(fps) +
                   ", extractor=" + std::to_string(effective_fps) +
                   ", frame=" + std::to_string(frame_number));
    }

    // Calculate base timestamp
    double timestamp = frame_number / effective_fps;

    // Account for start time offset in MP4s and other containers
    if (background_extractor) {
        int64_t start_time_av = background_extractor->GetStartTime();
        if (start_time_av > 0) {
            double start_time_seconds = static_cast<double>(start_time_av) / AV_TIME_BASE;
            timestamp += start_time_seconds;
        }
    }

    return timestamp;
}

void FrameCache::EvictOldFrames() {
    auto now = std::chrono::steady_clock::now();
    auto evict_threshold = now - std::chrono::seconds(10); // Evict frames not accessed in 10 seconds (seek cache behavior)
    
    auto evict_from_cache = [&](auto& cache) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (it->second->last_accessed < evict_threshold) {
                int frame_number = it->first;
                // Removed: memory usage tracking (memory-based eviction removed)
                it = cache.erase(it);

                // Notify background extractor to remove from tracking
                if (background_extractor) {
                    background_extractor->RemoveFrameFromTracking(frame_number);
                }
            } else {
                ++it;
            }
        }
    };
    
    evict_from_cache(scrub_cache);
    evict_from_cache(keyframe_cache);
}

void FrameCache::EvictFramesBeyondWindow(double center_timestamp, double window_seconds) {
    double fps = cached_video_player ? cached_video_player->GetFrameRate() : 30.0;
    int center_frame = TimestampToFrameNumber(center_timestamp, fps);
    int window_frames = static_cast<int>(window_seconds * fps);

    for (auto it = scrub_cache.begin(); it != scrub_cache.end();) {
        int frame_distance = std::abs(it->first - center_frame);
        if (frame_distance > window_frames) {
            int frame_number = it->first;
            // Removed: memory usage tracking (memory-based eviction removed)
            it = scrub_cache.erase(it);

            // Notify background extractor to remove from tracking
            if (background_extractor) {
                background_extractor->RemoveFrameFromTracking(frame_number);
            }
        } else {
            ++it;
        }
    }
}

// Removed: EvictFramesFarthestFromSeekbar() method (memory-based eviction removed)

// Removed: EvictOldestFrames() method (memory-based eviction removed)

void FrameCache::EvictFramesBeyondSeconds(double center_timestamp, int max_seconds) {
    if (scrub_cache.empty() || !cached_video_player) return;

    double fps = cached_video_player->GetFrameRate();
    if (fps <= 0) return;

    int center_frame = TimestampToFrameNumber(center_timestamp, fps);
    int window_frames = static_cast<int>(max_seconds * fps);

    // Calculate sliding window bounds (same logic as caching)
    int max_frame;
    // For videos, use duration-based calculation
    double duration = background_extractor ? background_extractor->GetDuration() : 0;
    max_frame = (duration > 0) ? static_cast<int>(duration * fps) - 1 : 0;

    int window_start, window_end;
    int ideal_start = center_frame - (window_frames / 2);
    int ideal_end = center_frame + (window_frames / 2);

    if (ideal_start < 0) {
        // Near beginning: slide window right
        window_start = 0;
        window_end = std::min(window_frames - 1, max_frame);
    } else if (ideal_end > max_frame) {
        // Near end: slide window left
        window_end = max_frame;
        window_start = std::max(0, max_frame - window_frames + 1);
    } else {
        // Middle: use ideal centered window
        window_start = ideal_start;
        window_end = ideal_end;
    }

    // Evict frames outside the sliding window
    for (auto it = scrub_cache.begin(); it != scrub_cache.end();) {
        int frame_number = it->first;
        if (frame_number < window_start || frame_number > window_end) {
            // Removed: memory usage tracking (memory-based eviction removed)
            it = scrub_cache.erase(it);

            // Notify background extractor to remove from tracking
            if (background_extractor) {
                background_extractor->RemoveFrameFromTracking(frame_number);
            }
        } else {
            ++it;
        }
    }

    //Debug::Log("FrameCache: Evicted frames outside sliding window [" +
    //           std::to_string(window_start) + "-" + std::to_string(window_end) + "], " +
    //           std::to_string(scrub_cache.size()) + " frames remaining");
}


FrameCache::CacheStats FrameCache::GetStats() const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    CacheStats stats;
    stats.total_frames_cached = scrub_cache.size() + keyframe_cache.size();
    // Removed: memory_used_mb (memory-based eviction removed)
    stats.cache_hits = cache_hits.load();
    stats.cache_misses = cache_misses.load();
    
    size_t total_requests = stats.cache_hits + stats.cache_misses;
    stats.hit_ratio = total_requests > 0 ? (float)stats.cache_hits / total_requests : 0.0f;
    
    // Calculate coverage range
    if (!scrub_cache.empty() && cached_video_player) {
        double fps = cached_video_player->GetFrameRate();
        auto min_frame = std::min_element(scrub_cache.begin(), scrub_cache.end());
        auto max_frame = std::max_element(scrub_cache.begin(), scrub_cache.end());
        stats.coverage_start = FrameNumberToTimestamp(min_frame->first, fps);
        stats.coverage_end = FrameNumberToTimestamp(max_frame->first, fps);
    }
    
    return stats;
}

void FrameCache::NotifyPlaybackState(bool is_playing) {
    main_player_is_playing = is_playing;

    // Notify background extractor about playback state
    if (background_extractor) {
        background_extractor->NotifyPlaybackState(is_playing);
    }

    if (is_playing) {
        // Debug::Log("FrameCache: Playback started - reducing cache activity");
    } else {
        // Debug::Log("FrameCache: Playback stopped - resuming full cache activity");
    }
}

std::vector<FrameCache::CacheSegment> FrameCache::GetCacheSegments() const {
    // Get cache segments directly from background extractor
    if (background_extractor) {
        auto extractor_segments = background_extractor->GetCacheSegments();
        std::vector<CacheSegment> segments;

        // Convert from extractor format to FrameCache format
        for (const auto& extractor_seg : extractor_segments) {
            CacheSegment seg;
            seg.start_time = extractor_seg.start_time;
            seg.end_time = extractor_seg.end_time;
            seg.type = CacheSegment::SCRUB_CACHE; // Background cache shows as green "scrub cache"
            seg.density = 1.0f; // Full density for extracted frames
            segments.push_back(seg);
        }

        return segments;
    }

    // No background extractor available
    return std::vector<CacheSegment>();
}

void FrameCache::SetVideoFile(const std::string& video_path, const VideoMetadata* metadata) {
    if (current_video_path == video_path) return; // No change

    Debug::Log("FrameCache: SetVideoFile called for " + video_path);

    // EXR PATTERN: Clear old cache, thread stays alive
    // This is fast because it's just marking entries invalid, no thread stop/start

    // Clean up previous video extractor
    if (background_extractor) {
        background_extractor->Shutdown();
    }

    // Clear cache for new video (instant - just invalidates entries)
    InvalidateCache();

    // Reset sequential caching position for new video
    sequential_cache_position.store(0);
    sequential_cache_complete.store(false);

    // Update video path
    current_video_path = video_path;

    // Initialize background extractor with new video
    // metadata can be nullptr - start immediately!
    if (background_extractor && !video_path.empty()) {
        if (!background_extractor->Initialize(video_path, metadata)) {
            Debug::Log("FrameCache: Failed to initialize background extractor for " + video_path);
        } else {
            // Start extraction immediately (metadata is optional)
            background_extractor->StartBackgroundExtraction();
            Debug::Log("FrameCache: Video swapped, cache cleared, extractor started (EXR pattern - thread still running)");
        }
    }

    // Background thread continues running, starts caching new video
}

void FrameCache::UpdateVideoMetadata(const std::string& video_path, const VideoMetadata& metadata) {
    if (current_video_path != video_path) {
        return; // Path mismatch, ignore
    }

    // EXR PATTERN: Just update metadata, no thread management needed
    // Background extractor updates color matrix for future frames
    if (background_extractor && background_extractor->IsInitialized()) {
        background_extractor->UpdateVideoMetadata(metadata);
        Debug::Log("FrameCache: Updated background extractor with video metadata (color matrix will apply to new frames)");
    } else {
        Debug::Log("FrameCache: Background extractor not ready for metadata update");
    }

    // EXR PATTERN: Thread is already running, no need to start anything
}

void FrameCache::InvalidateCache() {
    std::lock_guard<std::mutex> lock(cache_mutex);

    // Notify background extractor to clear tracking for all frames being removed
    if (background_extractor) {
        for (const auto& pair : scrub_cache) {
            background_extractor->RemoveFrameFromTracking(pair.first);
        }
        for (const auto& pair : keyframe_cache) {
            background_extractor->RemoveFrameFromTracking(pair.first);
        }
    }

    scrub_cache.clear();
    keyframe_cache.clear();
    // Removed: current_cache_size reset (memory-based eviction removed)
    // Debug::Log("FrameCache: Cache invalidated");
}

void FrameCache::SetCacheConfig(const CacheConfig& new_config) {
    // Check if caching mode changed - if so, invalidate cache for clean start
    bool mode_changed = (config.use_centered_caching != new_config.use_centered_caching);

    if (mode_changed) {
        std::string old_mode = config.use_centered_caching ? "CENTERED" : "SEQUENTIAL";
        std::string new_mode = new_config.use_centered_caching ? "CENTERED" : "SEQUENTIAL";
        //("Cache mode changed from " + old_mode + " to " + new_mode + " - invalidating cache");
    }

    config = new_config;

    // If mode changed, clear existing cache and reset state
    if (mode_changed) {
        InvalidateCache();
        sequential_cache_position.store(0);
        sequential_cache_complete.store(false);
    }

    // Debug removed - configuration logging
}

// EXR PATTERN: Removed PauseBackgroundCaching, ResumeBackgroundCaching, RestartBackgroundThread
// These are no longer needed - thread runs permanently

// EXR PATTERN: Removed StartBackgroundCaching and StopBackgroundCaching
// Thread is created once in constructor, destroyed only in destructor

void FrameCache::SetCachingEnabled(bool enabled) {
    // EXR PATTERN: Just set the flag, thread checks it in the loop
    caching_enabled = enabled;
    Debug::Log(enabled ? "FrameCache: Caching enabled" : "FrameCache: Caching disabled");
    // Thread continues running, just skips work when disabled
}

bool FrameCache::IsInitialized() const {
    return background_extractor && background_extractor->IsInitialized();
}

void FrameCache::ClearCachedFrames() {
    std::lock_guard<std::mutex> lock(cache_mutex);

    // Clear all cached frames but keep the cache structure
    scrub_cache.clear();     // Main RAM cache
    keyframe_cache.clear();  // Keyframe cache

    Debug::Log("FrameCache: Cleared all cached frames (kept cache structure)");

    // Restart background extraction to repopulate the cache
    if (background_extractor) {
        // Check if extractor is initialized (has video loaded)
        if (!background_extractor->IsInitialized()) {
            Debug::Log("FrameCache: Background extractor not initialized yet, skipping restart");
            return;
        }

        // Clear old pending requests first
        background_extractor->ClearPendingRequests();

        // Try to resume if paused (handles PAUSED_MANUAL state)
        background_extractor->ResumeExtraction();

        // Try to start if stopped (handles STOPPED state)
        background_extractor->StartBackgroundExtraction();

        // Force window refresh - this works in any state including EXTRACTING
        // This is the key: even if already extracting, we need new requests
        background_extractor->ForceWindowRefresh();

        Debug::Log("FrameCache: Triggered background extraction restart (cleared requests + forced window refresh)");
    }
}

// ============================================================================
// 3-Tier Cache Implementation (RAM → Disk → Direct EXR)
// ============================================================================

// Removed: InitializeDiskCache method (simplified to RAM-only cache)

bool FrameCache::GetFrameFromRAM(int frame_number, GLuint& texture_id, int& width, int& height) {
    auto it = scrub_cache.find(frame_number);
    if (it != scrub_cache.end() && it->second->is_valid && it->second->texture_id != 0) {
        texture_id = it->second->texture_id;
        width = it->second->width;
        height = it->second->height;
        it->second->last_accessed = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

// Removed: GetFrameFromDisk (disk cache removed)

// Removed: CacheFrameToDisk (disk cache removed)

// Removed: ReadTexturePixels (disk cache removed)

// Removed: SyncRAMToDisk (disk cache removed)

// Removed: GetFrameLocation (disk cache removed)

// Removed: PromoteFrameFromDisk (disk cache removed)

// Removed: InvalidateDiskCache (disk cache removed)

// Removed: Immediate disk caching methods (disk cache removed)

// Removed: ExtractAndCacheFrameFromVideo (disk cache removed)

// Removed: ExtractAndCacheEXRFrame (disk cache removed)

// Removed: ExtractAndCacheVideoFrame (disk cache removed)