#include "managed_video_decoder.h"
#include "decoder_cleanup_queue.h"
#include "../utils/debug_utils.h"

#include <cmath>
#include <algorithm>

namespace ump {

//=============================================================================
// Constructor / Destructor
//=============================================================================

ManagedVideoDecoder::ManagedVideoDecoder(const std::string& video_path)
    : video_path_(video_path) {
}

ManagedVideoDecoder::~ManagedVideoDecoder() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool ManagedVideoDecoder::Initialize() {
    if (initialized_.load()) return true;

    // Create and initialize the first decoder
    auto decoder = std::make_shared<StreamingVideoDecoder>(video_path_);
    decoder->SetConfig(config_);

    if (!decoder->Initialize()) {
        Debug::Log("ManagedVideoDecoder: Failed to initialize decoder for " + video_path_);
        return false;
    }

    // Cache metadata
    width_ = decoder->GetWidth();
    height_ = decoder->GetHeight();
    fps_ = decoder->GetFPS();
    duration_ = decoder->GetDuration();
    frame_count_ = decoder->GetFrameCount();
    metadata_cached_ = true;

    // Install as active
    {
        std::lock_guard<std::mutex> lock(active_mutex_);
        active_ = decoder;
    }

    initialized_ = true;
    spawn_count_++;

    Debug::Log("ManagedVideoDecoder: Initialized for " + video_path_ +
               " (" + std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps)");

    return true;
}

void ManagedVideoDecoder::Shutdown() {
    if (!initialized_.load()) return;

    Debug::Log("ManagedVideoDecoder: Shutting down (spawns: " +
               std::to_string(spawn_count_.load()) + ", abandons: " +
               std::to_string(abandon_count_.load()) + ")");

    initialized_ = false;

    // Get the active decoder
    std::shared_ptr<StreamingVideoDecoder> decoder;
    {
        std::lock_guard<std::mutex> lock(active_mutex_);
        decoder = std::move(active_);
    }

    // Queue for cleanup (or clean immediately if queue is shut down)
    if (decoder) {
        DecoderCleanupQueue::Instance().Abandon(std::move(decoder));
    }
}

void ManagedVideoDecoder::HardReset(int target_frame) {
    Debug::Log("ManagedVideoDecoder: HardReset requested to frame " +
               std::to_string(target_frame));
    SpawnFreshDecoder(target_frame);
}

//=============================================================================
// Frame Access
//=============================================================================

std::shared_ptr<PixelData> ManagedVideoDecoder::GetFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return nullptr;

    auto pixels = active_->GetFrame(frame_number);
    if (pixels) {
        // Got a frame - update watchdog timestamp
        last_frame_time_ = std::chrono::steady_clock::now();
        return pixels;
    }

    // No frame available - check watchdog if playing
    // (lock is held, so release and call watchdog outside)
    return nullptr;
}

std::shared_ptr<PixelData> ManagedVideoDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return nullptr;
    return active_->GetClosestFrame(frame_number, actual_frame);
}

bool ManagedVideoDecoder::HasFrame(int frame_number) const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return false;
    return active_->HasFrame(frame_number);
}

//=============================================================================
// Keyframe Access
//=============================================================================

std::shared_ptr<PixelData> ManagedVideoDecoder::GetKeyframe(int target_frame, int* actual_keyframe) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return nullptr;
    return active_->GetKeyframe(target_frame, actual_keyframe);
}

int ManagedVideoDecoder::GetNearestKeyframePosition(int target_frame) const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return target_frame;
    return active_->GetNearestKeyframePosition(target_frame);
}

bool ManagedVideoDecoder::IsIntraFrameCodec() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return false;
    return active_->IsIntraFrameCodec();
}

bool ManagedVideoDecoder::HasKeyframeIndex() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return false;
    return active_->HasKeyframeIndex();
}

//=============================================================================
// Playhead Management - THE KEY LOGIC
//=============================================================================

