#include "pipewire_audio_device.h"

#ifdef __linux__

#include "../utils/debug_utils.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <cstring>
#include <algorithm>

namespace qcview {

//=============================================================================
// PipeWire stream events
//=============================================================================

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = PipeWireAudioDevice::OnProcess,
};

//=============================================================================
// Constructor / Destructor
//=============================================================================

PipeWireAudioDevice::PipeWireAudioDevice() {
}

PipeWireAudioDevice::~PipeWireAudioDevice() {
    Shutdown();
}

//=============================================================================
// Initialize
//=============================================================================

bool PipeWireAudioDevice::Initialize(const PipeWireDeviceConfig& config) {
    if (initialized_) return true;

    config_ = config;

    if (!config_.dataCallback) {
        Debug::Log("PipeWireAudioDevice: No data callback provided");
        return false;
    }

    // Initialize PipeWire library (safe to call multiple times)
    pw_init(nullptr, nullptr);

    stride_ = config_.channels * sizeof(float);

    // Calculate buffer size in frames from ms
    buffer_frame_count_ = static_cast<uint32_t>(
        config_.sampleRate * config_.bufferSizeMs / 1000);
    if (buffer_frame_count_ < 64) buffer_frame_count_ = 64;

    // Create main loop
    pw_main_loop_ = pw_main_loop_new(nullptr);
    if (!pw_main_loop_) {
        Debug::Log("PipeWireAudioDevice: Failed to create main loop");
        return false;
    }

    auto* loop = pw_main_loop_get_loop(static_cast<struct pw_main_loop*>(pw_main_loop_));

    // Create stream properties
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Movie",
        PW_KEY_APP_NAME, "QCView Player",
        PW_KEY_NODE_NAME, "qcview-playback",
        PW_KEY_NODE_LATENCY, std::to_string(buffer_frame_count_).append("/")
            .append(std::to_string(config_.sampleRate)).c_str(),
        nullptr
    );

    // Create stream
    pw_stream_ = pw_stream_new_simple(
        loop,
        "QCView Audio",
        props,
        &stream_events,
        this  // userdata
    );

    if (!pw_stream_) {
        Debug::Log("PipeWireAudioDevice: Failed to create stream");
        pw_main_loop_destroy(static_cast<struct pw_main_loop*>(pw_main_loop_));
        pw_main_loop_ = nullptr;
        return false;
    }

    // Build audio format params
    uint8_t params_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

    struct spa_audio_info_raw audio_info = {};
    audio_info.format = SPA_AUDIO_FORMAT_F32;
    audio_info.rate = static_cast<uint32_t>(config_.sampleRate);
    audio_info.channels = static_cast<uint32_t>(config_.channels);
    audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
    audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;

    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

    // Connect stream (but don't start playback yet)
    int ret = pw_stream_connect(
        static_cast<struct pw_stream*>(pw_stream_),
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        static_cast<enum pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS),
        params, 1
    );

    if (ret < 0) {
        Debug::Log("PipeWireAudioDevice: Failed to connect stream: " + std::string(strerror(-ret)));
        pw_stream_destroy(static_cast<struct pw_stream*>(pw_stream_));
        pw_stream_ = nullptr;
        pw_main_loop_destroy(static_cast<struct pw_main_loop*>(pw_main_loop_));
        pw_main_loop_ = nullptr;
        return false;
    }

    // Start the PipeWire thread (runs the main loop)
    thread_started_ = false;
    pw_thread_ = std::thread(&PipeWireAudioDevice::PipeWireThread, this);

    // Wait for thread to start
    int wait_count = 0;
    while (!thread_started_.load() && wait_count < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        wait_count++;
    }

    initialized_ = true;
    Debug::Log("PipeWireAudioDevice: Initialized (rate=" + std::to_string(config_.sampleRate) +
               ", channels=" + std::to_string(config_.channels) +
               ", buffer=" + std::to_string(buffer_frame_count_) + " frames)");

    return true;
}

