#ifdef WITH_GSTREAMER

#include "gstreamer_audio_manager.h"
#include "../player/gstreamer_manager.h"
#include "../player/playback_timer.h"
#include "../timeline/timeline_view.h"
#include "../utils/debug_utils.h"

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/pbutils/pbutils.h>

#include <algorithm>
#include <cmath>

namespace ump {

//=============================================================================
// Construction / Destruction
//=============================================================================

GStreamerAudioManager::GStreamerAudioManager() {
}

GStreamerAudioManager::~GStreamerAudioManager() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool GStreamerAudioManager::Initialize() {
    if (initialized_) {
        return true;
    }

    Debug::Log("[GStreamerAudioManager] Initializing...");

    // TEMPORARILY DISABLED: Skip audio pipeline to isolate crash source
    // We need to determine if crash is from audio (WASAPI) or video (GStreamer)
    Debug::Log("[GStreamerAudioManager] Audio pipeline DISABLED for debugging");
    initialized_ = true;
    return true;

#if 0  // Disabled for debugging
    // Ensure GStreamer is initialized
    if (!GStreamerManager::Instance().IsInitialized()) {
        if (!GStreamerManager::Instance().Initialize()) {
            Debug::Log("[GStreamerAudioManager] GStreamer init failed: " +
                      GStreamerManager::Instance().GetInitError());
            return false;
        }
    }

    // Build the output pipeline (audiomixer -> volume -> audiosink)
    if (!BuildOutputPipeline()) {
        Debug::Log("[GStreamerAudioManager] Failed to build output pipeline");
        return false;
    }

    initialized_ = true;
    Debug::Log("[GStreamerAudioManager] Initialized successfully");
    return true;
#endif
}

void GStreamerAudioManager::Shutdown() {
    if (!initialized_) {
        return;
    }

    Debug::Log("[GStreamerAudioManager] Shutting down...");

    // Stop playback
    Stop();

    // Remove all sources
    {
        std::lock_guard<std::mutex> lock(sources_mutex_);
        sources_.clear();
        active_clips_.clear();
    }

    // Destroy pipeline
    DestroyPipeline();

    initialized_ = false;
    Debug::Log("[GStreamerAudioManager] Shutdown complete");
}

//=============================================================================
// Pipeline Building
//=============================================================================

bool GStreamerAudioManager::BuildOutputPipeline() {
    Debug::Log("[GStreamerAudioManager] Building output pipeline...");

    // Create main pipeline
    pipeline_ = gst_pipeline_new("audio-output-pipeline");
    if (!pipeline_) {
        Debug::Log("[GStreamerAudioManager] Failed to create pipeline");
        return false;
    }

    // Create audiomixer - this is where all sources will connect
    audiomixer_ = gst_element_factory_make("audiomixer", "mixer");
    if (!audiomixer_) {
        Debug::Log("[GStreamerAudioManager] Failed to create audiomixer");
        DestroyPipeline();
        return false;
    }

    // Create master volume control
    master_volume_element_ = gst_element_factory_make("volume", "master-volume");
    if (!master_volume_element_) {
        Debug::Log("[GStreamerAudioManager] Failed to create volume element");
        DestroyPipeline();
        return false;
    }

    // Create audio sink - autoaudiosink will select the best available
    audiosink_ = gst_element_factory_make("autoaudiosink", "audiosink");
    if (!audiosink_) {
        Debug::Log("[GStreamerAudioManager] Failed to create autoaudiosink, trying directsoundsink");
        audiosink_ = gst_element_factory_make("directsoundsink", "audiosink");
    }
    if (!audiosink_) {
        Debug::Log("[GStreamerAudioManager] Failed to create any audio sink");
        DestroyPipeline();
        return false;
    }

    // Add elements to pipeline
    gst_bin_add_many(GST_BIN(pipeline_), audiomixer_, master_volume_element_, audiosink_, nullptr);

    // Link: audiomixer -> volume -> audiosink
    if (!gst_element_link_many(audiomixer_, master_volume_element_, audiosink_, nullptr)) {
        Debug::Log("[GStreamerAudioManager] Failed to link output pipeline");
        DestroyPipeline();
        return false;
    }

    // Set initial volume
    g_object_set(master_volume_element_, "volume", (double)master_volume_.load(), nullptr);

    // Start pipeline in PAUSED state (ready to receive sources)
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        Debug::Log("[GStreamerAudioManager] Failed to set pipeline to PAUSED");
        DestroyPipeline();
        return false;
    }

