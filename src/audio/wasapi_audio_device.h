#pragma once

#include <atomic>
#include <thread>
#include <cstdint>
#include <functional>

// Forward declare COM types to avoid including Windows headers in the header
struct IAudioClient3;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;

namespace qcview {

/**
 * Callback signature for audio data requests.
 * Matches the pattern used by ProcessAudio in AudioPlayer/AudioMixer.
 *
 * @param device Pointer to the WasapiAudioDevice (for context)
 * @param output Buffer to fill with float32 stereo interleaved samples
 * @param frameCount Number of frames to generate (frame = 2 samples for stereo)
 * @param userData User-provided context pointer
 */
using WasapiDataCallback = void(*)(void* device, float* output,
                                   uint32_t frameCount, void* userData);

/**
 * Configuration for WasapiAudioDevice initialization.
 */
struct WasapiDeviceConfig {
    WasapiDataCallback dataCallback = nullptr;  // Called when audio data is needed
    void* userData = nullptr;                    // Passed to dataCallback
    int sampleRate = 48000;                      // Sample rate in Hz
    int channels = 2;                            // Number of channels (stereo)
    int bufferSizeMs = 10;                       // Desired buffer size in milliseconds
};

/**
 * WasapiAudioDevice - Direct WASAPI audio output for Windows
 *
 * Provides low-latency audio output using Windows Audio Session API (WASAPI).
 * Uses event-driven rendering with MMCSS thread priority for glitch-free playback.
 *
 * Key features:
 * - Event-driven rendering (no polling)
 * - MMCSS "Pro Audio" thread priority
 * - Automatic device loss recovery
 * - Float32 stereo output @ 48kHz
 *
 * Usage:
 *   WasapiAudioDevice device;
 *   WasapiDeviceConfig config;
 *   config.dataCallback = MyCallback;
 *   config.userData = this;
 *   device.Initialize(config);
 *   device.Start();
 *   // ... playback ...
 *   device.Stop();
 *   device.Shutdown();
 */
class WasapiAudioDevice {
public:
    WasapiAudioDevice();
    ~WasapiAudioDevice();

    // Prevent copying
    WasapiAudioDevice(const WasapiAudioDevice&) = delete;
    WasapiAudioDevice& operator=(const WasapiAudioDevice&) = delete;

    /**
     * Initialize the WASAPI audio device.
     * Creates audio client, configures format, and prepares for playback.
     *
     * @param config Device configuration including callback and format settings
     * @return true if initialization succeeded, false on error
     */
    bool Initialize(const WasapiDeviceConfig& config);

    /**
     * Shutdown the device and release all resources.
     * Safe to call multiple times or if not initialized.
     */
    void Shutdown();

    /**
     * Start audio playback.
     * Launches the render thread and begins calling the data callback.
     */
    void Start();

    /**
     * Stop audio playback.
     * Stops the render thread but keeps the device initialized.
     */
    void Stop();

    /**
     * Check if audio is currently playing.
     * @return true if Start() was called and playback is active
     */
    bool IsRunning() const { return running_.load(); }

    /**
     * Check if device is initialized.
     * @return true if Initialize() succeeded
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * Get the actual buffer size being used (in frames).
     * May differ from requested size based on device capabilities.
     */
    uint32_t GetBufferFrameCount() const { return bufferFrameCount_; }

    /**
     * Get the sample rate in Hz.
     */
    int GetSampleRate() const { return config_.sampleRate; }

    /**
     * Get current buffer latency in seconds.
     * Returns the time delay between when audio is written and when it plays.
     * Uses IAudioClock for accurate measurement, falls back to estimate.
     */
    double GetBufferLatencySeconds() const;

    /**
     * Get total samples written to WASAPI since start.
     * Used for position tracking.
     */
    uint64_t GetSamplesWritten() const { return samples_written_.load(); }

private:
    /**
     * Render thread function - runs the event-driven audio loop.
     * Waits for buffer events and calls RenderAudioBuffer() to fill data.
     */
    void RenderThread();

    /**
     * Fill the audio buffer by calling the user callback.
     * Called by RenderThread() when the device needs more data.
     */
    void RenderAudioBuffer();

    /**
     * Handle device loss (e.g., headphones unplugged).
     * Attempts to reinitialize with the new default device.
     *
     * @return true if recovery succeeded, false if playback should stop
     */
    bool HandleDeviceLoss();

    /**
     * Release all COM resources.
     * Called by Shutdown() and during device loss recovery.
     */
    void ReleaseResources();

    /**
     * Initialize COM interfaces for the default audio device.
     * @return true if successful
     */
    bool InitializeDevice();

    // Configuration
    WasapiDeviceConfig config_;
    bool initialized_ = false;

    // COM interfaces (stored as void* to avoid Windows headers in this header)
    // Cast to actual types in the .cpp file
    void* enumerator_ = nullptr;      // IMMDeviceEnumerator*
    void* device_ = nullptr;          // IMMDevice*
    void* audioClient_ = nullptr;     // IAudioClient3*
    void* renderClient_ = nullptr;    // IAudioRenderClient*

    // Event handles
    void* bufferEvent_ = nullptr;     // HANDLE - signaled when buffer needs data
    void* stopEvent_ = nullptr;       // HANDLE - signaled to stop render thread

    // Buffer info
    uint32_t bufferFrameCount_ = 0;   // Total buffer size in frames
    uint32_t frameSize_ = 0;          // Bytes per frame (channels * sizeof(float))

    // Render thread
    std::thread renderThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> threadStarted_{false};

    // Device recovery
    std::atomic<bool> deviceLost_{false};
    int recoveryAttempts_ = 0;
    static constexpr int MaxRecoveryAttempts = 5;

    // IAudioClock for latency measurement (Phase 0: Audio Sync)
    void* audioClock_ = nullptr;          // IAudioClock*
    uint64_t clock_frequency_ = 0;        // IAudioClock frequency
    std::atomic<uint64_t> samples_written_{0};  // Total samples written to WASAPI
};

} // namespace qcview