//=============================================================================
// Shutdown
//=============================================================================

void PipeWireAudioDevice::Shutdown() {
    if (!initialized_) return;

    Debug::Log("PipeWireAudioDevice: Shutting down...");

    running_ = false;
    should_play_ = false;

    // Quit the main loop to unblock the thread
    if (pw_main_loop_) {
        pw_main_loop_quit(static_cast<struct pw_main_loop*>(pw_main_loop_));
    }

    if (pw_thread_.joinable()) {
        pw_thread_.join();
    }

    if (pw_stream_) {
        pw_stream_destroy(static_cast<struct pw_stream*>(pw_stream_));
        pw_stream_ = nullptr;
    }

    if (pw_main_loop_) {
        pw_main_loop_destroy(static_cast<struct pw_main_loop*>(pw_main_loop_));
        pw_main_loop_ = nullptr;
    }

    initialized_ = false;
    Debug::Log("PipeWireAudioDevice: Shutdown complete");
}

//=============================================================================
// Start / Stop
//=============================================================================

void PipeWireAudioDevice::Start() {
    if (!initialized_) return;

    should_play_ = true;
    running_ = true;
    samples_written_ = 0;

    Debug::Log("PipeWireAudioDevice: Started playback");
}

void PipeWireAudioDevice::Stop() {
    if (!initialized_) return;

    should_play_ = false;
    running_ = false;

    Debug::Log("PipeWireAudioDevice: Stopped playback");
}

//=============================================================================
// GetBufferLatencySeconds
//=============================================================================

double PipeWireAudioDevice::GetBufferLatencySeconds() const {
    if (config_.sampleRate <= 0) return 0.0;
    return static_cast<double>(buffer_frame_count_) / config_.sampleRate;
}

//=============================================================================
// PipeWire Thread
//=============================================================================

void PipeWireAudioDevice::PipeWireThread() {
    Debug::Log("PipeWireAudioDevice: Thread started");
    thread_started_ = true;

    // Run the main loop — blocks until pw_main_loop_quit() is called
    pw_main_loop_run(static_cast<struct pw_main_loop*>(pw_main_loop_));

    Debug::Log("PipeWireAudioDevice: Thread exiting");
}

//=============================================================================
// OnProcess — PipeWire stream callback (real-time thread)
//=============================================================================

void PipeWireAudioDevice::OnProcess(void* userdata) {
    PipeWireAudioDevice* self = static_cast<PipeWireAudioDevice*>(userdata);
    if (!self || !self->pw_stream_) return;

    auto* stream = static_cast<struct pw_stream*>(self->pw_stream_);

    // Dequeue a buffer from PipeWire
    struct pw_buffer* pw_buf = pw_stream_dequeue_buffer(stream);
    if (!pw_buf) return;

    struct spa_buffer* spa_buf = pw_buf->buffer;
    struct spa_data* d = &spa_buf->datas[0];

    if (!d->data) {
        pw_stream_queue_buffer(stream, pw_buf);
        return;
    }

    // Calculate how many frames we can write
    uint32_t stride = self->config_.channels * sizeof(float);
    uint32_t max_frames = d->maxsize / stride;

    // Use the requested size if available, otherwise fill the buffer
    uint32_t n_frames = pw_buf->requested
        ? std::min(static_cast<uint32_t>(pw_buf->requested), max_frames)
        : max_frames;

    float* output = static_cast<float*>(d->data);

    if (self->should_play_.load() && self->config_.dataCallback) {
        // Call the user callback to fill the buffer
        self->config_.dataCallback(self, output, n_frames, self->config_.userData);
        self->samples_written_ += n_frames;
    } else {
        // Not playing — output silence
        std::memset(output, 0, n_frames * stride);
    }

    // Set buffer metadata
    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size = n_frames * stride;

    // Queue buffer back to PipeWire
    pw_stream_queue_buffer(stream, pw_buf);
}

} // namespace qcview

#endif // __linux__