void ManagedVideoDecoder::UpdatePlayhead(int frame_number, SeekQuality quality, bool force_seek, bool is_prefetch) {
    last_playhead_ = frame_number;

    // Watchdog: check if decoder is stuck and needs recovery
    // This must happen before normal spawn logic to catch jammed decoders
    if (CheckWatchdogAndRecover()) {
        return;  // Recovery triggered, fresh decoder spawned
    }

    // Decision: spawn new decoder or use existing?
    // For prefetch (upcoming clips), use playback-like spawning (less aggressive)
    if (force_seek || ShouldSpawnNewDecoder(frame_number, is_prefetch)) {
        SpawnFreshDecoder(frame_number);
        return;
    }

    // Forward to existing decoder
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_) {
        active_->UpdatePlayhead(frame_number, quality, false);
    }
}

bool ManagedVideoDecoder::ShouldSpawnNewDecoder(int target_frame, bool is_prefetch) const {
    std::lock_guard<std::mutex> lock(active_mutex_);

    // No active decoder - need to spawn
    if (!active_) {
        return true;
    }

    // Get buffer state first - needed for multiple checks
    int buf_start = -1, buf_end = -1;
    active_->GetBufferedRange(buf_start, buf_end);

    // CRITICAL: Don't re-spawn for the same target frame!
    // This prevents infinite spawn loops for inter-frame codecs (H.264/H.265)
    // where seeking to frame N lands on keyframe K (K < N), and the buffer
    // [K, K+x] never contains N until decoder catches up. Without this check,
    // we'd keep spawning fresh decoders that reset to keyframe K repeatedly.
    //
    // HOWEVER: After a loop wrap, we might request the same frame (loop_start)
    // but the buffer has OLD frames from loop_end. In this case, we MUST spawn.
    // Check if buffer is actually useful for the target.
    if (last_spawn_target_.load() == target_frame) {
        // Same target - only skip if buffer is relevant
        if (buf_start >= 0 && buf_end >= 0) {
            double fps = active_->GetFPS();
            int tolerance = static_cast<int>(fps * 2);  // ~2 seconds

            // Buffer is useful if target is within tolerance of buffer range
            bool buffer_useful = (target_frame >= buf_start - tolerance &&
                                  target_frame <= buf_end + tolerance);
            if (buffer_useful) {
                return false;  // Buffer can catch up to target
            }
            // Buffer is far from target (e.g., loop wrap) - need to spawn!
            Debug::Log("ManagedVideoDecoder: Buffer [" + std::to_string(buf_start) +
                       "," + std::to_string(buf_end) + "] far from target " +
                       std::to_string(target_frame) + " - spawning despite same target");
        }
    }

    // Effective "playing" state: actual playback OR prefetching upcoming clips
    // When prefetching, we want playback-like behavior (allow buffer to fill)
    bool treat_as_playing = is_playing_.load() || is_prefetch;

    // Empty buffer - spawn if not playing/prefetching
    if (buf_start < 0 || buf_end < 0) {
        if (treat_as_playing) {
            return false;
        }
        return true;
    }

    // Target is in buffer - no need to spawn
    if (target_frame >= buf_start && target_frame <= buf_end) {
        return false;
    }

    // Target is outside buffer - this is a seek

    // Backward seek - check distance before spawning
    // Small backward seeks can use internal FlushAndSeek without new decoder
    // This avoids excessive decoder spawns during light scrubbing
    if (target_frame < buf_start) {
        int backward_distance = buf_start - target_frame;
        double fps = active_->GetFPS();
        // Allow ~1 second backward without spawn (decoder will FlushAndSeek internally)
        int backward_tolerance = static_cast<int>(fps);

        if (backward_distance > backward_tolerance) {
            // Large backward jump - spawn fresh for faster response
            return true;
        }

        // Small backward step - let existing decoder handle it via internal seek
        // This is especially important during playback with jitter
        return false;
    }

    // Forward - check distance
    int distance = target_frame - buf_end;
    double fps = active_->GetFPS();
    int threshold = static_cast<int>(fps);  // ~1 second

    // Large forward jump - spawn fresh
    if (distance > threshold) {
        return true;
    }

    // Small forward jump - spawn if not playing/prefetching
    if (!treat_as_playing) {
        return true;
    }

    // Playing/prefetching with small forward gap - let decoder catch up
    return false;
}

