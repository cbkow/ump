// SimpleLRU — Phase 7.4.b.3 port from old QCView's
// `direct_exr_cache.h:170-340`. Byte-budget LRU with O(1) Touch
// via list iterator map.
//
// Critical semantics (preserved verbatim from the old impl —
// these were tuned over the original year of development):
//
//   Get(key, value)   — touches the LRU (moves entry to MRU).
//                       Use this when the caller intends to keep
//                       the entry around (manual cache management).
//
//   Peek(key, value)  — does NOT touch the LRU. "For playback —
//                       don't keep old frames fresh." Already-
//                       displayed frames age out naturally.
//                       This is the load-bearing distinction.
//
//   Add(key, value, bytes)
//                     — replaces any existing entry for `key`,
//                       pushes to MRU, then evicts oldest entries
//                       in a `while (currentBytes_ > maxBytes_)`
//                       loop. Eviction callbacks fire OUTSIDE the
//                       lock to prevent priority inversion (the
//                       callback may acquire other mutexes).
//
// Thread safety: shared_mutex; readers (Get/Peek/Contains/GetKeys)
// take shared_lock, writers (Add/Remove/Touch/Clear) take
// unique_lock. Get takes unique_lock because it Touches.
//
// Backed by:
//   std::map<K, V>                 cache_         — value store
//   std::map<K, size_t>            sizes_         — per-entry bytes
//   std::list<K>                   lruList_       — MRU at back
//   std::unordered_map<K, list-it> iterMap_       — O(1) Touch
//   size_t                         maxBytes_,
//                                  currentBytes_  — totals

#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qcv {

template <typename K, typename V>
class SimpleLRU {
public:
    using EvictionCallback = std::function<void(const K &, const V &)>;

    void   setMaxBytes(std::size_t bytes) { m_maxBytes = bytes; }
    std::size_t maxBytes() const { return m_maxBytes; }
    std::size_t currentBytes() const { return m_currentBytes; }
    std::size_t count() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_cache.size();
    }

    void setEvictionCallback(EvictionCallback cb) {
        m_evictionCallback = std::move(cb);
    }

    bool contains(const K &key) const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_cache.find(key) != m_cache.end();
    }

    // Touches LRU. Use for manual cache management (cache thread's
    // touch-reverse pass that pulls closest-to-playhead frames to
    // MRU).
    bool get(const K &key, V &value) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_cache.find(key);
        if (it == m_cache.end()) return false;
        value = it->second;
        touchLocked(key);
        return true;
    }

    // Does NOT touch LRU — playback path. Allows already-displayed
    // frames to age out without artificially staying fresh.
    bool peek(const K &key, V &value) const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_cache.find(key);
        if (it == m_cache.end()) return false;
        value = it->second;
        return true;
    }

    void add(const K &key, const V &value, std::size_t bytes) {
        // Collect evicted entries, fire callbacks OUTSIDE the lock
        // to prevent priority inversion (eviction callback may
        // acquire other mutexes).
        std::vector<std::pair<K, V>> evicted;
        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);

            // Replace existing entry if present.
            auto it = m_cache.find(key);
            if (it != m_cache.end()) {
                m_currentBytes -= m_sizes[key];
                m_cache.erase(it);
                m_sizes.erase(key);
            }

            m_cache[key] = value;
            m_sizes[key] = bytes;
            m_currentBytes += bytes;
            touchLocked(key);

            // Single-entry eviction in a while loop until under budget.
            while (m_currentBytes > m_maxBytes && !m_lruList.empty()) {
                K oldest = m_lruList.front();
                m_lruList.pop_front();
                m_iterMap.erase(oldest);

                auto eit = m_cache.find(oldest);
                if (eit != m_cache.end()) {
                    evicted.emplace_back(oldest, eit->second);
                }
                m_currentBytes -= m_sizes[oldest];
                m_cache.erase(oldest);
                m_sizes.erase(oldest);
            }
        }
        if (m_evictionCallback) {
            for (auto &kv : evicted) {
                m_evictionCallback(kv.first, kv.second);
            }
        }
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_cache.clear();
        m_sizes.clear();
        m_lruList.clear();
        m_iterMap.clear();
        m_currentBytes = 0;
    }

    void remove(const K &key) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_cache.find(key);
        if (it == m_cache.end()) return;
        m_currentBytes -= m_sizes[key];
        m_cache.erase(it);
        m_sizes.erase(key);
        auto iit = m_iterMap.find(key);
        if (iit != m_iterMap.end()) {
            m_lruList.erase(iit->second);
            m_iterMap.erase(iit);
        }
    }

    bool removeAndGet(const K &key, V &value) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_cache.find(key);
        if (it == m_cache.end()) return false;
        value = it->second;
        m_currentBytes -= m_sizes[key];
        m_cache.erase(it);
        m_sizes.erase(key);
        auto iit = m_iterMap.find(key);
        if (iit != m_iterMap.end()) {
            m_lruList.erase(iit->second);
            m_iterMap.erase(iit);
        }
        return true;
    }

    std::vector<K> keys() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        std::vector<K> out;
        out.reserve(m_cache.size());
        for (const auto &p : m_cache) out.push_back(p.first);
        return out;
    }

private:
    // Caller must hold unique_lock(m_mutex).
    void touchLocked(const K &key) {
        auto it = m_iterMap.find(key);
        if (it != m_iterMap.end()) {
            m_lruList.erase(it->second);
        }
        m_lruList.push_back(key);
        m_iterMap[key] = std::prev(m_lruList.end());
    }

    mutable std::shared_mutex                                 m_mutex;
    std::map<K, V>                                            m_cache;
    std::map<K, std::size_t>                                  m_sizes;
    std::list<K>                                              m_lruList;
    std::unordered_map<K, typename std::list<K>::iterator>    m_iterMap;
    std::size_t                                               m_maxBytes = 0;
    std::size_t                                               m_currentBytes = 0;
    EvictionCallback                                          m_evictionCallback;
};

} // namespace qcv
