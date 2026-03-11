#pragma once

#ifdef __linux__

#include <atomic>
#include <thread>
#include <cstdint>
#include <functional>

namespace qcview {

/**
 * Callback signature for audio data requests.
 * Matches WasapiDataCallback for drop-in compatibility.
 *
 * @param device Pointer to the PipeWireAudioDevice (for context)
 * @param output Buffer to fill with float32 stereo interleaved samples
 * @param frameCount Number of frames to generate (frame = 2 samples for stereo)
 * @param userData User-provided context pointer
 */
using PipeWireDataCallback = void(*)(void* device, float* output,
                                      uint32_t frameCount, void* userData);

/**
 * Configuration for PipeWireAudioDevice initialization.
 * Mirrors WasapiDeviceConfig for API compatibility.
 */
struct PipeWireDeviceConfig {
    PipeWireDataCallback dataCallback = nullptr;
    void* userData = nullptr;
    int sampleRate = 48000;
    int channels = 2;
    int bufferSizeMs = 10;
};

/**
 * PipeWireAudioDevice - PipeWire audio output for Linux
 *
 * Provides low-latency audio output using PipeWire.
 * Drop-in replacement for WasapiAudioDevice with matching API.
 *
 * Key features:
 * - Event-driven rendering via pw_stream process callback
 * - Float32 stereo output @ 48kHz
 * - Low-latency buffer configuration
 *
 * Usage:
 *   PipeWireAudioDevice device;
 *   PipeWireDeviceConfig config;
 *   config.dataCallback = MyCallback;
 *   config.userData = this;
 *   device.Initialize(config);
 *   device.Start();
 *   // ... playback ...
 *   device.Stop();
 *   device.Shutdown();
 */
class PipeWireAudioDevice {
public:
    PipeWireAudioDevice();
    ~PipeWireAudioDevice();

    PipeWireAudioDevice(const PipeWireAudioDevice&) = delete;
    PipeWireAudioDevice& operator=(const PipeWireAudioDevice&) = delete;

    bool Initialize(const PipeWireDeviceConfig& config);
    void Shutdown();

    void Start();
    void Stop();

    bool IsRunning() const { return running_.load(); }
    bool IsInitialized() const { return initialized_; }

    uint32_t GetBufferFrameCount() const { return buffer_frame_count_; }
    int GetSampleRate() const { return config_.sampleRate; }
    double GetBufferLatencySeconds() const;
    uint64_t GetSamplesWritten() const { return samples_written_.load(); }

    // PipeWire stream process callback (called from PipeWire thread)
    // Public because it's referenced by the C pw_stream_events struct
    static void OnProcess(void* userdata);

private:

    // PipeWire thread function — runs the pw_main_loop
    void PipeWireThread();

    PipeWireDeviceConfig config_;
    bool initialized_ = false;

    // PipeWire objects (stored as void* to avoid PipeWire headers in this header)
    void* pw_main_loop_ = nullptr;     // struct pw_main_loop*
    void* pw_stream_ = nullptr;        // struct pw_stream*

    // Thread management
    std::thread pw_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> thread_started_{false};
    std::atomic<bool> should_play_{false};

    // Buffer info
    uint32_t buffer_frame_count_ = 0;
    uint32_t stride_ = 0;  // bytes per frame

    // Position tracking
    std::atomic<uint64_t> samples_written_{0};
};

} // namespace qcview

#endif // __linux__