void ManagedVideoDecoder::SpawnFreshDecoder(int target_frame) {
    // Record what we're spawning for - prevents re-spawning for same target
    last_spawn_target_ = target_frame;

    // Capture the old decoder
    std::shared_ptr<StreamingVideoDecoder> old_decoder;
    {
        std::lock_guard<std::mutex> lock(active_mutex_);
        old_decoder = std::move(active_);
    }

    // Queue old decoder for cleanup (instant, non-blocking)
    if (old_decoder) {
        DecoderCleanupQueue::Instance().Abandon(std::move(old_decoder));
        abandon_count_++;
    }

    // Create fresh decoder
    auto new_decoder = std::make_shared<StreamingVideoDecoder>(video_path_);
    new_decoder->SetConfig(config_);

    if (new_decoder->Initialize()) {
        // Propagate shuttle mode to new decoder (for unthrottled decoding)
        if (shuttle_.active) {
            new_decoder->SetShuttleMode(true);
        }

        // Seek to target frame immediately
        new_decoder->UpdatePlayhead(target_frame, SeekQuality::NORMAL, true);

        // Wait for at least one frame to be decoded before installing
        // EXCEPT during shuttle mode - we already have harvested frames to display,
        // so don't block the UI waiting for the new decoder
        if (!shuttle_.active) {
            auto start = std::chrono::steady_clock::now();
            const int kMaxWaitMs = 2000;  // 2 second max wait
            while (new_decoder->GetBufferSize() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed > kMaxWaitMs) {
                    Debug::Log("ManagedVideoDecoder: Timeout waiting for first frame");
                    break;
                }
            }
        }

        // Install as active
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_ = new_decoder;
        }

        spawn_count_++;

        // Reset watchdog AFTER decoder is installed and has frames
        // (not at start of spawn - spawn itself takes time)
        last_frame_time_ = std::chrono::steady_clock::now();

        // Commented out to reduce log spam during debugging
        //int buf_start = -1, buf_end = -1;
        //new_decoder->GetBufferedRange(buf_start, buf_end);
        //Debug::Log("ManagedVideoDecoder: Spawned decoder #" +
        //           std::to_string(spawn_count_.load()) + " at frame " +
        //           std::to_string(target_frame) + " buf=[" +
        //           std::to_string(buf_start) + "," + std::to_string(buf_end) + "]");
    } else {
        Debug::Log("ManagedVideoDecoder: FAILED to spawn fresh decoder at frame " +
                   std::to_string(target_frame));
    }
}

//=============================================================================
// Watchdog Recovery
//=============================================================================

bool ManagedVideoDecoder::CheckWatchdogAndRecover() {
    // Only check when playing - scrubbing naturally has gaps
    if (!is_playing_.load()) {
        return false;
    }

    // Don't check during shuttle mode
    if (shuttle_.active) {
        return false;
    }

    // Check how long since we got a frame
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_frame_time_).count();

    if (elapsed_ms > WATCHDOG_THRESHOLD_MS) {
        // Before triggering, check if decoder actually has frames
        // If buffer has frames, decoder is working - just not at playhead yet
        int buf_size = 0;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (active_) {
                buf_size = active_->GetBufferSize();
            }
        }

        if (buf_size > 0) {
            // Decoder has frames, just not the one we need
            // This is normal during catch-up - don't trigger watchdog
            // But do update the timestamp to prevent immediate re-check
            last_frame_time_ = now;
            return false;
        }

        // Buffer is truly empty - decoder is stuck
        int target = last_playhead_.load();
        Debug::Log("ManagedVideoDecoder: WATCHDOG triggered! No frame for " +
                   std::to_string(elapsed_ms) + "ms, buffer empty, forcing reset to frame " +
                   std::to_string(target));

        // Force spawn fresh decoder at current playhead
        SpawnFreshDecoder(target);
        return true;
    }

    return false;
}

