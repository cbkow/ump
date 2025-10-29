#include "difference_cache.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>

// STB image for PNG loading (implementation in main.cpp)
#include "../../external/glfw/deps/stb_image.h"

namespace fs = std::filesystem;

namespace ump {

// ============================================================================
// CachedDifference Implementation
// ============================================================================

void DifferenceCache::CachedDifference::ReleaseTexture() {
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
        texture_id = 0;
    }
    is_valid = false;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

DifferenceCache::DifferenceCache() {
    Debug::Log("DifferenceCache: Constructor");
}

DifferenceCache::~DifferenceCache() {
    Debug::Log("DifferenceCache: Destructor");
    Cleanup();
}

// ============================================================================
// Initialization
// ============================================================================

bool DifferenceCache::Initialize(
    const std::string& combined_cache_dir,
    int total_frames,
    double frame_rate,
    CacheMode mode)
{
    Debug::Log("DifferenceCache: Initializing");
    Debug::Log("  Combined cache: " + combined_cache_dir);
    Debug::Log("  Total frames: " + std::to_string(total_frames));
    Debug::Log("  Mode: " + std::string(mode == CacheMode::RAM_ONLY ? "RAM_ONLY" : "DISK_FALLBACK"));

    combined_cache_dir_ = combined_cache_dir;
    total_frames_ = total_frames;
    frame_rate_ = frame_rate;
    cache_mode_ = mode;

    // Verify sequences exist
    if (!AreSequencesReady()) {
        Debug::Log("ERROR: DifferenceCache: Combined cache not ready on disk");
        return false;
    }

    // Pre-load all frames if RAM_ONLY mode
    if (cache_mode_ == CacheMode::RAM_ONLY) {
        Debug::Log("DifferenceCache: Pre-loading all frames into RAM...");
        if (!PreloadAllFrames()) {
            Debug::Log("ERROR: DifferenceCache: Failed to preload frames");
            return false;
        }
        Debug::Log("DifferenceCache: All frames preloaded successfully!");
    }

    initialized_ = true;
    Debug::Log("DifferenceCache: Initialized successfully");
    return true;
}

void DifferenceCache::Cleanup() {
    Debug::Log("DifferenceCache: Cleanup");

    // Clear RAM cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        ram_cache_.clear();
    }

    initialized_ = false;
}

bool DifferenceCache::AreSequencesReady() const {
    // Check if combined cache directory exists
    if (!fs::exists(combined_cache_dir_)) {
        return false;
    }

    // Sample check: verify first, middle, and last frames exist
    if (total_frames_ <= 0) {
        return false;
    }

    std::vector<int> sample_frames = {0, total_frames_ / 2, total_frames_ - 1};
    for (int frame : sample_frames) {
        std::string frame_path = GetDifferenceFramePath(frame);

        if (!fs::exists(frame_path)) {
            Debug::Log("DifferenceCache: Missing combined frame " + std::to_string(frame));
            return false;
        }
    }

    return true;
}

// ============================================================================
// Frame Access
// ============================================================================

bool DifferenceCache::GetDifferenceTexture(int frame_number, GLuint& texture_id, int& width, int& height) {
    if (!initialized_) {
        Debug::Log("ERROR: DifferenceCache not initialized");
        return false;
    }

    // Bounds check
    if (frame_number < 0 || frame_number >= total_frames_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Check RAM cache first
    auto it = ram_cache_.find(frame_number);
    if (it != ram_cache_.end() && it->second->is_valid) {
        texture_id = it->second->texture_id;
        width = it->second->width;
        height = it->second->height;
        it->second->last_accessed = std::chrono::steady_clock::now();
        cache_hits_++;
        return true;
    }

    // Cache miss - load pre-computed difference from disk
    cache_misses_++;

    // Load pre-computed difference frame from disk (already composited!)
    int diff_width = 0, diff_height = 0;
    GLuint diff_tex = LoadPNGTexture(GetDifferenceFramePath(frame_number), diff_width, diff_height);
    if (diff_tex == 0) {
        Debug::Log("ERROR: Failed to load pre-computed difference frame " + std::to_string(frame_number));
        return false;
    }

    // Cache the texture
    auto cached = std::make_unique<CachedDifference>();
    cached->texture_id = diff_tex;
    cached->width = diff_width;
    cached->height = diff_height;
    cached->last_accessed = std::chrono::steady_clock::now();
    cached->is_valid = true;

    texture_id = diff_tex;
    width = diff_width;
    height = diff_height;

    ram_cache_[frame_number] = std::move(cached);
    disk_loads_++;

    // Check if we need to evict (only in DISK_FALLBACK mode)
    if (cache_mode_ == CacheMode::DISK_FALLBACK) {
        if (CalculateCacheMemoryUsage() > max_cache_size_mb_ * 1024 * 1024) {
            EvictLRUFrames();
        }
    }

    return true;
}

// ============================================================================
// PNG Loading from Disk
// ============================================================================

GLuint DifferenceCache::LoadPNGTexture(const std::string& path, int& width, int& height) {
    // Load PNG using stb_image
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 3);  // Force RGB
    if (!data) {
        Debug::Log("ERROR: Failed to load PNG: " + path);
        return 0;
    }

    // Create OpenGL texture
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    // Free stb_image data
    stbi_image_free(data);

    return texture_id;
}

