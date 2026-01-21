#include "cache_window_engine.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace ump {

//=============================================================================
// Constructor
//=============================================================================

CacheWindowEngine::CacheWindowEngine() {
    // Defaults set in header
}

//=============================================================================
// Configuration
//=============================================================================

void CacheWindowEngine::SetTotalFrames(int total_frames) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    total_frames_ = total_frames;
    boundary_length_dirty_ = true;
}

void CacheWindowEngine::SetBoundaries(int start, int end) {
    // Validate and swap if needed
    if (start > end) std::swap(start, end);

    boundary_start_.store(start);
    boundary_end_.store(end);
    boundary_length_dirty_ = true;
}

void CacheWindowEngine::ClearBoundaries() {
    boundary_start_.store(-1);
    boundary_end_.store(-1);
    boundary_length_dirty_ = true;
}

void CacheWindowEngine::SetWindow(int behind, int ahead) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    read_behind_ = behind;
    read_ahead_ = ahead;
}

//=============================================================================
// Playhead Management
//=============================================================================

void CacheWindowEngine::UpdatePlayhead(int frame) {
    // Wrap frame into boundaries before storing
    int wrapped = WrapFrame(frame);
    playhead_.store(wrapped);
}

//=============================================================================
// Frame Window Queries
//=============================================================================

std::vector<int> CacheWindowEngine::GetFrameWindow() const {
    std::vector<int> window;

    int playhead = playhead_.load();
    int boundary_start = GetBoundaryStart();
    int boundary_end = GetBoundaryEnd();
    int boundary_len = GetBoundaryLength();

    // Safety check - return just playhead if boundaries not valid
    if (boundary_len <= 0) {
        window.push_back(playhead);
        return window;
    }

    // Clamp window size to boundary length (can't have more frames than exist)
    int effective_ahead = std::min(read_ahead_, boundary_len - 1);
    int effective_behind = std::min(read_behind_, boundary_len - 1);

    // Reserve space for efficiency
    window.reserve(effective_behind + effective_ahead + 1);

    // 1. Current frame (highest priority)
    // Ensure playhead is within boundaries
    int safe_playhead = playhead;
    if (safe_playhead < boundary_start || safe_playhead > boundary_end) {
        // Wrap playhead into valid range
        while (safe_playhead > boundary_end) safe_playhead -= boundary_len;
        while (safe_playhead < boundary_start) safe_playhead += boundary_len;
    }
    window.push_back(safe_playhead);

    // Use a set to track unique frames (avoid duplicates when window > boundary)
    std::set<int> added;
    added.insert(safe_playhead);

    // 2. Frames AHEAD sorted by distance (closest first)
    for (int i = 1; i <= effective_ahead; i++) {
        int frame = safe_playhead + i;
        // Wrap into [boundary_start, boundary_end]
        while (frame > boundary_end) frame -= boundary_len;
        while (frame < boundary_start) frame += boundary_len;
        if (added.find(frame) == added.end()) {
            window.push_back(frame);
            added.insert(frame);
        }
    }

    // 3. Frames BEHIND sorted by distance (closest first)
    for (int i = 1; i <= effective_behind; i++) {
        int frame = safe_playhead - i;
        // Wrap into [boundary_start, boundary_end]
        while (frame < boundary_start) frame += boundary_len;
        while (frame > boundary_end) frame -= boundary_len;
        if (added.find(frame) == added.end()) {
            window.push_back(frame);
            added.insert(frame);
        }
    }

    return window;
}

std::set<int> CacheWindowEngine::GetFrameWindowSet() const {
    auto window = GetFrameWindow();
    return std::set<int>(window.begin(), window.end());
}

bool CacheWindowEngine::IsInWindow(int frame) const {
    int playhead = playhead_.load();

    // Quick check: is the absolute circular distance within window range?
    int dist = AbsCircularDistance(playhead, frame);
    return dist <= std::max(read_ahead_, read_behind_);
}

//=============================================================================
// Circular Math Utilities
//=============================================================================

int CacheWindowEngine::WrapFrame(int frame) const {
    int boundary_start = GetBoundaryStart();
    int boundary_end = GetBoundaryEnd();
    int boundary_len = GetBoundaryLength();

    if (boundary_len <= 0) return frame;

    // Wrap into [boundary_start, boundary_end]
    while (frame > boundary_end) frame -= boundary_len;
    while (frame < boundary_start) frame += boundary_len;

    return frame;
}

int CacheWindowEngine::CircularDistance(int from, int to) const {
    int boundary_len = GetBoundaryLength();

    if (boundary_len <= 0) {
        return to - from;  // Linear distance
    }

    // Normalize both frames to boundary range
    from = WrapFrame(from);
    to = WrapFrame(to);

    // Calculate forward distance (to - from, wrapping)
    int forward = to - from;
    if (forward < 0) forward += boundary_len;

    // Calculate backward distance
    int backward = boundary_len - forward;

    // Return shortest distance with sign
    // Positive = to is ahead of from
    // Negative = to is behind from
    if (forward <= backward) {
        return forward;
    } else {
        return -backward;
    }
}

int CacheWindowEngine::AbsCircularDistance(int from, int to) const {
    return std::abs(CircularDistance(from, to));
}

//=============================================================================
// Boundary Queries
//=============================================================================

int CacheWindowEngine::GetBoundaryStart() const {
    int custom = boundary_start_.load();
    return (custom >= 0) ? custom : 0;
}

int CacheWindowEngine::GetBoundaryEnd() const {
    int custom = boundary_end_.load();
    if (custom > 0) return custom;
    // Ensure we return at least 0 even if total_frames_ not set yet
    return (total_frames_ > 0) ? (total_frames_ - 1) : 0;
}

int CacheWindowEngine::GetBoundaryLength() const {
    // Always recalculate to avoid stale cached values during initialization
    // The calculation is cheap (just arithmetic on atomic loads)
    int start = GetBoundaryStart();
    int end = GetBoundaryEnd();
    int len = end - start + 1;
    return (len > 0) ? len : 1;  // Ensure at least 1 to avoid division by zero
}

bool CacheWindowEngine::HasCustomBoundaries() const {
    return (boundary_start_.load() >= 0 && boundary_end_.load() > 0);
}

void CacheWindowEngine::UpdateBoundaryLength() const {
    // This function is no longer used but kept for compatibility
    int start = GetBoundaryStart();
    int end = GetBoundaryEnd();
    cached_boundary_length_ = end - start + 1;
    if (cached_boundary_length_ <= 0) cached_boundary_length_ = 1;
    boundary_length_dirty_ = false;
}

} // namespace ump
