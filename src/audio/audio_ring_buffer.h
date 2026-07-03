// AudioRingBuffer — Phase 5.0.a, ported from old app.
//
// Lock-free SPSC (single-producer / single-consumer) ring buffer for
// audio samples.
//   Producer = AudioDecoder's decode thread (writes resampled PCM)
//   Consumer = CoreAudio render callback (drains PCM into the device)
//
// Thread-safe for exactly one producer + one consumer. Multiple
// producers or consumers will corrupt the indices.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace qcv {

class AudioRingBuffer
{
public:
    // Default ~2 MB capacity ≈ 5.5 s at 48 kHz stereo float32
    // (384,000 B/s). The exact size isn't load-bearing — decode
    // thread back-pressures when full so anything ≥ a few frames
    // works. Both decoder classes now pass an explicit ~500 ms
    // capacity and hold a ~100 ms depth target instead of filling
    // to the brim (deep buffering delays producer-side changes and
    // buys nothing once the sync servo bounds drift).
    explicit AudioRingBuffer(size_t capacityBytes = 2 * 1024 * 1024)
        : m_capacity(capacityBytes)
        , m_buffer(capacityBytes)
        , m_writePos(0)
        , m_readPos(0)
    {}

    // Reset both indices. Call on seek / stop.
    void clear() noexcept
    {
        m_writePos.store(0, std::memory_order_release);
        m_readPos .store(0, std::memory_order_release);
    }

    // Bytes available to read (consumer perspective).
    size_t availableRead() const noexcept
    {
        const size_t w = m_writePos.load(std::memory_order_acquire);
        const size_t r = m_readPos .load(std::memory_order_relaxed);
        return (w >= r) ? (w - r) : 0;
    }

    // Bytes available to write (producer perspective).
    size_t availableWrite() const noexcept
    {
        return m_capacity - availableRead();
    }

    // Producer-only. Returns bytes actually written.
    size_t write(const void *data, size_t bytes) noexcept
    {
        const size_t avail = availableWrite();
        const size_t n = (bytes < avail) ? bytes : avail;
        if (n == 0) return 0;

        const size_t w = m_writePos.load(std::memory_order_relaxed) % m_capacity;
        const auto *src = static_cast<const uint8_t *>(data);

        const size_t firstChunk = m_capacity - w;
        if (firstChunk >= n) {
            std::memcpy(m_buffer.data() + w, src, n);
        } else {
            std::memcpy(m_buffer.data() + w, src, firstChunk);
            std::memcpy(m_buffer.data(), src + firstChunk, n - firstChunk);
        }
        m_writePos.fetch_add(n, std::memory_order_release);
        return n;
    }

    // Consumer-only. Returns bytes actually read.
    size_t read(void *dest, size_t bytes) noexcept
    {
        const size_t avail = availableRead();
        const size_t n = (bytes < avail) ? bytes : avail;
        if (n == 0) return 0;

        const size_t r = m_readPos.load(std::memory_order_relaxed) % m_capacity;
        auto *dst = static_cast<uint8_t *>(dest);

        const size_t firstChunk = m_capacity - r;
        if (firstChunk >= n) {
            std::memcpy(dst, m_buffer.data() + r, n);
        } else {
            std::memcpy(dst, m_buffer.data() + r, firstChunk);
            std::memcpy(dst + firstChunk, m_buffer.data(), n - firstChunk);
        }
        m_readPos.fetch_add(n, std::memory_order_release);
        return n;
    }

    size_t capacity() const noexcept { return m_capacity; }
    bool   isEmpty() const noexcept  { return availableRead() == 0; }

private:
    const size_t           m_capacity;
    std::vector<uint8_t>   m_buffer;
    std::atomic<size_t>    m_writePos;
    std::atomic<size_t>    m_readPos;
};

} // namespace qcv
