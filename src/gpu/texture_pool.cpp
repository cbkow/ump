#include "texture_pool.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <thread>

namespace ump {

    // Static helper for memory calculations
    size_t TextureInfo::GetBytesPerPixel(GLenum internal_format, GLenum type) {
        size_t component_size = 1;
        size_t num_components = 4; // Default RGBA

        // Determine component size based on type
        switch (type) {
            case GL_UNSIGNED_BYTE:
                component_size = 1;
                break;
            case GL_HALF_FLOAT:
                component_size = 2;
                break;
            case GL_FLOAT:
                component_size = 4;
                break;
            case GL_UNSIGNED_SHORT:
                component_size = 2;
                break;
            default:
                component_size = 1;
                break;
        }

        // Determine number of components based on internal format
        switch (internal_format) {
            case GL_R8:
            case GL_R16F:
            case GL_R32F:
                num_components = 1;
                break;
            case GL_RG8:
            case GL_RG16F:
            case GL_RG32F:
                num_components = 2;
                break;
            case GL_RGB8:
            case GL_RGB16F:
            case GL_RGB32F:
                num_components = 3;
                break;
            case GL_RGBA8:
            case GL_RGBA16F:
            case GL_RGBA32F:
            default:
                num_components = 4;
                break;
        }

        return component_size * num_components;
    }

    // Constructor implementations
    GPUTexturePool::GPUTexturePool() : GPUTexturePool(TexturePoolConfig{}) {
    }

    GPUTexturePool::GPUTexturePool(const TexturePoolConfig& config) : config_(config) {
        if (!config_.IsValid()) {
            Debug::Log("GPUTexturePool: WARNING - Invalid config, using defaults");
            config_ = TexturePoolConfig{};
        }

        Debug::Log("GPUTexturePool: Initialized with " + std::to_string(config_.max_memory_mb) +
                   "MB limit, " + std::to_string(config_.max_textures) + " max textures");
    }

    GPUTexturePool::~GPUTexturePool() {
        StopBackgroundEviction();
        ClearPool(true);  // Use fast shutdown in destructor
    }

    void GPUTexturePool::SetConfig(const TexturePoolConfig& config) {
        if (!config.IsValid()) {
            Debug::Log("GPUTexturePool: ERROR - Invalid config rejected");
            return;
        }

        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = config;
        Debug::Log("GPUTexturePool: Config updated - " + std::to_string(config_.max_memory_mb) + "MB limit");
    }