//=============================================================================
// Demand-Driven Decode API
//=============================================================================

void ManagedVideoDecoder::SetNeededFrames(const std::vector<int>& frames_by_priority) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_) {
        active_->SetNeededFrames(frames_by_priority);
    }
}

StreamingVideoDecoder::DecodeStatus ManagedVideoDecoder::GetDecodeStatus() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_) {
        return active_->GetDecodeStatus();
    }
    return StreamingVideoDecoder::DecodeStatus{};
}

void ManagedVideoDecoder::EvictOutsideWindow(const std::set<int>& keep_frames) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_) {
        active_->EvictOutsideWindow(keep_frames);
    }
}

std::set<int> ManagedVideoDecoder::GetBufferedFramesSet() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_) {
        return active_->GetBufferedFramesSet();
    }
    return std::set<int>{};
}

//=============================================================================
// Buffer Status
//=============================================================================

int ManagedVideoDecoder::GetBufferedAhead() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0;
    return active_->GetBufferedAhead();
}

int ManagedVideoDecoder::GetBufferedBehind() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0;
    return active_->GetBufferedBehind();
}

int ManagedVideoDecoder::GetBufferSize() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0;
    return active_->GetBufferSize();
}

void ManagedVideoDecoder::GetBufferedRange(int& start_frame, int& end_frame) const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) {
        start_frame = -1;
        end_frame = -1;
        return;
    }
    active_->GetBufferedRange(start_frame, end_frame);
}

void ManagedVideoDecoder::ClearBuffer() {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_) {
        active_->ClearBuffer();
    }
}

bool ManagedVideoDecoder::IsSeekPending() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return false;
    return active_->IsSeekPending();
}

int ManagedVideoDecoder::GetLastSeekTarget() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0;
    return active_->GetLastSeekTarget();
}

//=============================================================================
// Metadata
//=============================================================================

int ManagedVideoDecoder::GetWidth() const {
    if (metadata_cached_) return width_;
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0;
    return active_->GetWidth();
}

int ManagedVideoDecoder::GetHeight() const {
    if (metadata_cached_) return height_;
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0;
    return active_->GetHeight();
}

double ManagedVideoDecoder::GetFPS() const {
    if (metadata_cached_) return fps_;
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0.0;
    return active_->GetFPS();
}

double ManagedVideoDecoder::GetDuration() const {
    if (metadata_cached_) return duration_;
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0.0;
    return active_->GetDuration();
}

int ManagedVideoDecoder::GetFrameCount() const {
    if (metadata_cached_) return frame_count_;
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return 0;
    return active_->GetFrameCount();
}

HWAccelType ManagedVideoDecoder::GetHWAccelType() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return HWAccelType::NONE;
    return active_->GetHWAccelType();
}

bool ManagedVideoDecoder::IsHardwareAccelerated() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (!active_) return false;
    return active_->IsHardwareAccelerated();
}

//=============================================================================
// Configuration
//=============================================================================

void ManagedVideoDecoder::SetConfig(const StreamingDecoderConfig& config) {
    config_ = config;
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_) {
        active_->SetConfig(config);
    }
}

void ManagedVideoDecoder::SetPlaybackMode(bool playing) {
    bool was_playing = is_playing_.exchange(playing);
    if (playing && !was_playing) {
        // Starting playback - reset spawn target so first UpdatePlayhead can spawn
        last_spawn_target_ = -1;
        Debug::Log("ManagedVideoDecoder: Playback started");
    } else if (!playing && was_playing) {
        Debug::Log("ManagedVideoDecoder: Playback stopped");
    }
}

//=============================================================================
// Shuttle Mode - FF/RW support
//=============================================================================

