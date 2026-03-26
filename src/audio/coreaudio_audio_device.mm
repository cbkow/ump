#ifdef __APPLE__

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include "coreaudio_audio_device.h"
#include "../utils/debug_utils.h"

#include <cstring>

namespace qcview {

CoreAudioAudioDevice::CoreAudioAudioDevice() = default;

CoreAudioAudioDevice::~CoreAudioAudioDevice() {
    Shutdown();
}

bool CoreAudioAudioDevice::Initialize(const CoreAudioDeviceConfig& config) {
    if (initialized_) return true;

    config_ = config;

    // Find default output audio unit
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
        Debug::Log("CoreAudioDevice: No default output component found");
        return false;
    }

    AudioComponentInstance unit = nullptr;
    OSStatus status = AudioComponentInstanceNew(component, &unit);
    if (status != noErr) {
        Debug::Log("CoreAudioDevice: Failed to create AudioUnit (error " +
                   std::to_string(status) + ")");
        return false;
    }
    audio_unit_ = unit;

    // Set output format: float32, stereo, interleaved
    AudioStreamBasicDescription format = {};
    format.mSampleRate = config_.sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = config_.channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * config_.channels;
    format.mBytesPerPacket = format.mBytesPerFrame;

    status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0,
                                  &format, sizeof(format));
    if (status != noErr) {
        Debug::Log("CoreAudioDevice: Failed to set stream format (error " +
                   std::to_string(status) + ")");
        AudioComponentInstanceDispose(unit);
        audio_unit_ = nullptr;
        return false;
    }

    // Set render callback
    AURenderCallbackStruct callback_struct = {};
    callback_struct.inputProc = (AURenderCallback)RenderCallback;
    callback_struct.inputProcRefCon = this;

    status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0,
                                  &callback_struct, sizeof(callback_struct));
    if (status != noErr) {
        Debug::Log("CoreAudioDevice: Failed to set render callback (error " +
                   std::to_string(status) + ")");
        AudioComponentInstanceDispose(unit);
        audio_unit_ = nullptr;
        return false;
    }

    // Initialize the audio unit
    status = AudioUnitInitialize(unit);
    if (status != noErr) {
        Debug::Log("CoreAudioDevice: Failed to initialize AudioUnit (error " +
                   std::to_string(status) + ")");
        AudioComponentInstanceDispose(unit);
        audio_unit_ = nullptr;
        return false;
    }

    // Get actual buffer size
    UInt32 buffer_size = 0;
    UInt32 prop_size = sizeof(buffer_size);
    AudioUnitGetProperty(unit, kAudioDevicePropertyBufferFrameSize,
                         kAudioUnitScope_Output, 0,
                         &buffer_size, &prop_size);
    buffer_frame_count_ = buffer_size > 0 ? buffer_size :
                          static_cast<uint32_t>(config_.sampleRate * config_.bufferSizeMs / 1000);

    initialized_ = true;
    Debug::Log("CoreAudioDevice: Initialized (sample rate: " +
               std::to_string(config_.sampleRate) + " Hz, buffer: " +
               std::to_string(buffer_frame_count_) + " frames)");
    return true;
}

void CoreAudioAudioDevice::Shutdown() {
    if (!initialized_) return;

    Stop();

    if (audio_unit_) {
        AudioComponentInstance unit = (AudioComponentInstance)audio_unit_;
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        audio_unit_ = nullptr;
    }

    initialized_ = false;
    Debug::Log("CoreAudioDevice: Shutdown");
}

void CoreAudioAudioDevice::Start() {
    if (!initialized_ || running_.load()) return;

    should_play_ = true;
    AudioComponentInstance unit = (AudioComponentInstance)audio_unit_;
    OSStatus status = AudioOutputUnitStart(unit);
    if (status == noErr) {
        running_ = true;
        Debug::Log("CoreAudioDevice: Started");
    } else {
        Debug::Log("CoreAudioDevice: Failed to start (error " + std::to_string(status) + ")");
    }
}

void CoreAudioAudioDevice::Stop() {
    if (!initialized_ || !running_.load()) return;

    should_play_ = false;
    AudioComponentInstance unit = (AudioComponentInstance)audio_unit_;
    AudioOutputUnitStop(unit);
    running_ = false;
    Debug::Log("CoreAudioDevice: Stopped");
}

double CoreAudioAudioDevice::GetBufferLatencySeconds() const {
    if (!initialized_ || config_.sampleRate <= 0) return 0.0;

    // Get device latency
    AudioComponentInstance unit = (AudioComponentInstance)audio_unit_;
    Float64 latency = 0.0;
    UInt32 size = sizeof(latency);
    AudioUnitGetProperty(unit, kAudioUnitProperty_Latency,
                         kAudioUnitScope_Output, 0, &latency, &size);

    // Add buffer latency
    return latency + static_cast<double>(buffer_frame_count_) / config_.sampleRate;
}

int CoreAudioAudioDevice::RenderCallback(void* inRefCon, unsigned int inActionFlags,
                                          const void* inTimeStamp, unsigned int inBusNumber,
                                          unsigned int inNumberFrames, void* ioData) {
    auto* device = static_cast<CoreAudioAudioDevice*>(inRefCon);
    AudioBufferList* bufferList = (AudioBufferList*)ioData;

    if (!device->should_play_.load() || !device->config_.dataCallback) {
        // Output silence
        for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
            memset(bufferList->mBuffers[i].mData, 0, bufferList->mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    // Get output buffer
    float* output = (float*)bufferList->mBuffers[0].mData;

    // Call the data callback (same signature as WASAPI/PipeWire)
    device->config_.dataCallback(device, output, inNumberFrames, device->config_.userData);

    device->samples_written_ += inNumberFrames;

    return noErr;
}

} // namespace qcview

#endif // __APPLE__
