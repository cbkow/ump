#include "multi_cache_coordinator.h"
#include "../utils/debug_utils.h"
#include <chrono>
#include <algorithm>

namespace ump {

//=============================================================================
// Construction / Destruction
//=============================================================================

MultiCacheCoordinator::MultiCacheCoordinator() {
    Debug::Log("MultiCacheCoordinator: Created");
}

MultiCacheCoordinator::~MultiCacheCoordinator() {
    ClearAllSources();
    Debug::Log("MultiCacheCoordinator: Destroyed");
}

//=============================================================================
// Source Management
//=============================================================================

bool MultiCacheCoordinator::AddImageSequenceSource(
    const std::string& source_id,
    const std::vector<std::string>& files,
    const std::string& layer,
    double fps,
    PipelineMode pipeline_mode,
    int read_ahead_frames,
    int read_behind_frames,
    size_t thread_count,
    int start_frame)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove existing source with same ID
    auto it = sources_.find(source_id);
    if (it != sources_.end()) {
        Debug::Log("MultiCacheCoordinator: Replacing existing source: " + source_id);
        it->second->Shutdown();
        sources_.erase(it);
    }

    // Create new source
    auto source = std::make_unique<DirectEXRCacheSource>();

    FrameSourceConfig config;
    config.files = files;
    config.layer = layer;
    config.fps = fps;
    config.pipeline_mode = pipeline_mode;
    config.read_ahead_frames = read_ahead_frames;
    config.read_behind_frames = read_behind_frames;
    config.thread_count = thread_count;
    config.start_frame = start_frame;
    config.use_shared_pool = true;  // Use SharedMemoryPool for cross-cache memory management

    if (!source->Initialize(config)) {
        Debug::Log("MultiCacheCoordinator: Failed to initialize source: " + source_id);
        return false;
    }

    sources_[source_id] = std::move(source);

    // If this is the first source, make it active
    if (active_source_id_.empty()) {
        active_source_id_ = source_id;
    }

    Debug::Log("MultiCacheCoordinator: Added source '" + source_id +
               "' with " + std::to_string(files.size()) + " frames");
    return true;
}

void MultiCacheCoordinator::RemoveSource(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sources_.find(source_id);
    if (it != sources_.end()) {
        it->second->Shutdown();
        sources_.erase(it);
        Debug::Log("MultiCacheCoordinator: Removed source: " + source_id);

        // Clear active if this was the active source
        if (active_source_id_ == source_id) {
            active_source_id_.clear();
            // Pick new active if sources remain
            if (!sources_.empty()) {
                active_source_id_ = sources_.begin()->first;
            }
        }
    }
}

void MultiCacheCoordinator::ClearAllSources() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [id, source] : sources_) {
        source->Shutdown();
    }
    sources_.clear();
    active_source_id_.clear();

    Debug::Log("MultiCacheCoordinator: Cleared all sources");
}

bool MultiCacheCoordinator::HasSource(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.find(source_id) != sources_.end();
}

IFrameSource* MultiCacheCoordinator::GetSource(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(source_id);
    return (it != sources_.end()) ? it->second.get() : nullptr;
}

const IFrameSource* MultiCacheCoordinator::GetSource(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(source_id);
    return (it != sources_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> MultiCacheCoordinator::GetSourceIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sources_.size());
    for (const auto& [id, source] : sources_) {
        ids.push_back(id);
    }
    return ids;
}

size_t MultiCacheCoordinator::GetSourceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
}

//=============================================================================
// Position & Playback
//=============================================================================

void MultiCacheCoordinator::UpdatePosition(double timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
        source->UpdatePosition(timestamp);
    }
}

void MultiCacheCoordinator::SetPlaying(bool playing) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
        source->SetPlaying(playing);
    }
}

void MultiCacheCoordinator::SetLoopRange(int in_frame, int out_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
        source->SetLoopRange(in_frame, out_frame);
    }
}

void MultiCacheCoordinator::ClearLoopRange() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
        source->ClearLoopRange();
    }
}

void MultiCacheCoordinator::SetLooping(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
        source->SetLooping(enabled);
    }
}

void MultiCacheCoordinator::ResetPlaybackState() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
        source->ResetPlaybackState();
    }
    // Invalidate speed cache
    speed_cache_timestamp_.store(0);
}

//=============================================================================
// Frame Access
//=============================================================================

GLuint MultiCacheCoordinator::GetFrame(const std::string& source_id, int frame,
                                        int& width, int& height, bool& exact) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sources_.find(source_id);
    if (it == sources_.end()) {
        exact = false;
        return 0;
    }

    return it->second->GetFrame(frame, width, height, exact);
}

bool MultiCacheCoordinator::IsFrameCached(const std::string& source_id, int frame) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sources_.find(source_id);
    if (it == sources_.end()) {
        return false;
    }

    return it->second->IsFrameCached(frame);
}