    Debug::Log("[GStreamerAudioManager] Output pipeline built successfully");
    return true;
}

void GStreamerAudioManager::DestroyPipeline() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    audiomixer_ = nullptr;
    master_volume_element_ = nullptr;
    audiosink_ = nullptr;
}

//=============================================================================
// Playback Control
//=============================================================================

void GStreamerAudioManager::Play() {
    if (!initialized_ || !pipeline_) return;

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret != GST_STATE_CHANGE_FAILURE) {
        is_playing_ = true;
        Debug::Log("[GStreamerAudioManager] Playing");
    }
}

void GStreamerAudioManager::Pause() {
    if (!initialized_ || !pipeline_) return;

    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    is_playing_ = false;
    Debug::Log("[GStreamerAudioManager] Paused");
}

void GStreamerAudioManager::Stop() {
    if (!initialized_ || !pipeline_) return;

    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    is_playing_ = false;

    // Seek to beginning
    gst_element_seek_simple(pipeline_, GST_FORMAT_TIME,
                            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                            0);
    current_position_ = 0.0;
    Debug::Log("[GStreamerAudioManager] Stopped");
}

void GStreamerAudioManager::SetVolume(float volume) {
    master_volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (master_volume_element_) {
        g_object_set(master_volume_element_, "volume", (double)master_volume_.load(), nullptr);
    }
}

void GStreamerAudioManager::SetMuted(bool muted) {
    is_muted_ = muted;
    if (master_volume_element_) {
        g_object_set(master_volume_element_, "mute", muted ? TRUE : FALSE, nullptr);
    }
}

//=============================================================================
// Timeline Integration
//=============================================================================

void GStreamerAudioManager::SetFlattener(TimelineFlattener* flattener) {
    flattener_ = flattener;
}

void GStreamerAudioManager::SetTimer(PlaybackTimer* timer) {
    timer_ = timer;
}

void GStreamerAudioManager::PreloadClips(const std::vector<OTIOClip>& clips) {
    // GStreamer creates sources on-demand when clips become active.
    // Unlike FFmpeg-based AudioMixer, we don't need to pre-decode audio.
    // The audiomixer element handles dynamic source addition/removal.
    //
    // We skip HasAudio() checks here because:
    // 1. GstDiscoverer can be slow/problematic with some formats (MXF)
    // 2. GStreamer handles files without audio gracefully
    // 3. Audio presence is checked when sources are actually created

    Debug::Log("[GStreamerAudioManager] PreloadClips called with " +
               std::to_string(clips.size()) + " clips (on-demand loading)");
}

void GStreamerAudioManager::ClearClips() {
    std::lock_guard<std::mutex> lock(sources_mutex_);

    // Stop and destroy all sources
    for (auto& [handle, source] : sources_) {
        if (source && source->bin) {
            gst_element_set_state(source->bin, GST_STATE_NULL);
        }
    }
    sources_.clear();
    active_clips_.clear();

    Debug::Log("[GStreamerAudioManager] All clips cleared");
}

void GStreamerAudioManager::Update() {
    if (!initialized_) return;

    // Get current position from timer if available
    double timeline_time = current_position_.load();
    if (timer_) {
        // We could sync with timer here if needed
        // For now, position is set via SetTimelinePosition callback
    }

    // Update active clips based on current position
    UpdateActiveClips(timeline_time);
}

void GStreamerAudioManager::SetTimelinePosition(double position) {
    current_position_ = position;
}

