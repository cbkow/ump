#include "wasapi_audio_device.h"
#include "../utils/debug_utils.h"

// Windows headers for WASAPI
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <Ks.h>
#include <Ksmedia.h>

#include <cstring>
#include <algorithm>

// WASAPI GUIDs - these need to be defined as they're not in a library
static const GUID WASAPI_CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID WASAPI_IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID WASAPI_IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4c32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID WASAPI_IID_IAudioClient3 = { 0x7ED4EE07, 0x8E67, 0x4CD4, { 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42 } };
static const GUID WASAPI_IID_IAudioRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID WASAPI_IID_IAudioClock = { 0xCD63314F, 0x3FBA, 0x4a1b, { 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7 } };

namespace qcview {

//=============================================================================
// Helper macros for COM
//=============================================================================

#define SAFE_RELEASE(p) if (p) { ((IUnknown*)p)->Release(); p = nullptr; }

//=============================================================================
// Constructor / Destructor
//=============================================================================

WasapiAudioDevice::WasapiAudioDevice() = default;

WasapiAudioDevice::~WasapiAudioDevice() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool WasapiAudioDevice::Initialize(const WasapiDeviceConfig& config) {
    if (initialized_) {
        Debug::Log("WasapiAudioDevice: Already initialized");
        return true;
    }

    Debug::Log("WasapiAudioDevice: Initializing...");

    config_ = config;
    frameSize_ = config_.channels * sizeof(float);

    // Initialize COM for this thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Debug::Log("WasapiAudioDevice: Failed to initialize COM, hr=" + std::to_string(hr));
        return false;
    }

    // Create stop event
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        Debug::Log("WasapiAudioDevice: Failed to create stop event");
        return false;
    }

    // Create buffer event
    bufferEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!bufferEvent_) {
        Debug::Log("WasapiAudioDevice: Failed to create buffer event");
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return false;
    }

    // Initialize the audio device
    if (!InitializeDevice()) {
        CloseHandle(stopEvent_);
        CloseHandle(bufferEvent_);
        stopEvent_ = nullptr;
        bufferEvent_ = nullptr;
        return false;
    }

    initialized_ = true;
    Debug::Log("WasapiAudioDevice: Initialized successfully, buffer=" +
               std::to_string(bufferFrameCount_) + " frames");
    return true;
}