void MultiCacheCoordinator::ProcessPendingOperations() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
        source->ProcessPendingOperations();
    }
}

//=============================================================================
// Unified Speed Control
//=============================================================================

double MultiCacheCoordinator::GetUnifiedPlaybackSpeed() const {
    // Check cache validity
    auto now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000;  // ms
    auto cached_time = speed_cache_timestamp_.load();

    if (now - cached_time < SPEED_CACHE_VALID_MS) {
        return cached_unified_speed_.load();
    }

    // Recalculate
    std::lock_guard<std::mutex> lock(mutex_);

    if (sources_.empty()) {
        cached_unified_speed_.store(1.0);
        speed_cache_timestamp_.store(now);
        return 1.0;
    }

    // Find minimum speed across all sources (slowest wins)
    double min_speed = 1.0;
    for (const auto& [id, source] : sources_) {
        double speed = source->GetPlaybackSpeedFactor();
        if (speed < min_speed) {
            min_speed = speed;
        }
    }

    cached_unified_speed_.store(min_speed);
    speed_cache_timestamp_.store(now);

    return min_speed;
}

bool MultiCacheCoordinator::NeedsBufferWait() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // If ANY source needs buffer wait, return true
    for (const auto& [id, source] : sources_) {
        if (source->NeedsBufferWait()) {
            return true;
        }
    }
    return false;
}

BufferHealth MultiCacheCoordinator::GetCombinedBufferHealth() const {
    std::lock_guard<std::mutex> lock(mutex_);

    BufferHealth combined;

    if (sources_.empty()) {
        combined.speed_factor = 1.0;
        return combined;
    }

    // Aggregate health from all sources
    double min_speed = 1.0;
    int min_frames_ahead = INT_MAX;
    bool any_needs_wait = false;
    int total_cached = 0;
    int total_frames = 0;

    for (const auto& [id, source] : sources_) {
        BufferHealth health = source->GetBufferHealth();

        min_speed = std::min(min_speed, health.speed_factor);
        min_frames_ahead = std::min(min_frames_ahead, health.frames_ahead);
        any_needs_wait = any_needs_wait || health.needs_buffer_wait;
        total_cached += health.total_cached;
        total_frames += health.total_frames;
    }

    combined.frames_ahead = (min_frames_ahead == INT_MAX) ? 0 : min_frames_ahead;
    combined.speed_factor = min_speed;
    combined.needs_buffer_wait = any_needs_wait;
    combined.total_cached = total_cached;
    combined.total_frames = total_frames;

    return combined;
}

//=============================================================================
// Playlist Support
//=============================================================================

void MultiCacheCoordinator::SetActiveSource(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sources_.find(source_id) != sources_.end()) {
        active_source_id_ = source_id;
        Debug::Log("MultiCacheCoordinator: Active source set to: " + source_id);
    }
}

std::string MultiCacheCoordinator::GetActiveSourceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_source_id_;
}

bool MultiCacheCoordinator::SwapActiveSource(
    const std::string& new_source_id,
    const std::vector<std::string>& files,
    const std::string& layer,
    double fps,
    PipelineMode pipeline_mode)
{
    // Remember old source
    std::string old_source_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_source_id = active_source_id_;
    }

    // Add new source (this acquires lock internally)
    if (!AddImageSequenceSource(new_source_id, files, layer, fps, pipeline_mode)) {
        return false;
    }

    // Set as active
    SetActiveSource(new_source_id);

    // Remove old source (with grace period handling done by caller if needed)
    if (!old_source_id.empty() && old_source_id != new_source_id) {
        RemoveSource(old_source_id);
    }

    Debug::Log("MultiCacheCoordinator: Swapped from '" + old_source_id +
               "' to '" + new_source_id + "'");
    return true;
}

//=============================================================================
// Dual-View Support
//=============================================================================

bool MultiCacheCoordinator::AddLeftSource(
    const std::vector<std::string>& files,
    const std::string& layer,
    double fps,
    PipelineMode pipeline_mode)
{
    return AddImageSequenceSource(LEFT_SOURCE_ID, files, layer, fps, pipeline_mode);
}

bool MultiCacheCoordinator::AddRightSource(
    const std::vector<std::string>& files,
    const std::string& layer,
    double fps,
    PipelineMode pipeline_mode)
{
    return AddImageSequenceSource(RIGHT_SOURCE_ID, files, layer, fps, pipeline_mode);
}

GLuint MultiCacheCoordinator::GetLeftFrame(int frame, int& width, int& height, bool& exact) {
    return GetFrame(LEFT_SOURCE_ID, frame, width, height, exact);
}

GLuint MultiCacheCoordinator::GetRightFrame(int frame, int& width, int& height, bool& exact) {
    return GetFrame(RIGHT_SOURCE_ID, frame, width, height, exact);
}

} // namespace ump
