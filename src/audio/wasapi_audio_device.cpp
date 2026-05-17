#include "wasapi_audio_device.h"

#if defined(Q_OS_WIN)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <avrt.h>
#include <combaseapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <QtLogging>

#include <cstring>

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Convert a millisecond span to WASAPI's 100-ns REFERENCE_TIME unit.
constexpr REFERENCE_TIME msToRefTime(int ms)
{
    return static_cast<REFERENCE_TIME>(ms) * 10000;
}

// PCM IEEE float, interleaved, stereo @ caller's rate. Paired with
// AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM so WASAPI handles whatever the
// system mixer is doing (rate conversion, channel-map, etc.).
WAVEFORMATEX buildWaveFormat(int sampleRate, int channels)
{
    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = static_cast<WORD>(channels);
    fmt.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = static_cast<WORD>(channels * sizeof(float));
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;
    return fmt;
}

} // namespace

struct WasapiAudioDevice::Impl {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient>        audioClient;
    ComPtr<IAudioRenderClient>  renderClient;
    HANDLE                      eventHandle = nullptr;
    bool                        comInitialized = false;
};

WasapiAudioDevice::WasapiAudioDevice()
    : m_impl(new Impl()) {}

WasapiAudioDevice::~WasapiAudioDevice()
{
    shutdown();
    delete m_impl;
}

bool WasapiAudioDevice::initialize(const WasapiAudioDeviceConfig &config)
{
    if (m_initialized) return true;
    m_config = config;

    // COM init for this thread. Match with CoUninitialize in shutdown.
    // CoInitializeEx is per-thread and reference-counted, so it nests
    // safely if the caller (GUI thread) already initialized COM.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        m_impl->comInitialized = (hr == S_OK);   // S_FALSE = already init'd
    } else if (hr != RPC_E_CHANGED_MODE) {
        qWarning("WasapiAudioDevice: CoInitializeEx failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }
    // RPC_E_CHANGED_MODE means the thread already chose a different
    // apartment (typically STA on the GUI thread). WASAPI works on
    // STA threads too; we just don't claim ownership of the apartment.

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                            reinterpret_cast<void **>(m_impl->enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: CoCreateInstance(MMDeviceEnumerator) failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    hr = m_impl->enumerator->GetDefaultAudioEndpoint(
            eRender, eConsole, m_impl->device.GetAddressOf());
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: GetDefaultAudioEndpoint failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    hr = m_impl->device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void **>(m_impl->audioClient.GetAddressOf()));
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: IMMDevice::Activate(IAudioClient) failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    const WAVEFORMATEX fmt = buildWaveFormat(m_config.sampleRate, m_config.channels);

    // Event-driven shared mode + auto-convert. AUTOCONVERTPCM is
    // valid in shared mode (Vista SP1+) and lets WASAPI handle the
    // system-mixer format negotiation — we always feed float32 stereo
    // at the configured rate.
    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = m_impl->audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            msToRefTime(m_config.bufferSizeMs),
            0,
            &fmt,
            nullptr);
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: IAudioClient::Initialize failed hr=0x%08lX "
                 "(rate=%d ch=%d bufMs=%d)", static_cast<unsigned long>(hr),
                 m_config.sampleRate, m_config.channels, m_config.bufferSizeMs);
        shutdown();
        return false;
    }

    m_impl->eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_impl->eventHandle) {
        qWarning("WasapiAudioDevice: CreateEvent failed (GetLastError=%lu)",
                 GetLastError());
        shutdown();
        return false;
    }
    hr = m_impl->audioClient->SetEventHandle(m_impl->eventHandle);
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: SetEventHandle failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    UINT32 bufferFrames = 0;
    hr = m_impl->audioClient->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: GetBufferSize failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }
    m_bufferFrameCount = bufferFrames;

    hr = m_impl->audioClient->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void **>(m_impl->renderClient.GetAddressOf()));
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: GetService(IAudioRenderClient) failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    m_initialized = true;
    qInfo("WasapiAudioDevice: initialized (%d Hz, %d ch, buffer=%u frames, %d ms target)",
          m_config.sampleRate, m_config.channels, m_bufferFrameCount,
          m_config.bufferSizeMs);
    return true;
}

void WasapiAudioDevice::shutdown()
{
    if (m_initialized || m_running.load()) {
        stop();
    }
    if (m_impl) {
        m_impl->renderClient.Reset();
        m_impl->audioClient.Reset();
        m_impl->device.Reset();
        m_impl->enumerator.Reset();
        if (m_impl->eventHandle) {
            CloseHandle(m_impl->eventHandle);
            m_impl->eventHandle = nullptr;
        }
        if (m_impl->comInitialized) {
            CoUninitialize();
            m_impl->comInitialized = false;
        }
    }
    m_bufferFrameCount = 0;
    m_initialized = false;
}