    TexturePoolConfig GPUTexturePool::GetConfig() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_;
    }

    GLuint GPUTexturePool::AcquireTexture(int width, int height, GLenum internal_format,
                                         GLenum format, GLenum type) {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        GLuint texture_id = 0;

        // Try to find compatible texture from pool
        if (FindCompatibleTexture(width, height, internal_format, format, type, texture_id)) {
            // Found compatible texture, mark as in use
            auto& info = texture_info_[texture_id];
            info->in_use = true;
            info->last_used = std::chrono::steady_clock::now();

            // Remove from available queue
            std::queue<GLuint> temp_queue;
            while (!available_textures_.empty()) {
                GLuint id = available_textures_.front();
                available_textures_.pop();
                if (id != texture_id) {
                    temp_queue.push(id);
                }
            }
            available_textures_ = std::move(temp_queue);

            stats_.cache_hits++;
            Debug::Log("GPUTexturePool: Reused texture " + std::to_string(texture_id) +
                       " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
        } else {
            // Create new texture
            texture_id = CreateNewTexture(width, height, internal_format, format, type);
            stats_.cache_misses++;
            stats_.textures_created++;
        }

        UpdateStats();
        return texture_id;
    }

    void GPUTexturePool::ReleaseTexture(GLuint texture_id) {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        auto it = texture_info_.find(texture_id);
        if (it != texture_info_.end() && it->second->in_use) {
            // Mark as available
            it->second->in_use = false;
            it->second->last_used = std::chrono::steady_clock::now();
            available_textures_.push(texture_id);

            Debug::Log("GPUTexturePool: Released texture " + std::to_string(texture_id));
            UpdateStats();
        } else {
            Debug::Log("GPUTexturePool: WARNING - Attempted to release unknown/unused texture " +
                       std::to_string(texture_id));
        }
    }

    GLuint GPUTexturePool::CreateNewTexture(int width, int height, GLenum internal_format,
                                          GLenum format, GLenum type) {
        GLuint texture_id = 0;
        glGenTextures(1, &texture_id);

        if (texture_id == 0) {
            Debug::Log("GPUTexturePool: ERROR - Failed to generate texture");
            return 0;
        }

        glBindTexture(GL_TEXTURE_2D, texture_id);

        // Set texture parameters for EXR usage
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Allocate texture storage
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, type, nullptr);

        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            Debug::Log("GPUTexturePool: ERROR - OpenGL error creating texture: " + std::to_string(error));
            glDeleteTextures(1, &texture_id);
            return 0;
        }

        // Create texture info
        auto info = std::make_unique<TextureInfo>();
        info->width = width;
        info->height = height;
        info->internal_format = internal_format;
        info->format = format;
        info->type = type;
        info->in_use = true;
        info->created_time = std::chrono::steady_clock::now();
        info->last_used = info->created_time;
        info->memory_size_bytes = info->CalculateMemorySize();

        texture_info_[texture_id] = std::move(info);

        Debug::Log("GPUTexturePool: Created new texture " + std::to_string(texture_id) +
                   " (" + std::to_string(width) + "x" + std::to_string(height) + ", " +
                   std::to_string(texture_info_[texture_id]->memory_size_bytes / 1024 / 1024) + "MB)");

        return texture_id;
    }

    bool GPUTexturePool::FindCompatibleTexture(int width, int height, GLenum internal_format,
                                              GLenum format, GLenum type, GLuint& texture_id) {
        // Look through available textures for exact match
        std::queue<GLuint> temp_queue;
        bool found = false;

        while (!available_textures_.empty() && !found) {
            GLuint id = available_textures_.front();
            available_textures_.pop();

            auto it = texture_info_.find(id);
            if (it != texture_info_.end() &&
                IsTextureCompatible(*it->second, width, height, internal_format, format, type)) {
                texture_id = id;
                found = true;
                // Put other textures back
                while (!available_textures_.empty()) {
                    temp_queue.push(available_textures_.front());
                    available_textures_.pop();
                }
            } else {
                temp_queue.push(id);
            }
        }

        available_textures_ = std::move(temp_queue);
        return found;
    }

    bool GPUTexturePool::IsTextureCompatible(const TextureInfo& info, int width, int height,
                                           GLenum internal_format, GLenum format, GLenum type) const {
        return info.width == width &&
               info.height == height &&
               info.internal_format == internal_format &&
               info.format == format &&
               info.type == type &&
               !info.in_use;
    }

    void GPUTexturePool::EvictOldTextures() {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        auto now = std::chrono::steady_clock::now();
        auto ttl = std::chrono::seconds(config_.texture_ttl_seconds);

        std::vector<GLuint> to_evict;

        // Find old unused textures
        for (auto& [texture_id, info] : texture_info_) {
            if (!info->in_use && (now - info->last_used) > ttl) {
                to_evict.push_back(texture_id);
            }
        }

        // Evict old textures
        for (GLuint texture_id : to_evict) {
            glDeleteTextures(1, &texture_id);
            texture_info_.erase(texture_id);
            stats_.textures_evicted++;
        }

        if (!to_evict.empty()) {
            Debug::Log("GPUTexturePool: Evicted " + std::to_string(to_evict.size()) + " old textures");
            stats_.last_eviction = now;
            UpdateStats();
        }
    }

    void GPUTexturePool::ClearPool(bool fast_shutdown) {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        if (fast_shutdown) {
            // Fast shutdown: Skip OpenGL calls, let OS/driver clean up GPU resources
            // This is safe when the OpenGL context is being destroyed anyway
            Debug::Log("GPUTexturePool: Fast shutdown - skipping GL cleanup for " +
                      std::to_string(texture_info_.size()) + " textures");
        } else {
            // Normal cleanup: Batch delete all textures for better performance
            if (!texture_info_.empty()) {
                // Collect all texture IDs
                std::vector<GLuint> texture_ids;
                texture_ids.reserve(texture_info_.size());
                for (const auto& [texture_id, info] : texture_info_) {
                    texture_ids.push_back(texture_id);
                }

                // Batch delete all textures at once (much faster than one-by-one)
                glDeleteTextures(static_cast<GLsizei>(texture_ids.size()), texture_ids.data());

                Debug::Log("GPUTexturePool: Batch deleted " + std::to_string(texture_ids.size()) + " textures");
            }
        }

        texture_info_.clear();
        while (!available_textures_.empty()) {
            available_textures_.pop();
        }

        stats_ = TexturePoolStats{};
        Debug::Log("GPUTexturePool: Cleared all textures");
    }

    void GPUTexturePool::UpdateStats() {
        // Called with pool_mutex_ already locked
        stats_.total_textures = texture_info_.size();
        stats_.textures_available = available_textures_.size();
        stats_.textures_in_use = stats_.total_textures - stats_.textures_available;

        stats_.total_memory_bytes = 0;
        stats_.memory_in_use_bytes = 0;

        for (const auto& [texture_id, info] : texture_info_) {
            stats_.total_memory_bytes += info->memory_size_bytes;
            if (info->in_use) {
                stats_.memory_in_use_bytes += info->memory_size_bytes;
            }
        }

        stats_.memory_usage_mb = static_cast<double>(stats_.total_memory_bytes) / (1024.0 * 1024.0);
        stats_.memory_limit_mb = static_cast<double>(config_.max_memory_mb);

        int total_requests = stats_.cache_hits + stats_.cache_misses;
        stats_.hit_ratio = total_requests > 0 ?
            static_cast<double>(stats_.cache_hits) / total_requests : 0.0;
    }

    TexturePoolStats GPUTexturePool::GetStats() const {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return stats_;
    }

    bool GPUTexturePool::IsTexturePooled(GLuint texture_id) const {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return texture_info_.find(texture_id) != texture_info_.end();
    }

    size_t GPUTexturePool::GetMemoryUsage() const {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return stats_.total_memory_bytes;
    }

    double GPUTexturePool::GetMemoryUsageMB() const {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return stats_.memory_usage_mb;
    }

    bool GPUTexturePool::IsMemoryLimitExceeded() const {
        return GetMemoryUsageMB() > static_cast<double>(config_.max_memory_mb);
    }

    void GPUTexturePool::StartBackgroundEviction() {
        if (eviction_active_.load()) {
            return; // Already running
        }

        should_stop_eviction_.store(false);
        eviction_active_.store(true);

        eviction_thread_ = std::thread(&GPUTexturePool::BackgroundEvictionWorker, this);
        Debug::Log("GPUTexturePool: Started background eviction thread");
    }

    void GPUTexturePool::StopBackgroundEviction() {
        if (!eviction_active_.load()) {
            return; // Not running
        }

        should_stop_eviction_.store(true);

        if (eviction_thread_.joinable()) {
            eviction_thread_.join();
        }

        eviction_active_.store(false);
        Debug::Log("GPUTexturePool: Stopped background eviction thread");
    }

    void GPUTexturePool::BackgroundEvictionWorker() {
        auto interval = std::chrono::milliseconds(config_.eviction_check_interval_ms);

        while (!should_stop_eviction_.load()) {
            std::this_thread::sleep_for(interval);

            if (should_stop_eviction_.load()) {
                break;
            }

            try {
                EvictOldTextures();

                // Check memory limit and force eviction if needed
                if (IsMemoryLimitExceeded()) {
                    size_t target_bytes = static_cast<size_t>(config_.max_memory_mb * 0.8 * 1024 * 1024);
                    EvictLRUTextures(target_bytes);
                }
            } catch (const std::exception& e) {
                Debug::Log("GPUTexturePool: Background eviction error: " + std::string(e.what()));
            }
        }
    }

    void GPUTexturePool::EvictLRUTextures(size_t target_memory_bytes) {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        if (stats_.total_memory_bytes <= target_memory_bytes) {
            return; // Already under target
        }

        // Create list of unused textures sorted by last used time (oldest first)
        std::vector<std::pair<std::chrono::steady_clock::time_point, GLuint>> candidates;

        for (const auto& [texture_id, info] : texture_info_) {
            if (!info->in_use) {
                candidates.emplace_back(info->last_used, texture_id);
            }
        }

        std::sort(candidates.begin(), candidates.end());

        // Evict oldest textures until under target
        size_t current_memory = stats_.total_memory_bytes;
        int evicted_count = 0;

        for (const auto& [last_used, texture_id] : candidates) {
            if (current_memory <= target_memory_bytes) {
                break;
            }

            auto it = texture_info_.find(texture_id);
            if (it != texture_info_.end()) {
                current_memory -= it->second->memory_size_bytes;
                glDeleteTextures(1, &texture_id);
                texture_info_.erase(texture_id);
                evicted_count++;
                stats_.textures_evicted++;
            }
        }

        if (evicted_count > 0) {
            Debug::Log("GPUTexturePool: LRU evicted " + std::to_string(evicted_count) +
                       " textures to free memory");
            UpdateStats();
        }
    }

} // namespace ump