void ManagedVideoDecoder::SetShuttleMode(bool enabled, int direction) {
    if (enabled && !shuttle_.active) {
        // Starting shuttle mode
        shuttle_.active = true;
        shuttle_.direction = direction;
        shuttle_.playhead = last_playhead_.load();
        shuttle_.last_spawn_position = shuttle_.playhead;
        shuttle_.last_displayed_frame = -1;
        shuttle_.held_frame = nullptr;

        {
            std::lock_guard<std::mutex> lock(shuttle_.queue_mutex);
            shuttle_.frame_queue.clear();
        }

        Debug::Log("ManagedVideoDecoder: Shuttle mode STARTED, direction=" +
                   std::to_string(direction) + ", playhead=" + std::to_string(shuttle_.playhead));

        // Enable shuttle mode on the streaming decoder (disables window throttling)
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (active_) {
                active_->SetShuttleMode(true);
            }
        }

        // CRITICAL: Harvest frames from current decoder BEFORE any spawning
        // This gives us immediate frames to display, especially for RW
        HarvestDecoderFrames();

        int queue_size = 0;
        {
            std::lock_guard<std::mutex> lock(shuttle_.queue_mutex);
            queue_size = static_cast<int>(shuttle_.frame_queue.size());
        }
        Debug::Log("ManagedVideoDecoder: Harvested " + std::to_string(queue_size) +
                   " frames from existing decoder");

        // DON'T spawn new decoders during shuttle - it blocks the UI
        // The harvested frames provide initial display content
        // The existing decoder keeps running (unthrottled) and we harvest as it produces
        // For RW: we'll show harvested frames, then held frame as we run out
        // For FF: existing decoder is ahead, so we have frames
    }
    else if (!enabled && shuttle_.active) {
        // Exiting via SetShuttleMode(false) - use ExitShuttle() for proper cleanup
        ExitShuttle();
    }
}

std::shared_ptr<PixelData> ManagedVideoDecoder::UpdateShuttle(int playhead_frame) {
    if (!shuttle_.active) {
        return nullptr;
    }

    shuttle_.playhead = playhead_frame;

    // Harvest any new frames from decoder into queue
    HarvestDecoderFrames();

    // Purge frames that are now irrelevant
    PurgeStaleFrames();

    // Maybe spawn new decoder chunk if needed (for rewind)
    MaybeSpawnNextChunk();

    // Find best frame to display
    return GetBestShuttleFrame();
}

int ManagedVideoDecoder::ExitShuttle() {
    if (!shuttle_.active) {
        return last_playhead_.load();
    }

    shuttle_.active = false;

    // Disable shuttle mode on the streaming decoder (restore window throttling)
    {
        std::lock_guard<std::mutex> lock(active_mutex_);
        if (active_) {
            active_->SetShuttleMode(false);
        }
    }

    // Snap position = last displayed frame (what user sees)
    int snap_frame = shuttle_.last_displayed_frame;
    if (snap_frame < 0) {
        snap_frame = shuttle_.playhead;
    }

    Debug::Log("ManagedVideoDecoder: Shuttle mode EXITED, snap_frame=" + std::to_string(snap_frame));

    // Clear shuttle queue
    {
        std::lock_guard<std::mutex> lock(shuttle_.queue_mutex);
        shuttle_.frame_queue.clear();
    }

    // Spawn fresh decoder at snap position for exact frame
    // Keep held_frame until exact frame arrives (GetFrame will use it as fallback)
    SpawnFreshDecoder(snap_frame);

    // Update last_playhead so normal operations continue from snap point
    last_playhead_ = snap_frame;

    return snap_frame;
}

void ManagedVideoDecoder::HarvestDecoderFrames() {
    std::shared_ptr<StreamingVideoDecoder> decoder;
    {
        std::lock_guard<std::mutex> lock(active_mutex_);
        decoder = active_;
    }

    if (!decoder) return;

    // Get buffered range from decoder
    int buf_start = -1, buf_end = -1;
    decoder->GetBufferedRange(buf_start, buf_end);

    if (buf_start < 0 || buf_end < 0) return;

    // Pull frames from decoder buffer into shuttle queue
    for (int f = buf_start; f <= buf_end; f++) {
        // Skip if already in queue
        if (IsInShuttleQueue(f)) continue;

        auto pixels = decoder->GetFrame(f);
        if (pixels) {
            std::lock_guard<std::mutex> qlock(shuttle_.queue_mutex);
            shuttle_.frame_queue.push_back({f, pixels});
        }
    }

    // Sort queue by frame number
    SortShuttleQueue();
}

