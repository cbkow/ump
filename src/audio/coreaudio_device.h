// CoreAudioDevice — Phase 5.0.a, macOS audio output.
//
// Thin wrapper around AudioUnit kAudioUnitSubType_DefaultOutput. The
// render callback runs on a Core Audio thread; we just hand off to a
// user-supplied data callback that fills the output buffer with
// float32 stereo PCM.
//
// Phase 5.x adds Windows (WASAPI) and Linux (PipeWire) backends with
// the same callback contract.

#pragma once

#ifdef Q_OS_MACOS
#define QCV_HAS_COREAUDIO 1
#else
#define QCV_HAS_COREAUDIO 0
#endif

#include <QtGlobal>

#if defined(Q_OS_MACOS) || defined(__APPLE__)

#include <atomic>
#include <cstdint>

namespace qcv {

// Drop-in compatible with the old app's signature so a future
// WasapiAudioDevice / PipeWireAudioDevice port can share the same
// caller wiring.
using CoreAudioDataCallback = void(*)(void *device, float *output,
                                       uint32_t frameCount, void *userData);

struct CoreAudioDeviceConfig {
    CoreAudioDataCallback dataCallback = nullptr;
    void                 *userData     = nullptr;
    int                   sampleRate   = 48000;
    int                   channels     = 2;
    int                   bufferSizeMs = 10;
};

class CoreAudioDevice
{
public:
    CoreAudioDevice();
    ~CoreAudioDevice();

    CoreAudioDevice(const CoreAudioDevice &) = delete;
    CoreAudioDevice &operator=(const CoreAudioDevice &) = delete;

    bool initialize(const CoreAudioDeviceConfig &config);
    void shutdown();

    void start();
    void stop();

    bool isInitialized() const { return m_initialized; }
    bool isRunning()     const { return m_running.load(); }

    uint32_t bufferFrameCount() const { return m_bufferFrameCount; }
    int      sampleRate()       const { return m_config.sampleRate; }

    // Buffer + device latency in seconds (CoreAudio reports the
    // hardware path; we add the buffer-frame contribution).
    double   bufferLatencySeconds() const;

    // Total samples handed to the OS since start. Used by Phase
    // 5.0.b for WASAPI-style audio-clock-driven sync.
    uint64_t samplesWritten() const { return m_samplesWritten.load(); }

private:
    static int renderCallback(void *inRefCon, unsigned int actionFlags,
                               const void *timeStamp, unsigned int busNumber,
                               unsigned int numberFrames, void *ioData);

    CoreAudioDeviceConfig m_config;
    bool                  m_initialized = false;
    void                 *m_audioUnit   = nullptr;     // AudioComponentInstance
    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_shouldPlay{false};
    uint32_t              m_bufferFrameCount = 0;
    std::atomic<uint64_t> m_samplesWritten{0};
};

} // namespace qcv

#endif // __APPLE__
