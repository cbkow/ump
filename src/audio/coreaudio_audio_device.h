#pragma once

#ifdef __APPLE__

#include <atomic>
#include <cstdint>
#include <functional>

namespace qcview {

// Callback signature matching WASAPI/PipeWire for drop-in compatibility
using CoreAudioDataCallback = void(*)(void* device, float* output,
                                       uint32_t frameCount, void* userData);

struct CoreAudioDeviceConfig {
    CoreAudioDataCallback dataCallback = nullptr;
    void* userData = nullptr;
    int sampleRate = 48000;
    int channels = 2;
    int bufferSizeMs = 10;
};

//=============================================================================
// CoreAudioAudioDevice — Core Audio output for macOS
//
// Drop-in replacement for WasapiAudioDevice / PipeWireAudioDevice.
// Uses AudioUnit with kAudioUnitSubType_DefaultOutput.
//=============================================================================

class CoreAudioAudioDevice {
public:
    CoreAudioAudioDevice();
    ~CoreAudioAudioDevice();

    CoreAudioAudioDevice(const CoreAudioAudioDevice&) = delete;
    CoreAudioAudioDevice& operator=(const CoreAudioAudioDevice&) = delete;

    bool Initialize(const CoreAudioDeviceConfig& config);
    void Shutdown();

    void Start();
    void Stop();

    bool IsRunning() const { return running_.load(); }
    bool IsInitialized() const { return initialized_; }

    uint32_t GetBufferFrameCount() const { return buffer_frame_count_; }
    int GetSampleRate() const { return config_.sampleRate; }
    double GetBufferLatencySeconds() const;
    uint64_t GetSamplesWritten() const { return samples_written_.load(); }

private:
    // AudioUnit render callback (called from Core Audio thread)
    static int RenderCallback(void* inRefCon, unsigned int inActionFlags,
                               const void* inTimeStamp, unsigned int inBusNumber,
                               unsigned int inNumberFrames, void* ioData);

    CoreAudioDeviceConfig config_;
    bool initialized_ = false;

    void* audio_unit_ = nullptr;  // AudioComponentInstance (opaque)

    std::atomic<bool> running_{false};
    std::atomic<bool> should_play_{false};

    uint32_t buffer_frame_count_ = 0;
    std::atomic<uint64_t> samples_written_{0};
};

} // namespace qcview

#endif // __APPLE__