void ManagedVideoDecoder::PurgeStaleFrames() {
    std::lock_guard<std::mutex> lock(shuttle_.queue_mutex);

    // Remove frames that are too far from playhead in wrong direction
    const int MAX_BEHIND = 24;  // Keep ~1 sec behind for safety

    auto it = shuttle_.frame_queue.begin();
    while (it != shuttle_.frame_queue.end()) {
        bool should_purge = false;

        if (shuttle_.direction > 0) {  // FF
            // Purge frames way behind playhead
            if (it->frame_number < shuttle_.playhead - MAX_BEHIND)
                should_purge = true;
        } else {  // RW
            // Purge frames way ahead of playhead
            if (it->frame_number > shuttle_.playhead + MAX_BEHIND)
                should_purge = true;
        }

        if (should_purge)
            it = shuttle_.frame_queue.erase(it);
        else
            ++it;
    }
}

std::shared_ptr<PixelData> ManagedVideoDecoder::GetBestShuttleFrame() {
    std::lock_guard<std::mutex> lock(shuttle_.queue_mutex);

    std::shared_ptr<PixelData> best = nullptr;
    int best_frame = -1;

    for (const auto& sf : shuttle_.frame_queue) {
        if (shuttle_.direction > 0) {  // FF
            // Want highest frame <= playhead
            if (sf.frame_number <= shuttle_.playhead &&
                sf.frame_number > best_frame) {
                best_frame = sf.frame_number;
                best = sf.pixels;
            }
        } else {  // RW
            // Want frame closest to playhead
            // Prefer frames >= playhead (just passed), but accept < playhead too
            if (sf.frame_number >= shuttle_.playhead) {
                // Frame is at or ahead of playhead (we haven't passed it yet in RW)
                if (best_frame < 0 || sf.frame_number < best_frame) {
                    best_frame = sf.frame_number;
                    best = sf.pixels;
                }
            } else if (best_frame < 0) {
                // No frame ahead yet, take closest behind
                if (sf.frame_number > best_frame) {
                    best_frame = sf.frame_number;
                    best = sf.pixels;
                }
            }
        }
    }

    // If found better frame, update held frame
    if (best && best_frame != shuttle_.last_displayed_frame) {
        shuttle_.held_frame = best;
        shuttle_.last_displayed_frame = best_frame;
    }

    // Return held frame (may be from previous call)
    return shuttle_.held_frame;
}

bool ManagedVideoDecoder::IsInShuttleQueue(int frame_number) const {
    std::lock_guard<std::mutex> lock(shuttle_.queue_mutex);
    for (const auto& sf : shuttle_.frame_queue) {
        if (sf.frame_number == frame_number) return true;
    }
    return false;
}

void ManagedVideoDecoder::SortShuttleQueue() {
    std::lock_guard<std::mutex> lock(shuttle_.queue_mutex);
    std::sort(shuttle_.frame_queue.begin(), shuttle_.frame_queue.end(),
              [](const ShuttleState::ShuttleFrame& a, const ShuttleState::ShuttleFrame& b) {
                  return a.frame_number < b.frame_number;
              });
}

void ManagedVideoDecoder::MaybeSpawnNextChunk() {
    // DISABLED: Spawning during shuttle blocks the UI (Initialize takes 200-300ms)
    // The existing decoder keeps running and we harvest from it continuously
    // Trade-off: For RW, we may run out of frames, but UI stays smooth
    return;
}

int ManagedVideoDecoder::GetChunkSize() const {
    // ~2 seconds of frames
    double fps = fps_ > 0 ? fps_ : 24.0;
    return static_cast<int>(fps * 2.0);
}

} // namespace ump