void GStreamerAudioManager::UpdateActiveClips(double timeline_time) {
    if (!initialized_ || !flattener_) return;

    current_position_ = timeline_time;

    // TODO: Multi-track audio source management is temporarily disabled
    // to isolate crash. Video-only playback should work.
    // The crash may be in CreateSource() or GStreamer decodebin for MXF audio.
    return;

#if 0  // Disabled for debugging
    // Get all audible clips at current time from flattener
    auto audible_clips = flattener_->GetAllAudibleClipsAtTime(timeline_time);

    std::lock_guard<std::mutex> lock(sources_mutex_);

    // Build set of clip IDs that should be active
    std::map<std::string, const OTIOClip*> should_be_active;
    for (const auto* clip : audible_clips) {
        if (clip && !clip->is_gap && clip->is_linked) {
            should_be_active[clip->id] = clip;
        }
    }

    // Remove clips that are no longer active
    std::vector<std::string> to_remove;
    for (const auto& [clip_id, handle] : active_clips_) {
        if (should_be_active.find(clip_id) == should_be_active.end()) {
            to_remove.push_back(clip_id);
        }
    }
    for (const auto& clip_id : to_remove) {
        AudioSourceHandle handle = active_clips_[clip_id];
        DestroySource(handle);
        active_clips_.erase(clip_id);
    }

    // Add clips that should be active but aren't
    for (const auto& [clip_id, clip] : should_be_active) {
        if (active_clips_.find(clip_id) == active_clips_.end()) {
            // Create AudioClipInfo from OTIOClip
            AudioClipInfo info;
            info.clip_id = clip->id;
            info.source_path = clip->linked_path.empty() ? clip->file_path : clip->linked_path;
            info.timeline_start = clip->start_time;
            info.timeline_end = clip->start_time + clip->duration;
            info.source_in = clip->source_in;
            info.source_out = clip->source_out;
            info.volume = 1.0f;  // Could get from track volume
            info.muted = false;

            AudioSourceHandle handle = CreateSource(info);
            if (handle != 0) {
                active_clips_[clip_id] = handle;
            }
        }
    }
#endif
}

void GStreamerAudioManager::Seek(double timeline_time) {
    if (!initialized_ || !pipeline_) return;

    current_position_ = timeline_time;

    // Seek each active source to its correct position
    std::lock_guard<std::mutex> lock(sources_mutex_);

    for (auto& [handle, source] : sources_) {
        if (!source || !source->bin) continue;

        // Calculate source position from timeline position
        double source_time = timeline_time - source->timeline_start + source->source_in;

        // Clamp to valid range
        source_time = std::max(source->source_in, std::min(source_time, source->source_out));

        // Convert to nanoseconds
        gint64 position_ns = static_cast<gint64>(source_time * GST_SECOND);

        // Seek the source bin
        gst_element_seek_simple(source->bin, GST_FORMAT_TIME,
                                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                                position_ns);
    }
}

double GStreamerAudioManager::GetPosition() const {
    return current_position_.load();
}

//=============================================================================
// Source Management
//=============================================================================

AudioSourceHandle GStreamerAudioManager::AddSource(const std::string& file_path, double start_time) {
    AudioClipInfo info;
    info.clip_id = "manual_" + std::to_string(NextHandle());
    info.source_path = file_path;
    info.timeline_start = start_time;
    info.timeline_end = start_time + 3600.0;  // 1 hour default
    info.source_in = 0.0;
    info.source_out = 3600.0;
    info.volume = 1.0f;

    return CreateSource(info);
}

void GStreamerAudioManager::RemoveSource(AudioSourceHandle handle) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    DestroySource(handle);
}

void GStreamerAudioManager::SetSourceVolume(AudioSourceHandle handle, float volume) {
    std::lock_guard<std::mutex> lock(sources_mutex_);

    auto it = sources_.find(handle);
    if (it != sources_.end() && it->second && it->second->volume) {
        it->second->volume_level = std::clamp(volume, 0.0f, 1.0f);
        g_object_set(it->second->volume, "volume", (double)it->second->volume_level, nullptr);
    }
}