bool WasapiAudioDevice::InitializeDevice() {
    HRESULT hr;

    // 1. Create device enumerator
    hr = CoCreateInstance(
        WASAPI_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
        WASAPI_IID_IMMDeviceEnumerator, &enumerator_
    );
    if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Failed to create device enumerator, hr=" + std::to_string(hr));
        return false;
    }

    // 2. Get default audio endpoint
    hr = static_cast<IMMDeviceEnumerator*>(enumerator_)->GetDefaultAudioEndpoint(
        eRender, eConsole, reinterpret_cast<IMMDevice**>(&device_)
    );
    if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Failed to get default audio endpoint, hr=" + std::to_string(hr));
        ReleaseResources();
        return false;
    }

    // Log device name
    IPropertyStore* props = nullptr;
    hr = static_cast<IMMDevice*>(device_)->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            char deviceName[256];
            WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, deviceName, sizeof(deviceName), nullptr, nullptr);
            Debug::Log("WasapiAudioDevice: Using device: " + std::string(deviceName));
        }
        PropVariantClear(&varName);
        props->Release();
    }

    // 3. Activate IAudioClient3 for low-latency mode
    hr = static_cast<IMMDevice*>(device_)->Activate(
        WASAPI_IID_IAudioClient3, CLSCTX_ALL, nullptr, &audioClient_
    );
    if (FAILED(hr)) {
        // Fall back to IAudioClient if IAudioClient3 not available
        Debug::Log("WasapiAudioDevice: IAudioClient3 not available, falling back to IAudioClient");
        hr = static_cast<IMMDevice*>(device_)->Activate(
            WASAPI_IID_IAudioClient, CLSCTX_ALL, nullptr, &audioClient_
        );
        if (FAILED(hr)) {
            Debug::Log("WasapiAudioDevice: Failed to activate audio client, hr=" + std::to_string(hr));
            ReleaseResources();
            return false;
        }
    }

    // 4. Configure format: Float32, stereo, 48kHz
    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = static_cast<WORD>(config_.channels);
    wfx.Format.nSamplesPerSec = static_cast<DWORD>(config_.sampleRate);
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = static_cast<WORD>(config_.channels * sizeof(float));
    wfx.Format.nAvgBytesPerSec = config_.sampleRate * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    // Check if format is supported
    WAVEFORMATEX* closestMatch = nullptr;
    hr = static_cast<IAudioClient*>(audioClient_)->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED, reinterpret_cast<WAVEFORMATEX*>(&wfx), &closestMatch
    );
    if (hr == S_FALSE && closestMatch) {
        Debug::Log("WasapiAudioDevice: Requested format not directly supported, using closest match");
        // Could adapt to closestMatch here if needed
        CoTaskMemFree(closestMatch);
    } else if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Format not supported, hr=" + std::to_string(hr));
        // Try without AUTOCONVERTPCM - let Windows handle conversion
    }

    // 5. Initialize in shared mode with event callback
    // Calculate buffer duration in 100-nanosecond units
    REFERENCE_TIME bufferDuration = static_cast<REFERENCE_TIME>(config_.bufferSizeMs) * 10000;

    hr = static_cast<IAudioClient*>(audioClient_)->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        bufferDuration,
        0,  // Periodicity (must be 0 for shared mode)
        reinterpret_cast<WAVEFORMATEX*>(&wfx),
        nullptr  // Audio session GUID
    );

    if (FAILED(hr)) {
        // Try without AUTOCONVERTPCM if it fails
        Debug::Log("WasapiAudioDevice: Init with AUTOCONVERTPCM failed, trying without, hr=" + std::to_string(hr));
        hr = static_cast<IAudioClient*>(audioClient_)->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            bufferDuration,
            0,
            reinterpret_cast<WAVEFORMATEX*>(&wfx),
            nullptr
        );
    }

    if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Failed to initialize audio client, hr=" + std::to_string(hr));
        ReleaseResources();
        return false;
    }

    // 6. Get the actual buffer size
    hr = static_cast<IAudioClient*>(audioClient_)->GetBufferSize(&bufferFrameCount_);
    if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Failed to get buffer size, hr=" + std::to_string(hr));
        ReleaseResources();
        return false;
    }

    // 7. Get render client
    hr = static_cast<IAudioClient*>(audioClient_)->GetService(
        WASAPI_IID_IAudioRenderClient, &renderClient_
    );
    if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Failed to get render client, hr=" + std::to_string(hr));
        ReleaseResources();
        return false;
    }

    // 8. Get IAudioClock for latency measurement (Phase 0: Audio Sync)
    hr = static_cast<IAudioClient*>(audioClient_)->GetService(
        WASAPI_IID_IAudioClock, &audioClock_
    );
    if (SUCCEEDED(hr) && audioClock_) {
        hr = static_cast<IAudioClock*>(audioClock_)->GetFrequency(&clock_frequency_);
        if (SUCCEEDED(hr)) {
            Debug::Log("WasapiAudioDevice: IAudioClock initialized, frequency=" + std::to_string(clock_frequency_));
        } else {
            Debug::Log("WasapiAudioDevice: Failed to get clock frequency, latency estimation will be used");
            SAFE_RELEASE(audioClock_);
        }
    } else {
        Debug::Log("WasapiAudioDevice: IAudioClock not available, latency estimation will be used");
    }

    // Reset samples written counter
    samples_written_ = 0;

    // 9. Set event handle
    hr = static_cast<IAudioClient*>(audioClient_)->SetEventHandle(static_cast<HANDLE>(bufferEvent_));
    if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Failed to set event handle, hr=" + std::to_string(hr));
        ReleaseResources();
        return false;
    }

    return true;
}

void WasapiAudioDevice::ReleaseResources() {
    SAFE_RELEASE(audioClock_);
    SAFE_RELEASE(renderClient_);
    SAFE_RELEASE(audioClient_);
    SAFE_RELEASE(device_);
    SAFE_RELEASE(enumerator_);
    clock_frequency_ = 0;
}

