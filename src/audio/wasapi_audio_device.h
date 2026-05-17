// WasapiAudioDevice — Phase F.2.10, Windows audio output.
//
// Drop-in parallel to CoreAudioDevice. Same public surface, same
// callback contract (`AudioDataCallback` = void(*)(void *device,
// float *output, uint32_t frameCount, void *userData)). AudioPlayer
// uses CoreAudioDevice on macOS and WasapiAudioDevice on Windows
// behind the same wiring.
//
// Backend: WASAPI shared-mode renderer (so other apps can play
// audio alongside). Event-driven — the OS signals our handle when
// it wants more samples; a dedicated render thread waits on the
// event and pumps the user callback into the IAudioRenderClient
// buffer. MMCSS "Pro Audio" priority on the render thread for
// low-latency scheduling.
//
// Format: requests 48 kHz float32 stereo and uses
// AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM so WASAPI auto-converts
// between our format and the system mixer's mix format. No manual
// resampling needed in the device — the audio decoder still runs
// SwrContext to deliver float32 stereo at this rate; the mixer
// handles the system-side rate match.

#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <atomic>
#include <cstdint>
#include <thread>

namespace qcv {

// Same signature as CoreAudioDataCallback so AudioPlayer's static
// dataCallback bridges to both backends without #ifdef in the
// callback body.
using WasapiDataCallback = void(*)(void *device, float *output,
                                    uint32_t frameCount, void *userData);

struct WasapiAudioDeviceConfig {
    WasapiDataCallback dataCallback = nullptr;
    void              *userData     = nullptr;
    int                sampleRate   = 48000;
    int                channels     = 2;
    int                bufferSizeMs = 10;
};

class WasapiAudioDevice
{
public:
    WasapiAudioDevice();
    ~WasapiAudioDevice();

    WasapiAudioDevice(const WasapiAudioDevice &)            = delete;
    WasapiAudioDevice &operator=(const WasapiAudioDevice &) = delete;

    bool initialize(const WasapiAudioDeviceConfig &config);
    void shutdown();

    void start();
    void stop();

    bool isInitialized() const { return m_initialized; }
    bool isRunning()     const { return m_running.load(); }

    uint32_t bufferFrameCount() const { return m_bufferFrameCount; }
    int      sampleRate()       const { return m_config.sampleRate; }

    // Buffer + device latency in seconds.
    double   bufferLatencySeconds() const;

    // Total frames handed to the OS since start. Used by AudioPlayer
    // for drift-correction sync against the video clock.
    uint64_t samplesWritten() const { return m_samplesWritten.load(); }

private:
    void renderThreadProc();

    struct Impl;
    Impl                 *m_impl = nullptr;   // hides COM types from header

    WasapiAudioDeviceConfig m_config;
    bool                    m_initialized      = false;
    uint32_t                m_bufferFrameCount = 0;
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_stopRequested{false};
    std::atomic<uint64_t>   m_samplesWritten{0};
    std::thread             m_renderThread;
};

} // namespace qcv

#endif // Q_OS_WIN