bool GStreamerAudioManager::HasAudio(const std::string& file_path) {
    // Use GStreamer discoverer to check if file has audio
    GError* error = nullptr;
    GstDiscoverer* discoverer = gst_discoverer_new(5 * GST_SECOND, &error);

    if (!discoverer) {
        if (error) {
            Debug::Log("[GStreamerAudioManager] Discoverer creation failed: " + std::string(error->message));
            g_error_free(error);
        }
        return false;
    }

    GstDiscovererInfo* info = gst_discoverer_discover_uri(discoverer,
        ("file:///" + file_path).c_str(), &error);

    bool has_audio = false;
    if (info) {
        GList* audio_streams = gst_discoverer_info_get_audio_streams(info);
        has_audio = (audio_streams != nullptr);
        gst_discoverer_stream_info_list_free(audio_streams);
        gst_discoverer_info_unref(info);
    }

    g_object_unref(discoverer);

    if (error) {
        g_error_free(error);
    }

    return has_audio;
}

int GStreamerAudioManager::GetActiveSourceCount() const {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    return static_cast<int>(sources_.size());
}

std::vector<std::string> GStreamerAudioManager::GetActiveSourcePaths() const {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    std::vector<std::string> paths;
    for (const auto& [handle, source] : sources_) {
        if (source) {
            paths.push_back(source->file_path);
        }
    }
    return paths;
}

//=============================================================================
// Source Management (Internal)
//=============================================================================

AudioSourceHandle GStreamerAudioManager::CreateSource(const AudioClipInfo& clip_info) {
    if (!initialized_ || !pipeline_ || !audiomixer_) {
        return 0;
    }

    Debug::Log("[GStreamerAudioManager] Creating source for: " + clip_info.source_path);

    auto source = std::make_unique<AudioSource>();
    source->handle = NextHandle();
    source->file_path = clip_info.source_path;
    source->clip_id = clip_info.clip_id;
    source->timeline_start = clip_info.timeline_start;
    source->timeline_end = clip_info.timeline_end;
    source->source_in = clip_info.source_in;
    source->source_out = clip_info.source_out;
    source->volume_level = clip_info.volume;

    // Create a bin to contain all elements for this source
    std::string bin_name = "source_bin_" + std::to_string(source->handle);
    source->bin = gst_bin_new(bin_name.c_str());
    if (!source->bin) {
        Debug::Log("[GStreamerAudioManager] Failed to create source bin");
        return 0;
    }

    // Create elements
    source->filesrc = gst_element_factory_make("filesrc", nullptr);
    source->decodebin = gst_element_factory_make("decodebin", nullptr);
    source->audioconvert = gst_element_factory_make("audioconvert", nullptr);
    source->audioresample = gst_element_factory_make("audioresample", nullptr);
    source->volume = gst_element_factory_make("volume", nullptr);

    if (!source->filesrc || !source->decodebin || !source->audioconvert ||
        !source->audioresample || !source->volume) {
        Debug::Log("[GStreamerAudioManager] Failed to create source elements");
        if (source->bin) gst_object_unref(source->bin);
        return 0;
    }

    // Configure filesrc
    g_object_set(source->filesrc, "location", clip_info.source_path.c_str(), nullptr);

    // Configure volume
    g_object_set(source->volume, "volume", (double)clip_info.volume, nullptr);

    // Add elements to bin
    gst_bin_add_many(GST_BIN(source->bin), source->filesrc, source->decodebin,
                     source->audioconvert, source->audioresample, source->volume, nullptr);

    // Link filesrc -> decodebin
    if (!gst_element_link(source->filesrc, source->decodebin)) {
        Debug::Log("[GStreamerAudioManager] Failed to link filesrc to decodebin");
        gst_object_unref(source->bin);
        return 0;
    }

    // Link audioconvert -> audioresample -> volume
    if (!gst_element_link_many(source->audioconvert, source->audioresample, source->volume, nullptr)) {
        Debug::Log("[GStreamerAudioManager] Failed to link audio processing chain");
        gst_object_unref(source->bin);
        return 0;
    }

    // Connect decodebin's pad-added signal
    g_signal_connect(source->decodebin, "pad-added", G_CALLBACK(OnPadAdded), source.get());

    // Create ghost pad from volume output (will be linked to audiomixer)
    GstPad* volume_src = gst_element_get_static_pad(source->volume, "src");
    GstPad* ghost_pad = gst_ghost_pad_new("src", volume_src);
    gst_pad_set_active(ghost_pad, TRUE);
    gst_element_add_pad(source->bin, ghost_pad);
    gst_object_unref(volume_src);

    // Add bin to main pipeline
    gst_bin_add(GST_BIN(pipeline_), source->bin);

    // Request pad from audiomixer and link
    source->mixer_pad = gst_element_request_pad_simple(audiomixer_, "sink_%u");
    if (!source->mixer_pad) {
        Debug::Log("[GStreamerAudioManager] Failed to get mixer pad");
        gst_bin_remove(GST_BIN(pipeline_), source->bin);
        return 0;
    }

    GstPad* src_pad = gst_element_get_static_pad(source->bin, "src");
    if (gst_pad_link(src_pad, source->mixer_pad) != GST_PAD_LINK_OK) {
        Debug::Log("[GStreamerAudioManager] Failed to link to mixer");
        gst_object_unref(src_pad);
        gst_element_release_request_pad(audiomixer_, source->mixer_pad);
        gst_bin_remove(GST_BIN(pipeline_), source->bin);
        return 0;
    }
    gst_object_unref(src_pad);

    // Sync state with parent pipeline
    gst_element_sync_state_with_parent(source->bin);

    AudioSourceHandle handle = source->handle;
    sources_[handle] = std::move(source);

    Debug::Log("[GStreamerAudioManager] Source created successfully, handle=" + std::to_string(handle));
    return handle;
}