void WasapiAudioDevice::Shutdown() {
    if (!initialized_ && !stopEvent_) return;

    Debug::Log("WasapiAudioDevice: Shutting down...");

    // Stop playback
    Stop();

    // Release COM resources
    ReleaseResources();

    // Close event handles
    if (bufferEvent_) {
        CloseHandle(bufferEvent_);
        bufferEvent_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    initialized_ = false;
    Debug::Log("WasapiAudioDevice: Shutdown complete");
}

//=============================================================================
// Playback Control
//=============================================================================

void WasapiAudioDevice::Start() {
    if (!initialized_ || running_) return;

    Debug::Log("WasapiAudioDevice: Starting playback...");

    // Reset stop event
    ResetEvent(static_cast<HANDLE>(stopEvent_));
    deviceLost_ = false;
    recoveryAttempts_ = 0;

    // Mark as running before starting thread
    running_ = true;
    threadStarted_ = false;

    // Start render thread
    renderThread_ = std::thread(&WasapiAudioDevice::RenderThread, this);

    // Wait for thread to start (with timeout)
    int waitCount = 0;
    while (!threadStarted_ && waitCount < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waitCount++;
    }

    Debug::Log("WasapiAudioDevice: Playback started");
}

void WasapiAudioDevice::Stop() {
    if (!running_) return;

    Debug::Log("WasapiAudioDevice: Stopping playback...");

    // Signal stop
    running_ = false;
    SetEvent(static_cast<HANDLE>(stopEvent_));

    // Wait for render thread to finish
    if (renderThread_.joinable()) {
        renderThread_.join();
    }

    Debug::Log("WasapiAudioDevice: Playback stopped");
}

//=============================================================================
// Render Thread
//=============================================================================

void WasapiAudioDevice::RenderThread() {
    // Initialize COM for this thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Debug::Log("WasapiAudioDevice: Render thread failed to initialize COM");
        running_ = false;
        threadStarted_ = true;
        return;
    }

    // Set MMCSS thread priority for low-latency audio
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!mmcssHandle) {
        Debug::Log("WasapiAudioDevice: Warning - failed to set MMCSS priority");
    }

    // Pre-fill buffer with silence before starting
    BYTE* data = nullptr;
    hr = static_cast<IAudioRenderClient*>(renderClient_)->GetBuffer(bufferFrameCount_, &data);
    if (SUCCEEDED(hr)) {
        std::memset(data, 0, bufferFrameCount_ * frameSize_);
        static_cast<IAudioRenderClient*>(renderClient_)->ReleaseBuffer(bufferFrameCount_, 0);
    }

    // Start the audio client
    hr = static_cast<IAudioClient*>(audioClient_)->Start();
    if (FAILED(hr)) {
        Debug::Log("WasapiAudioDevice: Failed to start audio client, hr=" + std::to_string(hr));
        if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
        CoUninitialize();
        running_ = false;
        threadStarted_ = true;
        return;
    }

    threadStarted_ = true;

    // Event-driven render loop
    HANDLE waitArray[2] = { static_cast<HANDLE>(stopEvent_), static_cast<HANDLE>(bufferEvent_) };

    while (running_) {
        DWORD result = WaitForMultipleObjects(2, waitArray, FALSE, 2000);

        if (result == WAIT_OBJECT_0) {
            // Stop event signaled
            break;
        } else if (result == WAIT_OBJECT_0 + 1) {
            // Buffer event signaled - render audio
            if (deviceLost_) {
                if (!HandleDeviceLoss()) {
                    break;
                }
            } else {
                RenderAudioBuffer();
            }
        } else if (result == WAIT_TIMEOUT) {
            // Timeout - check if device is still valid
            Debug::Log("WasapiAudioDevice: Render loop timeout, checking device state");
            UINT32 padding;
            hr = static_cast<IAudioClient*>(audioClient_)->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                Debug::Log("WasapiAudioDevice: Device may be lost, hr=" + std::to_string(hr));
                deviceLost_ = true;
            }
        } else {
            // Wait failed
            Debug::Log("WasapiAudioDevice: WaitForMultipleObjects failed, error=" + std::to_string(GetLastError()));
            break;
        }
    }

    // Stop the audio client
    if (audioClient_) {
        static_cast<IAudioClient*>(audioClient_)->Stop();
    }

    // Revert MMCSS
    if (mmcssHandle) {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }

    CoUninitialize();
}

