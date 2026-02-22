#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace qcview {

/**
 * Lock-free SPSC (Single Producer Single Consumer) ring buffer for audio samples
 *
 * Producer: Decode thread writes PCM data
 * Consumer: miniaudio callback reads PCM data
 *
 * Thread-safe for exactly one producer and one consumer.
 * Do NOT use with multiple producers or multiple consumers.
 */
class AudioRingBuffer {
public:
    // Default ~2MB capacity (~11 seconds at 48kHz stereo float32)
    explicit AudioRingBuffer(size_t capacity_bytes = 2 * 1024 * 1024)
        : capacity_(capacity_bytes)
        , buffer_(capacity_bytes)
        , write_pos_(0)
        , read_pos_(0) {}

    // Reset buffer (call when seeking or stopping)
    void Clear() {
        write_pos_.store(0, std::memory_order_release);
        read_pos_.store(0, std::memory_order_release);
    }

    // Returns number of bytes available to read
    size_t AvailableRead() const {
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_relaxed);
        return (w >= r) ? (w - r) : 0;
    }

    // Returns number of bytes available to write
    size_t AvailableWrite() const {
        return capacity_ - AvailableRead();
    }

    // Write data to buffer (producer thread only)
    // Returns number of bytes actually written
    size_t Write(const void* data, size_t bytes) {
        size_t avail = AvailableWrite();
        size_t to_write = (bytes < avail) ? bytes : avail;

        if (to_write == 0) return 0;

        size_t w = write_pos_.load(std::memory_order_relaxed) % capacity_;
        const uint8_t* src = static_cast<const uint8_t*>(data);

        // Handle wrap-around
        size_t first_chunk = capacity_ - w;
        if (first_chunk >= to_write) {
            std::memcpy(buffer_.data() + w, src, to_write);
        } else {
            std::memcpy(buffer_.data() + w, src, first_chunk);
            std::memcpy(buffer_.data(), src + first_chunk, to_write - first_chunk);
        }

        write_pos_.fetch_add(to_write, std::memory_order_release);
        return to_write;
    }

    // Read data from buffer (consumer thread only)
    // Returns number of bytes actually read
    size_t Read(void* dest, size_t bytes) {
        size_t avail = AvailableRead();
        size_t to_read = (bytes < avail) ? bytes : avail;

        if (to_read == 0) return 0;

        size_t r = read_pos_.load(std::memory_order_relaxed) % capacity_;
        uint8_t* dst = static_cast<uint8_t*>(dest);

        // Handle wrap-around
        size_t first_chunk = capacity_ - r;
        if (first_chunk >= to_read) {
            std::memcpy(dst, buffer_.data() + r, to_read);
        } else {
            std::memcpy(dst, buffer_.data() + r, first_chunk);
            std::memcpy(dst + first_chunk, buffer_.data(), to_read - first_chunk);
        }

        read_pos_.fetch_add(to_read, std::memory_order_release);
        return to_read;
    }

    // Skip data without reading (useful for fast-forward)
    size_t Skip(size_t bytes) {
        size_t avail = AvailableRead();
        size_t to_skip = (bytes < avail) ? bytes : avail;

        if (to_skip > 0) {
            read_pos_.fetch_add(to_skip, std::memory_order_release);
        }
        return to_skip;
    }

    size_t GetCapacity() const { return capacity_; }

    // Check if buffer is empty
    bool IsEmpty() const { return AvailableRead() == 0; }

    // Check if buffer is full (or nearly full)
    bool IsFull() const { return AvailableWrite() < 4096; }

private:
    size_t capacity_;
    std::vector<uint8_t> buffer_;
    std::atomic<size_t> write_pos_;
    std::atomic<size_t> read_pos_;
};

} // namespace qcview
