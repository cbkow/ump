#include "shared_memory_pool.h"
#include "../utils/debug_utils.h"
#include <algorithm>

namespace qcview {

// Singleton instance
SharedMemoryPool& SharedMemoryPool::Instance() {
    static SharedMemoryPool instance;
    return instance;
}

SharedMemoryPool::SharedMemoryPool() {
    // Default 16 GB budget
    budget_bytes_ = static_cast<size_t>(16.0 * 1024 * 1024 * 1024);
    Debug::Log("SharedMemoryPool: Initialized with 16 GB default budget");
}

SharedMemoryPool::~SharedMemoryPool() {
    Clear();
}

void SharedMemoryPool::SetBudgetGB(double gb) {
    if (gb < 0.1) gb = 0.1;
    if (gb > 256.0) gb = 256.0;

    budget_bytes_ = static_cast<size_t>(gb * 1024 * 1024 * 1024);
    Debug::Log("SharedMemoryPool: Budget set to " + std::to_string(gb) + " GB");

    // Evict if new budget is smaller
    std::lock_guard<std::mutex> lock(mutex_);
    EvictIfNeeded();
}

double SharedMemoryPool::GetBudgetGB() const {
    return static_cast<double>(budget_bytes_.load()) / (1024.0 * 1024.0 * 1024.0);
}

void SharedMemoryPool::SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        // When disabled, clear all managed entries
        Clear();
    }
    Debug::Log("SharedMemoryPool: " + std::string(enabled ? "Enabled" : "Disabled"));
}

void SharedMemoryPool::RegisterEntry(const GlobalCacheKey& key, size_t bytes,
                                      std::function<void()> eviction_callback) {
    if (!enabled_) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if entry already exists
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        // Update existing entry
        used_bytes_ -= it->second.bytes;
        used_bytes_ += bytes;
        it->second.bytes = bytes;
        it->second.eviction_callback = std::move(eviction_callback);

        // Move to end of LRU
        RemoveFromLRU(key);
        lru_list_.push_back(key);
    } else {
        // Add new entry
        PoolEntry entry;
        entry.bytes = bytes;
        entry.eviction_callback = std::move(eviction_callback);

        entries_[key] = std::move(entry);
        lru_list_.push_back(key);
        used_bytes_ += bytes;
    }

    // Evict oldest entries if over budget
    EvictIfNeeded();
}

void SharedMemoryPool::TouchEntry(const GlobalCacheKey& key) {
    if (!enabled_) return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(key);
    if (it != entries_.end()) {
        // Move to end of LRU (most recently used)
        RemoveFromLRU(key);
        lru_list_.push_back(key);
    }
}

void SharedMemoryPool::RemoveEntry(const GlobalCacheKey& key) {
    if (!enabled_) return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(key);
    if (it != entries_.end()) {
        used_bytes_ -= it->second.bytes;
        RemoveFromLRU(key);
        entries_.erase(it);
    }
}

bool SharedMemoryPool::HasEntry(const GlobalCacheKey& key) const {
    if (!enabled_) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(key) != entries_.end();
}

PoolStats SharedMemoryPool::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    PoolStats stats;
    stats.budget_bytes = budget_bytes_;
    stats.used_bytes = used_bytes_;
    stats.eviction_count = eviction_count_;

    // Count entries by source
    for (const auto& [key, entry] : entries_) {
        switch (key.source) {
            case GlobalCacheKey::Source::TIMELINE:
                stats.timeline_entries++;
                break;
            case GlobalCacheKey::Source::STANDALONE_EXR:
                stats.exr_entries++;
                break;
            case GlobalCacheKey::Source::STANDALONE_VIDEO:
                stats.video_entries++;
                break;
        }
    }

    return stats;
}

void SharedMemoryPool::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Call eviction callbacks for all entries
    for (auto& [key, entry] : entries_) {
        if (entry.eviction_callback) {
            entry.eviction_callback();
        }
    }

    entries_.clear();
    lru_list_.clear();
    used_bytes_ = 0;

    Debug::Log("SharedMemoryPool: Cleared all entries");
}

void SharedMemoryPool::ClearSource(GlobalCacheKey::Source source) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find and remove all entries from the specified source
    std::vector<GlobalCacheKey> to_remove;
    for (const auto& [key, entry] : entries_) {
        if (key.source == source) {
            to_remove.push_back(key);
        }
    }

    for (const auto& key : to_remove) {
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            if (it->second.eviction_callback) {
                it->second.eviction_callback();
            }
            used_bytes_ -= it->second.bytes;
            RemoveFromLRU(key);
            entries_.erase(it);
        }
    }

    const char* source_name = "Unknown";
    switch (source) {
        case GlobalCacheKey::Source::TIMELINE: source_name = "Timeline"; break;
        case GlobalCacheKey::Source::STANDALONE_EXR: source_name = "EXR"; break;
        case GlobalCacheKey::Source::STANDALONE_VIDEO: source_name = "Video"; break;
    }

    Debug::Log("SharedMemoryPool: Cleared " + std::to_string(to_remove.size()) +
               " entries from " + source_name + " source");
}

void SharedMemoryPool::EvictIfNeeded() {
    // Must be called with mutex_ locked

    size_t budget = budget_bytes_;

    while (used_bytes_ > budget && !lru_list_.empty()) {
        // Get oldest entry (front of LRU list)
        GlobalCacheKey oldest_key = lru_list_.front();
        lru_list_.pop_front();

        auto it = entries_.find(oldest_key);
        if (it != entries_.end()) {
            // Call eviction callback to free resources
            if (it->second.eviction_callback) {
                it->second.eviction_callback();
            }

            used_bytes_ -= it->second.bytes;
            entries_.erase(it);
            eviction_count_++;
        }
    }
}

void SharedMemoryPool::RemoveFromLRU(const GlobalCacheKey& key) {
    // Must be called with mutex_ locked
    // Linear search is fine for typical cache sizes (< 10000 entries)
    lru_list_.remove(key);
}

} // namespace qcview