std::string DifferenceCache::GetDifferenceFramePath(int frame_number) const {
    std::ostringstream oss;
    oss << combined_cache_dir_ << "/frame_" << std::setw(5) << std::setfill('0') << frame_number << ".png";
    return oss.str();
}

// ============================================================================
// RAM-Only Preloading
// ============================================================================

bool DifferenceCache::PreloadAllFrames() {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    for (int frame = 0; frame < total_frames_; ++frame) {
        std::string frame_path = GetDifferenceFramePath(frame);

        int width = 0, height = 0;
        GLuint texture = LoadPNGTexture(frame_path, width, height);

        if (texture == 0) {
            Debug::Log("ERROR: Failed to preload frame " + std::to_string(frame));
            return false;
        }

        // Cache the frame
        auto cached = std::make_unique<CachedDifference>();
        cached->texture_id = texture;
        cached->width = width;
        cached->height = height;
        cached->last_accessed = std::chrono::steady_clock::now();
        cached->is_valid = true;

        ram_cache_[frame] = std::move(cached);

        // Log progress every 100 frames
        if ((frame + 1) % 100 == 0) {
            Debug::Log("DifferenceCache: Preloaded " + std::to_string(frame + 1) + "/" + std::to_string(total_frames_) + " frames");
        }
    }

    Debug::Log("DifferenceCache: Preload complete - " + std::to_string(total_frames_) + " frames in RAM");
    return true;
}

// ============================================================================
// LRU Eviction
// ============================================================================

void DifferenceCache::EvictLRUFrames() {
    // Build list of (frame_number, last_accessed) pairs
    std::vector<std::pair<int, std::chrono::steady_clock::time_point>> lru_list;

    for (const auto& pair : ram_cache_) {
        lru_list.emplace_back(pair.first, pair.second->last_accessed);
    }

    // Sort by last_accessed (oldest first)
    std::sort(lru_list.begin(), lru_list.end(),
        [](const auto& a, const auto& b) {
            return a.second < b.second;
        });

    // Evict oldest 25% of frames
    size_t evict_count = lru_list.size() / 4;
    for (size_t i = 0; i < evict_count && i < lru_list.size(); ++i) {
        int frame_number = lru_list[i].first;
        ram_cache_.erase(frame_number);
    }

    Debug::Log("DifferenceCache: Evicted " + std::to_string(evict_count) + " LRU frames");
}

size_t DifferenceCache::CalculateCacheMemoryUsage() const {
    // Estimate: RGBA8 texture = width * height * 4 bytes
    size_t total_bytes = 0;

    for (const auto& pair : ram_cache_) {
        if (pair.second->is_valid) {
            total_bytes += pair.second->width * pair.second->height * 4;
        }
    }

    return total_bytes;
}

// ============================================================================
// Statistics
// ============================================================================

DifferenceCache::CacheStats DifferenceCache::GetStats() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    CacheStats stats;
    stats.total_cached_frames = ram_cache_.size();
    stats.cache_hits = cache_hits_.load();
    stats.cache_misses = cache_misses_.load();
    stats.disk_loads = disk_loads_.load();

    size_t total_requests = stats.cache_hits + stats.cache_misses;
    stats.hit_ratio = total_requests > 0 ? (float)stats.cache_hits / total_requests : 0.0f;

    return stats;
}

} // namespace ump