void WasapiAudioDevice::RenderAudioBuffer() {
    if (!renderClient_ || !audioClient_) return;

    HRESULT hr;

    // Get current padding (how much data is already in the buffer)
    UINT32 padding = 0;
    hr = static_cast<IAudioClient*>(audioClient_)->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            Debug::Log("WasapiAudioDevice: Device invalidated during GetCurrentPadding");
            deviceLost_ = true;
        }
        return;
    }

    // Calculate available space
    UINT32 available = bufferFrameCount_ - padding;
    if (available == 0) return;

    // Get buffer from render client
    BYTE* data = nullptr;
    hr = static_cast<IAudioRenderClient*>(renderClient_)->GetBuffer(available, &data);
    if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            Debug::Log("WasapiAudioDevice: Device invalidated during GetBuffer");
            deviceLost_ = true;
        }
        return;
    }

    // Call user callback to fill the buffer
    if (config_.dataCallback) {
        config_.dataCallback(this, reinterpret_cast<float*>(data), available, config_.userData);
    } else {
        // No callback - fill with silence
        std::memset(data, 0, available * frameSize_);
    }

    // Release the buffer
    hr = static_cast<IAudioRenderClient*>(renderClient_)->ReleaseBuffer(available, 0);
    if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            Debug::Log("WasapiAudioDevice: Device invalidated during ReleaseBuffer");
            deviceLost_ = true;
        }
    } else {
        // Track samples written for latency calculation (Phase 0: Audio Sync)
        samples_written_.fetch_add(available);
    }
}

//=============================================================================
// Device Loss Recovery
//=============================================================================

bool WasapiAudioDevice::HandleDeviceLoss() {
    if (recoveryAttempts_ >= MaxRecoveryAttempts) {
        Debug::Log("WasapiAudioDevice: Max recovery attempts reached, giving up");
        return false;
    }

    recoveryAttempts_++;
    Debug::Log("WasapiAudioDevice: Attempting device recovery (attempt " +
               std::to_string(recoveryAttempts_) + "/" + std::to_string(MaxRecoveryAttempts) + ")");

    // Exponential backoff
    int delayMs = 100 * (1 << (recoveryAttempts_ - 1));
    delayMs = std::min(delayMs, 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

    // Release current resources
    ReleaseResources();

    // Try to reinitialize
    if (InitializeDevice()) {
        // Pre-fill buffer
        BYTE* data = nullptr;
        HRESULT hr = static_cast<IAudioRenderClient*>(renderClient_)->GetBuffer(bufferFrameCount_, &data);
        if (SUCCEEDED(hr)) {
            std::memset(data, 0, bufferFrameCount_ * frameSize_);
            static_cast<IAudioRenderClient*>(renderClient_)->ReleaseBuffer(bufferFrameCount_, 0);
        }

        // Restart audio client
        hr = static_cast<IAudioClient*>(audioClient_)->Start();
        if (SUCCEEDED(hr)) {
            Debug::Log("WasapiAudioDevice: Device recovery successful");
            deviceLost_ = false;
            recoveryAttempts_ = 0;
            return true;
        }
    }

    Debug::Log("WasapiAudioDevice: Device recovery failed");
    return false;
}

//=============================================================================
// Latency Measurement (Phase 0: Audio Sync)
//=============================================================================

double WasapiAudioDevice::GetBufferLatencySeconds() const {
    // Fallback estimate based on configured buffer size
    double fallback_latency = config_.bufferSizeMs / 1000.0;

    if (!audioClock_ || clock_frequency_ == 0) {
        return fallback_latency;
    }

    // Query IAudioClock for current playback position
    UINT64 device_position = 0;
    UINT64 qpc_position = 0;
    HRESULT hr = static_cast<IAudioClock*>(audioClock_)->GetPosition(&device_position, &qpc_position);
    if (FAILED(hr)) {
        return fallback_latency;
    }

    // Convert device position to samples
    // device_position is in units of clock_frequency_ per second
    // So samples played = device_position * sample_rate / clock_frequency_
    uint64_t samples_played = (device_position * config_.sampleRate) / clock_frequency_;

    // Calculate buffered samples (written but not yet played)
    uint64_t total_written = samples_written_.load();
    if (samples_played > total_written) {
        // Shouldn't happen, but handle gracefully
        return fallback_latency;
    }

    int64_t buffered_samples = static_cast<int64_t>(total_written - samples_played);

    // Convert to seconds
    double latency = static_cast<double>(buffered_samples) / config_.sampleRate;

    // Clamp to reasonable range (0 to 100ms)
    return std::max(0.0, std::min(latency, 0.1));
}

} // namespace qcview
