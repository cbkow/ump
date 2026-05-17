#include "coreaudio_device.h"

#if defined(Q_OS_MACOS) || defined(__APPLE__)

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <QtLogging>
#include <cstring>

namespace qcv {

CoreAudioDevice::CoreAudioDevice() = default;

CoreAudioDevice::~CoreAudioDevice() { shutdown(); }

bool CoreAudioDevice::initialize(const CoreAudioDeviceConfig &config)
{
    if (m_initialized) return true;
    m_config = config;

    AudioComponentDescription desc{};
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
        qWarning("CoreAudioDevice: no default output AudioComponent found");
        return false;
    }

    AudioComponentInstance unit = nullptr;
    OSStatus status = AudioComponentInstanceNew(component, &unit);
    if (status != noErr) {
        qWarning("CoreAudioDevice: AudioComponentInstanceNew failed (%d)",
                 static_cast<int>(status));
        return false;
    }
    m_audioUnit = unit;

    // Output format: float32, interleaved, stereo, target sample rate.
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = m_config.sampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 32;
    fmt.mChannelsPerFrame = m_config.channels;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = sizeof(float) * m_config.channels;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;

    status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (status != noErr) {
        qWarning("CoreAudioDevice: setStreamFormat failed (%d)",
                 static_cast<int>(status));
        AudioComponentInstanceDispose(unit);
        m_audioUnit = nullptr;
        return false;
    }

    AURenderCallbackStruct cb{};
    cb.inputProc       = (AURenderCallback)renderCallback;
    cb.inputProcRefCon = this;
    status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (status != noErr) {
        qWarning("CoreAudioDevice: setRenderCallback failed (%d)",
                 static_cast<int>(status));
        AudioComponentInstanceDispose(unit);
        m_audioUnit = nullptr;
        return false;
    }

    status = AudioUnitInitialize(unit);
    if (status != noErr) {
        qWarning("CoreAudioDevice: AudioUnitInitialize failed (%d)",
                 static_cast<int>(status));
        AudioComponentInstanceDispose(unit);
        m_audioUnit = nullptr;
        return false;
    }

    UInt32 bufSize = 0, propSize = sizeof(bufSize);
    AudioUnitGetProperty(unit, kAudioDevicePropertyBufferFrameSize,
                         kAudioUnitScope_Output, 0, &bufSize, &propSize);
    m_bufferFrameCount = bufSize > 0
        ? bufSize
        : static_cast<uint32_t>(m_config.sampleRate * m_config.bufferSizeMs / 1000);

    m_initialized = true;
    qInfo("CoreAudioDevice: initialized (%d Hz, %d ch, buffer=%u frames)",
          m_config.sampleRate, m_config.channels, m_bufferFrameCount);
    return true;
}

void CoreAudioDevice::shutdown()
{
    if (!m_initialized) return;
    stop();
    if (m_audioUnit) {
        auto unit = (AudioComponentInstance)m_audioUnit;
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        m_audioUnit = nullptr;
    }
    m_initialized = false;
}

void CoreAudioDevice::start()
{
    if (!m_initialized || m_running.load()) return;
    m_shouldPlay = true;
    auto unit = (AudioComponentInstance)m_audioUnit;
    OSStatus status = AudioOutputUnitStart(unit);
    if (status == noErr) {
        m_running = true;
    } else {
        qWarning("CoreAudioDevice: AudioOutputUnitStart failed (%d)",
                 static_cast<int>(status));
    }
}

void CoreAudioDevice::stop()
{
    if (!m_initialized || !m_running.load()) return;
    m_shouldPlay = false;
    auto unit = (AudioComponentInstance)m_audioUnit;
    AudioOutputUnitStop(unit);
    m_running = false;
}

double CoreAudioDevice::bufferLatencySeconds() const
{
    if (!m_initialized || m_config.sampleRate <= 0) return 0.0;
    auto unit = (AudioComponentInstance)m_audioUnit;
    Float64 latency = 0.0;
    UInt32 size = sizeof(latency);
    AudioUnitGetProperty(unit, kAudioUnitProperty_Latency,
                         kAudioUnitScope_Output, 0, &latency, &size);
    return latency
         + static_cast<double>(m_bufferFrameCount) / m_config.sampleRate;
}

int CoreAudioDevice::renderCallback(void *inRefCon, unsigned int /*flags*/,
                                     const void * /*timeStamp*/,
                                     unsigned int /*busNumber*/,
                                     unsigned int numberFrames, void *ioData)
{
    auto *self = static_cast<CoreAudioDevice *>(inRefCon);
    auto *bufList = static_cast<AudioBufferList *>(ioData);

    if (!self->m_shouldPlay.load() || !self->m_config.dataCallback) {
        for (UInt32 i = 0; i < bufList->mNumberBuffers; ++i) {
            std::memset(bufList->mBuffers[i].mData, 0,
                        bufList->mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    float *output = static_cast<float *>(bufList->mBuffers[0].mData);
    self->m_config.dataCallback(self, output, numberFrames,
                                self->m_config.userData);
    self->m_samplesWritten.fetch_add(numberFrames, std::memory_order_relaxed);
    return noErr;
}

} // namespace qcv

#endif // __APPLE__
