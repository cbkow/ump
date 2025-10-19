#pragma once

#include <vector>
#include <queue>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <memory>

#ifdef _WIN32
#include <glad/gl.h>
#else
#include <GL/gl.h>
#endif

namespace ump {

    // Configuration for texture pool behavior
    struct TexturePoolConfig {
        size_t max_memory_mb = 2048;           // Maximum GPU memory usage (MB)
        size_t max_textures = 1000;           // Maximum number of pooled textures
        int eviction_check_interval_ms = 5000; // How often to check for eviction (ms)
        int texture_ttl_seconds = 300;        // Time-to-live for unused textures (5 min)
        bool enable_memory_monitoring = true;  // Track memory usage statistics

        bool IsValid() const {
            return max_memory_mb >= 512 && max_memory_mb <= 16384 &&
                   max_textures >= 100 && max_textures <= 10000 &&
                   eviction_check_interval_ms >= 1000 && eviction_check_interval_ms <= 30000 &&
                   texture_ttl_seconds >= 60 && texture_ttl_seconds <= 3600;
        }
    };

    // Information about a pooled texture
    struct TextureInfo {
        int width = 0;
        int height = 0;
        GLenum internal_format = GL_RGBA8;
        GLenum format = GL_RGBA;
        GLenum type = GL_UNSIGNED_BYTE;
        bool in_use = false;
        std::chrono::steady_clock::time_point created_time;
        std::chrono::steady_clock::time_point last_used;
        size_t memory_size_bytes = 0;

        // Calculate texture memory size
        size_t CalculateMemorySize() const {
            size_t bytes_per_pixel = GetBytesPerPixel(internal_format, type);
            return width * height * bytes_per_pixel;
        }

    private:
        static size_t GetBytesPerPixel(GLenum internal_format, GLenum type);
    };

    // Statistics for monitoring texture pool performance
    struct TexturePoolStats {
        size_t total_textures = 0;
        size_t textures_in_use = 0;
        size_t textures_available = 0;
        size_t total_memory_bytes = 0;
        size_t memory_in_use_bytes = 0;
        double memory_usage_mb = 0.0;
        double memory_limit_mb = 0.0;
        int cache_hits = 0;
        int cache_misses = 0;
        int textures_created = 0;
        int textures_evicted = 0;
        double hit_ratio = 0.0;
        std::chrono::steady_clock::time_point last_eviction;
    };

    // GPU texture pool for efficient texture management
    class GPUTexturePool {
    public:
        GPUTexturePool();
        explicit GPUTexturePool(const TexturePoolConfig& config);
        ~GPUTexturePool();

        // Configuration management
        void SetConfig(const TexturePoolConfig& config);
        TexturePoolConfig GetConfig() const;

        // Core texture management
        GLuint AcquireTexture(int width, int height, GLenum internal_format = GL_RGBA16F,
                             GLenum format = GL_RGBA, GLenum type = GL_HALF_FLOAT);
        void ReleaseTexture(GLuint texture_id);

        // Pool management
        void EvictOldTextures();
        void ClearPool(bool fast_shutdown = false);  // fast_shutdown skips GL calls for instant cleanup
        void CompactPool();  // Remove unused textures to free memory

        // Statistics and monitoring
        TexturePoolStats GetStats() const;
        bool IsTexturePooled(GLuint texture_id) const;
        size_t GetMemoryUsage() const;      // Current memory usage in bytes
        double GetMemoryUsageMB() const;    // Current memory usage in MB
        bool IsMemoryLimitExceeded() const;

        // Automatic management
        void StartBackgroundEviction();
        void StopBackgroundEviction();

    private:
        // Configuration
        TexturePoolConfig config_;
        mutable std::mutex config_mutex_;

        // Texture storage
        std::unordered_map<GLuint, std::unique_ptr<TextureInfo>> texture_info_;
        std::queue<GLuint> available_textures_;
        mutable std::mutex pool_mutex_;

        // Statistics
        mutable TexturePoolStats stats_;
        mutable std::mutex stats_mutex_;

        // Background eviction
        std::thread eviction_thread_;
        std::atomic<bool> should_stop_eviction_{false};
        std::atomic<bool> eviction_active_{false};

        // Private methods
        GLuint CreateNewTexture(int width, int height, GLenum internal_format,
                               GLenum format, GLenum type);
        bool FindCompatibleTexture(int width, int height, GLenum internal_format,
                                  GLenum format, GLenum type, GLuint& texture_id);
        void EvictLRUTextures(size_t target_memory_bytes);
        void BackgroundEvictionWorker();
        void UpdateStats();
        bool IsTextureCompatible(const TextureInfo& info, int width, int height,
                                GLenum internal_format, GLenum format, GLenum type) const;

        // Memory calculations
        size_t CalculateTextureMemory(int width, int height, GLenum internal_format, GLenum type) const;
    };

} // namespace ump