void WasapiAudioDevice::start()
{
    if (!m_initialized || m_running.load()) return;
    m_stopRequested.store(false);
    m_samplesWritten.store(0);

    // Pre-fill the buffer with silence so the first event-wake has
    // valid data to play and we don't glitch on the first frame.
    BYTE *silence = nullptr;
    HRESULT hr = m_impl->renderClient->GetBuffer(m_bufferFrameCount, &silence);
    if (SUCCEEDED(hr) && silence) {
        std::memset(silence,
                    0,
                    static_cast<size_t>(m_bufferFrameCount)
                    * m_config.channels * sizeof(float));
        m_impl->renderClient->ReleaseBuffer(m_bufferFrameCount,
                                              AUDCLNT_BUFFERFLAGS_SILENT);
    }

    hr = m_impl->audioClient->Start();
    if (FAILED(hr)) {
        qWarning("WasapiAudioDevice: IAudioClient::Start failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        return;
    }

    m_running.store(true);
    m_renderThread = std::thread([this]{ renderThreadProc(); });
}

void WasapiAudioDevice::stop()
{
    if (!m_running.exchange(false)) return;
    m_stopRequested.store(true);
    // Nudge the render thread out of WaitForSingleObject so it
    // observes m_stopRequested and exits cleanly.
    if (m_impl && m_impl->eventHandle) {
        SetEvent(m_impl->eventHandle);
    }
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }
    if (m_impl && m_impl->audioClient) {
        m_impl->audioClient->Stop();
        m_impl->audioClient->Reset();   // discard pending samples for clean restart
    }
}

double WasapiAudioDevice::bufferLatencySeconds() const
{
    if (!m_initialized || m_config.sampleRate <= 0) return 0.0;
    REFERENCE_TIME streamLatency = 0;
    if (m_impl && m_impl->audioClient) {
        m_impl->audioClient->GetStreamLatency(&streamLatency);
    }
    return static_cast<double>(streamLatency) / 10000000.0
         + static_cast<double>(m_bufferFrameCount) / m_config.sampleRate;
}

void WasapiAudioDevice::renderThreadProc()
{
    // MMCSS "Pro Audio" boosts this thread's scheduling priority so
    // the OS doesn't preempt us during the audio callback. Fall back
    // to time-critical priority if the profile isn't available.
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(
        L"Pro Audio", &taskIndex);
    if (!mmcssHandle) {
        ::SetThreadPriority(::GetCurrentThread(),
                            THREAD_PRIORITY_TIME_CRITICAL);
    }

    // Each thread has its own COM apartment; init multithreaded here
    // so interface calls work even if the GUI thread is STA.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwnedHere = (comHr == S_OK);

    while (!m_stopRequested.load()) {
        const DWORD waitResult = WaitForSingleObject(
            m_impl->eventHandle, 2000);   // 2 s safety timeout

        if (m_stopRequested.load()) break;
        if (waitResult != WAIT_OBJECT_0) {
            // Timed out — keep looping; underrun is preferable to
            // stale audio if the device is paused externally.
            continue;
        }

        // Calculate how many frames are free in the WASAPI buffer.
        UINT32 padding = 0;
        if (FAILED(m_impl->audioClient->GetCurrentPadding(&padding))) {
            continue;
        }
        const UINT32 framesAvailable =
            (padding < m_bufferFrameCount) ? m_bufferFrameCount - padding : 0;
        if (framesAvailable == 0) continue;

        BYTE *buffer = nullptr;
        if (FAILED(m_impl->renderClient->GetBuffer(framesAvailable, &buffer))
            || !buffer) {
            continue;
        }

        if (m_config.dataCallback) {
            m_config.dataCallback(this,
                                   reinterpret_cast<float *>(buffer),
                                   framesAvailable,
                                   m_config.userData);
            m_impl->renderClient->ReleaseBuffer(framesAvailable, 0);
        } else {
            // No callback wired — emit silence and let the event loop
            // keep ticking so stop() can drain promptly.
            std::memset(buffer, 0,
                        static_cast<size_t>(framesAvailable)
                        * m_config.channels * sizeof(float));
            m_impl->renderClient->ReleaseBuffer(framesAvailable,
                                                  AUDCLNT_BUFFERFLAGS_SILENT);
        }

        m_samplesWritten.fetch_add(framesAvailable,
                                    std::memory_order_relaxed);
    }

    if (comOwnedHere) CoUninitialize();
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}

} // namespace qcv

#endif // Q_OS_WIN