void GStreamerAudioManager::DestroySource(AudioSourceHandle handle) {
    auto it = sources_.find(handle);
    if (it == sources_.end()) return;

    auto& source = it->second;
    if (!source) {
        sources_.erase(it);
        return;
    }

    Debug::Log("[GStreamerAudioManager] Destroying source: " + source->file_path);

    // Set bin to NULL state
    if (source->bin) {
        gst_element_set_state(source->bin, GST_STATE_NULL);
    }

    // Unlink from mixer
    if (source->mixer_pad && audiomixer_) {
        GstPad* src_pad = gst_element_get_static_pad(source->bin, "src");
        if (src_pad) {
            gst_pad_unlink(src_pad, source->mixer_pad);
            gst_object_unref(src_pad);
        }
        gst_element_release_request_pad(audiomixer_, source->mixer_pad);
    }

    // Remove bin from pipeline
    if (source->bin && pipeline_) {
        gst_bin_remove(GST_BIN(pipeline_), source->bin);
    }

    sources_.erase(it);
}

void GStreamerAudioManager::OnPadAdded(GstElement* element, GstPad* pad, void* data) {
    AudioSource* source = static_cast<AudioSource*>(data);
    if (!source || source->linked) return;

    // Check if this is an audio pad
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }

    if (!caps) return;

    GstStructure* str = gst_caps_get_structure(caps, 0);
    const gchar* name = gst_structure_get_name(str);

    if (g_str_has_prefix(name, "audio/")) {
        // Link to audioconvert
        GstPad* sink_pad = gst_element_get_static_pad(source->audioconvert, "sink");
        if (sink_pad) {
            if (gst_pad_link(pad, sink_pad) == GST_PAD_LINK_OK) {
                source->linked = true;
                Debug::Log("[GStreamerAudioManager] Audio pad linked successfully");
            }
            gst_object_unref(sink_pad);
        }
    }

    gst_caps_unref(caps);
}

AudioSourceHandle GStreamerAudioManager::NextHandle() {
    return next_handle_++;
}

} // namespace ump

#endif // WITH_GSTREAMER
