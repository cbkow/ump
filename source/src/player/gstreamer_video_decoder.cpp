#ifdef WITH_GSTREAMER

#include "gstreamer_video_decoder.h"
#include "gstreamer_manager.h"
#include "../utils/debug_utils.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

// FFmpeg for probing stream start_time (H.264/H.265 PTS sync)
extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>

namespace ump {

//=============================================================================
// Construction / Destruction
//=============================================================================

GStreamerVideoDecoder::GStreamerVideoDecoder(const std::string& video_path)
    : video_path_(video_path)
{
}

GStreamerVideoDecoder::~GStreamerVideoDecoder() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool GStreamerVideoDecoder::Initialize() {
    if (initialized_) {
        return true;
    }

    Debug::Log("[GStreamerVideoDecoder] Initialize: " + video_path_);

    // Ensure GStreamer is initialized
    if (!GStreamerManager::Instance().IsInitialized()) {
        if (!GStreamerManager::Instance().Initialize()) {
            Debug::Log("[GStreamerVideoDecoder] GStreamer init failed: " +
                      GStreamerManager::Instance().GetInitError());
            return false;
        }
    }

    // Build the pipeline
    if (!BuildPipeline()) {
        Debug::Log("[GStreamerVideoDecoder] Failed to build pipeline");
        return false;
    }

    // Extract video metadata
    if (!ExtractMetadata()) {
        Debug::Log("[GStreamerVideoDecoder] Failed to extract metadata");
        DestroyPipeline();
        return false;
    }

    // Probe codec type and stream start_time for H.264/H.265 PTS sync
    // Always probe to detect B-frame codec, even if stream_start_time was set externally
    ProbeStreamStartTime();
    Debug::Log("[BFRAME DEBUG] After ProbeStreamStartTime: is_bframe_codec=" + std::to_string(is_bframe_codec_) +
              ", stream_start_time=" + std::to_string(stream_start_time_ns_ / 1000000.0) + "ms" +
              ", first_pts_captured=" + std::to_string(first_pts_captured_));
    if (stream_start_time_ns_ != 0) {
        Debug::Log("[GStreamerVideoDecoder] Stream start_time offset: " +
                  std::to_string(stream_start_time_ns_ / 1000000.0) + "ms");
    }
    if (is_bframe_codec_) {
        Debug::Log("[GStreamerVideoDecoder] B-frame codec detected - will capture first PTS as baseline");
    }

    // Debug: Log the actual negotiated caps on appsink
    LogNegotiatedCaps();

    // Start decode thread
    running_ = true;
    decode_thread_ = std::thread(&GStreamerVideoDecoder::DecodeThread, this);

    initialized_ = true;
    Debug::Log("[GStreamerVideoDecoder] Initialized: " + video_path_ +
              " (" + std::to_string(width_) + "x" + std::to_string(height_) +
              " @ " + std::to_string(fps_) + " fps, " + std::to_string(frame_count_) + " frames)");

    return true;
}

void GStreamerVideoDecoder::Shutdown() {
    if (!initialized_) {
        return;
    }

    // Stop decode thread
    running_ = false;
    decode_cv_.notify_all();

    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    DestroyPipeline();

    // Clear buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        ring_buffer_.clear();
        buffer_frame_set_.clear();
        buffer_ahead_count_ = 0;
        buffer_behind_count_ = 0;
    }

    last_displayed_frame_.reset();

    // Reset PTS baseline for potential reinitialization
    Debug::Log("[BFRAME DEBUG] Shutdown: resetting first_pts_captured (was " +
              std::to_string(first_pts_captured_) + "), stream_start was " +
              std::to_string(stream_start_time_ns_ / 1000000.0) + "ms");
    first_pts_captured_ = false;
    first_pts_ns_ = 0;

    initialized_ = false;
    Debug::Log("[GStreamerVideoDecoder] Shutdown complete");
}

void GStreamerVideoDecoder::HardReset(int target_frame) {
    Shutdown();
    playhead_frame_ = target_frame;
    Initialize();
}

//=============================================================================
// GStreamer Pipeline
//=============================================================================

bool GStreamerVideoDecoder::BuildPipeline() {
    Debug::Log("[GStreamerVideoDecoder] Building pipeline (mode=" +
               std::string(PipelineModeToString(pipeline_mode_)) + ")...");

    pipeline_ = gst_pipeline_new("video-decoder-pipeline");
    if (!pipeline_) {
        Debug::Log("[GStreamerVideoDecoder] Failed to create pipeline");
        return false;
    }

    filesrc_ = gst_element_factory_make("filesrc", "source");
    decodebin_ = gst_element_factory_make("decodebin", "decoder");
    videoconvert_ = gst_element_factory_make("videoconvert", "converter");
    appsink_ = gst_element_factory_make("appsink", "sink");

    // Create capsfilter to enforce output format (required for HDR/High-Res)
    capsfilter_ = gst_element_factory_make("capsfilter", "formatfilter");

    if (!filesrc_ || !decodebin_ || !videoconvert_ || !appsink_ || !capsfilter_) {
        Debug::Log("[GStreamerVideoDecoder] Failed to create core elements");
        DestroyPipeline();
        return false;
    }

    // Configure filesrc
    g_object_set(filesrc_, "location", video_path_.c_str(), nullptr);

    // Configure appsink
    if (!ConfigureAppSink()) {
        DestroyPipeline();
        return false;
    }

    // Add elements to pipeline: videoconvert -> capsfilter -> appsink
    gst_bin_add_many(GST_BIN(pipeline_), filesrc_, decodebin_, videoconvert_,
                     capsfilter_, appsink_, nullptr);

    // Link filesrc -> decodebin
    if (!gst_element_link(filesrc_, decodebin_)) {
        Debug::Log("[GStreamerVideoDecoder] Failed to link filesrc to decodebin");
        DestroyPipeline();
        return false;
    }

    // Link videoconvert -> capsfilter -> appsink
    if (!gst_element_link_many(videoconvert_, capsfilter_, appsink_, nullptr)) {
        Debug::Log("[GStreamerVideoDecoder] Failed to link videoconvert -> capsfilter -> appsink");
        DestroyPipeline();
        return false;
    }

    // Connect decodebin's pad-added signal
    g_signal_connect(decodebin_, "pad-added", G_CALLBACK(+[](GstElement* element,
                                                              GstPad* pad,
                                                              gpointer data) {
        GStreamerVideoDecoder* self = static_cast<GStreamerVideoDecoder*>(data);

        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) {
            caps = gst_pad_query_caps(pad, nullptr);
        }

        if (caps) {
            GstStructure* str = gst_caps_get_structure(caps, 0);
            const gchar* name = gst_structure_get_name(str);

            if (g_str_has_prefix(name, "video/")) {
                GstPad* sink_pad = gst_element_get_static_pad(self->videoconvert_, "sink");
                if (sink_pad) {
                    gst_pad_link(pad, sink_pad);
                    gst_object_unref(sink_pad);
                    Debug::Log("[GStreamerVideoDecoder] Linked decodebin to videoconvert");
                }
            }
            gst_caps_unref(caps);
        }
    }), this);

    // Set pipeline to PAUSED for preroll
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        Debug::Log("[GStreamerVideoDecoder] Failed to set pipeline to PAUSED");
        DestroyPipeline();
        return false;
    }

    // Wait for preroll
    ret = gst_element_get_state(pipeline_, nullptr, nullptr, 5 * GST_SECOND);
    if (ret != GST_STATE_CHANGE_SUCCESS && ret != GST_STATE_CHANGE_NO_PREROLL) {
        // Get error details from bus
        GstBus* bus = gst_element_get_bus(pipeline_);
        if (bus) {
            GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (msg) {
                GError* err = nullptr;
                gchar* debug_info = nullptr;
                gst_message_parse_error(msg, &err, &debug_info);
                Debug::Log("[GStreamerVideoDecoder] Pipeline preroll failed: " +
                          std::string(err ? err->message : "unknown error"));
                if (debug_info) {
                    Debug::Log("[GStreamerVideoDecoder] Debug info: " + std::string(debug_info));
                }
                if (err) g_error_free(err);
                if (debug_info) g_free(debug_info);
                gst_message_unref(msg);
            } else {
                Debug::Log("[GStreamerVideoDecoder] Pipeline preroll failed (state=" +
                          std::to_string(ret) + ", no error message)");
            }
            gst_object_unref(bus);
        } else {
            Debug::Log("[GStreamerVideoDecoder] Pipeline preroll failed");
        }
        DestroyPipeline();
        return false;
    }

    Debug::Log("[GStreamerVideoDecoder] Pipeline ready");
    return true;
}

void GStreamerVideoDecoder::DestroyPipeline() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    filesrc_ = nullptr;
    decodebin_ = nullptr;
    videoconvert_ = nullptr;
    capsfilter_ = nullptr;
    appsink_ = nullptr;
}

bool GStreamerVideoDecoder::ConfigureAppSink() {
    // Select output format based on pipeline mode
    // NORMAL: RGBA (8-bit per channel) - best performance
    // HIGH_RES: RGBA64_LE (16-bit per channel) - for 10/12-bit video content and HDR
    // NOTE: Float formats not available - GStreamer only supports integer formats
    const char* format = "RGBA";  // Default 8-bit

    if (pipeline_mode_ == PipelineMode::HIGH_RES) {
        format = "RGBA64_LE";  // 16-bit per channel, little-endian
        Debug::Log("[GStreamerVideoDecoder] Using RGBA64_LE format for high-res pipeline");
    }

    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, format,
        nullptr);

    gst_app_sink_set_caps(GST_APP_SINK(appsink_), caps);
    gst_caps_unref(caps);

    gst_app_sink_set_emit_signals(GST_APP_SINK(appsink_), FALSE);
    gst_app_sink_set_drop(GST_APP_SINK(appsink_), FALSE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink_), 2);  // Small GStreamer buffer

    return true;
}

void GStreamerVideoDecoder::LogNegotiatedCaps() {
    if (!appsink_) return;

    // Get the pad and its negotiated caps
    GstPad* pad = gst_element_get_static_pad(appsink_, "sink");
    if (!pad) {
        Debug::Log("[GStreamerVideoDecoder] DIAG: Could not get appsink pad");
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        // Try allowed caps if current not yet negotiated
        caps = gst_pad_get_allowed_caps(pad);
        if (caps) {
            gchar* caps_str = gst_caps_to_string(caps);
            Debug::Log("[GStreamerVideoDecoder] DIAG: Allowed caps (not yet negotiated): " + std::string(caps_str));
            g_free(caps_str);
            gst_caps_unref(caps);
        } else {
            Debug::Log("[GStreamerVideoDecoder] DIAG: No caps available yet");
        }
        gst_object_unref(pad);
        return;
    }

    gchar* caps_str = gst_caps_to_string(caps);
    Debug::Log("[GStreamerVideoDecoder] DIAG: ACTUAL negotiated caps: " + std::string(caps_str));
    g_free(caps_str);

    // Parse and log specific fields
    GstStructure* s = gst_caps_get_structure(caps, 0);
    if (s) {
        const gchar* format = gst_structure_get_string(s, "format");
        const gchar* colorimetry = gst_structure_get_string(s, "colorimetry");
        gint width = 0, height = 0;
        gst_structure_get_int(s, "width", &width);
        gst_structure_get_int(s, "height", &height);

        Debug::Log("[GStreamerVideoDecoder] DIAG: format=" + std::string(format ? format : "NULL") +
                   ", colorimetry=" + std::string(colorimetry ? colorimetry : "NULL") +
                   ", size=" + std::to_string(width) + "x" + std::to_string(height));
    }

    gst_caps_unref(caps);
    gst_object_unref(pad);
}

void GStreamerVideoDecoder::SetPipelineMode(PipelineMode mode) {
    if (pipeline_mode_ == mode) return;

    Debug::Log("[GStreamerVideoDecoder] Pipeline mode changed to " +
               std::string(PipelineModeToString(mode)));

    pipeline_mode_ = mode;

    // Rebuild pipeline to apply new output format
    // (RGBA vs RGBA64 requires different appsink caps)
    if (initialized_) {
        HardReset(playhead_frame_.load());
    }
}

bool GStreamerVideoDecoder::ExtractMetadata() {
    gint64 duration_ns = 0;
    if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration_ns)) {
        Debug::Log("[GStreamerVideoDecoder] Failed to query duration");
        return false;
    }
    duration_ = static_cast<double>(duration_ns) / GST_SECOND;

    GstPad* pad = gst_element_get_static_pad(appsink_, "sink");
    if (!pad) {
        Debug::Log("[GStreamerVideoDecoder] Failed to get appsink pad");
        return false;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        gst_object_unref(pad);
        Debug::Log("[GStreamerVideoDecoder] Failed to get caps");
        return false;
    }

    GstVideoInfo video_info;
    if (!gst_video_info_from_caps(&video_info, caps)) {
        gst_caps_unref(caps);
        gst_object_unref(pad);
        return false;
    }

    width_ = GST_VIDEO_INFO_WIDTH(&video_info);
    height_ = GST_VIDEO_INFO_HEIGHT(&video_info);

    if (GST_VIDEO_INFO_FPS_D(&video_info) > 0) {
        fps_ = static_cast<double>(GST_VIDEO_INFO_FPS_N(&video_info)) /
               static_cast<double>(GST_VIDEO_INFO_FPS_D(&video_info));
    } else {
        fps_ = 24.0;
    }

    frame_count_ = static_cast<int>(duration_ * fps_ + 0.5);

    gst_caps_unref(caps);
    gst_object_unref(pad);

    DetectHardwareAcceleration();
    return true;
}

void GStreamerVideoDecoder::DetectHardwareAcceleration() {
    hw_accel_type_ = HWAccelType::NONE;

    if (!decodebin_) return;

    GstIterator* iter = gst_bin_iterate_recurse(GST_BIN(decodebin_));
    if (!iter) return;

    GValue item = G_VALUE_INIT;
    bool done = false;

    while (!done) {
        switch (gst_iterator_next(iter, &item)) {
            case GST_ITERATOR_OK: {
                GstElement* element = GST_ELEMENT(g_value_get_object(&item));
                if (element) {
                    GstElementFactory* factory = gst_element_get_factory(element);
                    if (factory) {
                        const gchar* factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
                        std::string name(factory_name ? factory_name : "");

                        // Detect B-frame codecs (H.264/H.265) for PTS sync
                        if (name.find("h264") != std::string::npos ||
                            name.find("h265") != std::string::npos ||
                            name.find("hevc") != std::string::npos ||
                            name.find("avc") != std::string::npos) {
                            is_bframe_codec_ = true;
                        }

                        if (name.find("d3d11") != std::string::npos) {
                            hw_accel_type_ = HWAccelType::D3D11VA;
                            Debug::Log("[GStreamerVideoDecoder] Using D3D11VA: " + name);
                            done = true;
                        } else if (name.find("nv") == 0 || name.find("cuda") != std::string::npos) {
                            hw_accel_type_ = HWAccelType::CUDA;
                            Debug::Log("[GStreamerVideoDecoder] Using CUDA: " + name);
                            done = true;
                        }
                    }
                }
                g_value_reset(&item);
                break;
            }
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(iter);
                break;
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                done = true;
                break;
        }
    }

    g_value_unset(&item);
    gst_iterator_free(iter);
}

void GStreamerVideoDecoder::ProbeStreamStartTime() {
    // Use FFmpeg to probe stream start_time and codec type for H.264/H.265 PTS synchronization
    // This accounts for B-frame reordering delay and container edit lists
    AVFormatContext* format_ctx = nullptr;
    if (avformat_open_input(&format_ctx, video_path_.c_str(), nullptr, nullptr) < 0) {
        return;  // Failed to open - leave start_time at 0
    }

    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        avformat_close_input(&format_ctx);
        return;
    }

    int video_stream = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream >= 0) {
        AVStream* stream = format_ctx->streams[video_stream];

        // Detect B-frame codecs (H.264/H.265) - these need PTS baseline adjustment
        AVCodecID codec_id = stream->codecpar->codec_id;
        if (codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC) {
            is_bframe_codec_ = true;

            // Get B-frame reorder delay from codec parameters
            // video_delay is the number of frames the decoder needs to buffer before outputting
            int video_delay = stream->codecpar->video_delay;
            if (video_delay > 0) {
                bframe_delay_ = video_delay;
            } else {
                // Fallback: typical H.264/H.265 uses 2 B-frames
                bframe_delay_ = 2;
            }

            Debug::Log("[GStreamerVideoDecoder] Detected B-frame codec: " +
                      std::string(codec_id == AV_CODEC_ID_H264 ? "H.264" : "H.265/HEVC") +
                      ", reorder delay=" + std::to_string(bframe_delay_) + " frames");
        }

        // Only set stream_start_time if not already set externally
        if (stream_start_time_ns_ == 0 && stream->start_time != AV_NOPTS_VALUE && stream->start_time > 0) {
            // Convert from stream timebase to nanoseconds for GStreamer
            int64_t start_time_us = av_rescale_q(stream->start_time,
                                                  stream->time_base,
                                                  AV_TIME_BASE_Q);
            stream_start_time_ns_ = start_time_us * 1000;  // microseconds to nanoseconds
        }
    }

    avformat_close_input(&format_ctx);
}

//=============================================================================
// Decode Thread - Fills ring buffer around playhead
// In scrub/shuttle mode: prioritizes immediate single-frame seeks
// In normal mode: buffers ahead for smooth playback
//=============================================================================

void GStreamerVideoDecoder::DecodeThread() {
    Debug::Log("[GStreamerVideoDecoder] Decode thread started");

    // Set pipeline to PLAYING
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);

    int last_scrub_frame = -1;  // Track last scrub target to avoid redundant seeks

    while (running_.load()) {
        //=====================================================================
        // SCRUB MODE: Immediate single-frame seek, no buffering
        // Used for: FF/RW buttons, mouse scrubbing on timeline
        //=====================================================================
        if (shuttle_mode_.load()) {
            int target = playhead_frame_.load();

            // Only seek if target changed
            if (target != last_scrub_frame) {
                last_scrub_frame = target;

                // Check if we already have this frame buffered
                std::shared_ptr<PixelData> cached_frame;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    cached_frame = FindInBuffer(target);
                }

                if (cached_frame) {
                    // Frame already in buffer - just update last displayed
                    last_displayed_frame_ = cached_frame;
                    last_displayed_frame_number_ = target;
                } else {
                    // Need to seek and decode - do it immediately
                    SeekToFrame(target);

                    int frame_num = -1;
                    auto pixels = PullNextFrame(&frame_num);

                    if (pixels) {
                        // DEBUG: Check if GStreamer returned the frame we requested
                        if (frame_num != target) {
                            Debug::Log("[GStreamer SEEK MISMATCH] requested=" + std::to_string(target) +
                                      " got=" + std::to_string(frame_num) +
                                      " diff=" + std::to_string(frame_num - target));
                        }

                        // Store as last displayed for immediate access
                        last_displayed_frame_ = pixels;
                        last_displayed_frame_number_ = frame_num;

                        // Also add to buffer for potential cache hit on re-scrub
                        AddToBuffer(frame_num, pixels);
                        decode_frame_ = frame_num;
                    }
                }
            }

            // In scrub mode, just wait briefly for next scrub position
            // Don't do any buffering ahead - responsiveness is priority
            std::unique_lock<std::mutex> lock(decode_mutex_);
            decode_cv_.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }

        // Reset scrub tracking when exiting scrub mode
        last_scrub_frame = -1;

        //=====================================================================
        // NORMAL MODE: Buffer ahead for smooth playback
        //=====================================================================

        // Handle any pending seek
        if (seek_pending_.load()) {
            int target = seek_target_frame_;
            seek_pending_ = false;

            // SMART EVICTION: Only evict frames outside the new window
            // Keep frames that are still useful (within [target - behind, target + ahead])
            // This prevents stuttering when scrubbing within or near cached range
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);

                int keep_start = target - config_.readBehindFrames;
                int keep_end = target + config_.readAheadFrames;

                // Check if EXACT target frame is already buffered
                // Only skip GStreamer seek if we have the precise frame requested
                // NOTE: Previously had target+1 tolerance, but this caused bugs when
                // seeking from 58 to 57 - having 58 doesn't mean we have 57!
                bool target_buffered = buffer_frame_set_.count(target) > 0;

                // Evict frames outside the new window
                auto it = ring_buffer_.begin();
                while (it != ring_buffer_.end()) {
                    if (it->frame_number < keep_start || it->frame_number > keep_end) {
                        buffer_frame_set_.erase(it->frame_number);
                        it = ring_buffer_.erase(it);
                    } else {
                        ++it;
                    }
                }
                UpdateBufferCounts();

                // If target is already buffered and we have frames ahead, skip GStreamer seek
                // Just update decode position to continue filling from where we need
                if (target_buffered && buffer_ahead_count_.load() > 0) {
                    decode_frame_ = target;
                    eof_reached_ = false;
                    Debug::Log("[GStreamerVideoDecoder] Seek to " + std::to_string(target) +
                               " - frame buffered, skipping GStreamer seek");
                    continue;
                }
            }

            // Execute the seek (only if we actually need to reposition GStreamer)
            SeekToFrame(target);
            decode_frame_ = target;

            // Pull frames after seek to populate buffer
            // For B-frame codecs: keep pulling until we have the TARGET frame
            // Due to B-frame reordering, GStreamer outputs frames in decode order,
            // not display order. Seeking to frame 57 might output 55, 56, 58 first.
            {
                if (is_bframe_codec_) {
                    // Pull frames until we have the target, with reasonable limit
                    // GOP is typically 15-30 frames, so we may need to decode many frames
                    // after seeking to a keyframe before getting the target display frame
                    const int max_pulls = 20;
                    bool have_target = false;

                    for (int i = 0; i < max_pulls && !have_target; i++) {
                        int frame_num = -1;
                        auto pixels = PullNextFrame(&frame_num);
                        if (pixels && frame_num >= 0) {
                            AddToBuffer(frame_num, pixels);
                            decode_frame_ = frame_num;

                            // Check if we now have the target
                            std::lock_guard<std::mutex> lock(buffer_mutex_);
                            have_target = buffer_frame_set_.count(target) > 0;
                        } else {
                            break;  // No more frames available
                        }
                    }

                    if (!have_target) {
                        Debug::Log("[BFRAME] Seek to " + std::to_string(target) +
                                  " - target not yet decoded after " + std::to_string(max_pulls) + " pulls");
                    }
                } else {
                    // Non-B-frame codec: single pull is sufficient
                    int frame_num = -1;
                    auto pixels = PullNextFrame(&frame_num);
                    if (pixels && frame_num >= 0) {
                        AddToBuffer(frame_num, pixels);
                        decode_frame_ = frame_num;
                    }
                }
            }

            continue;
        }

        // PRIORITY 1: Check if playhead frame is missing and needs immediate seek
        // Only for non-B-frame codecs - B-frame codecs use simpler buffer-fill approach
        if (!is_bframe_codec_) {
            int playhead = playhead_frame_.load();
            bool playhead_missing = false;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                playhead_missing = (buffer_frame_set_.count(playhead) == 0);

                // TOLERANCE: Accept if we have playhead+1 (common off-by-one seek result)
                // This prevents seek loops when GStreamer consistently returns frame+1
                if (playhead_missing && buffer_frame_set_.count(playhead + 1) > 0) {
                    playhead_missing = false;  // Close enough
                }
            }

            if (playhead_missing) {
                // Check if we're close enough to reach playhead by sequential decode
                int distance_to_playhead = playhead - decode_frame_;
                // TOLERANCE: Allow -1 to handle GStreamer off-by-one seeks
                // Only re-seek if we're more than 1 frame behind or more than 5 ahead
                if (distance_to_playhead < -1 || distance_to_playhead > 5) {
                    // Too far or behind - need to seek
                    SeekToFrame(playhead);
                    decode_frame_ = playhead;

                    // Immediately decode the playhead frame
                    int frame_num = -1;
                    auto pixels = PullNextFrame(&frame_num);
                    if (pixels && frame_num >= 0) {
                        AddToBuffer(frame_num, pixels);
                        decode_frame_ = frame_num;
                    }
                    continue;
                }
            }
        }

        // PRIORITY 2: Fill buffer ahead
        if (NeedsMoreFramesAhead()) {
            // Pull next frame from GStreamer
            int frame_num = -1;
            auto pixels = PullNextFrame(&frame_num);

            if (pixels && frame_num >= 0) {
                AddToBuffer(frame_num, pixels);
                decode_frame_ = frame_num;
                // Log occasionally for B-frame codecs
                static int fill_log_count = 0;
                if (is_bframe_codec_ && fill_log_count < 10) {
                    Debug::Log("[BFRAME DEBUG] Buffer fill: added frame " + std::to_string(frame_num));
                    fill_log_count++;
                }
            } else {
                // Check for EOS
                if (!running_.load()) break;

                GstBus* bus = gst_element_get_bus(pipeline_);
                GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_EOS);
                if (msg) {
                    eof_reached_ = true;
                    gst_message_unref(msg);
                }
                gst_object_unref(bus);

                // Small sleep if no frame available
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            // Buffer is full, wait for playhead to move or seek
            std::unique_lock<std::mutex> lock(decode_mutex_);
            decode_cv_.wait_for(lock, std::chrono::milliseconds(50));
        }

        // Periodically evict frames outside window
        EvictOutsideWindow();
    }

    Debug::Log("[GStreamerVideoDecoder] Decode thread stopped");
}

//=============================================================================
// Ring Buffer Management
//=============================================================================

void GStreamerVideoDecoder::AddToBuffer(int frame_number, std::shared_ptr<PixelData> pixels) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Check if already in buffer
    if (buffer_frame_set_.count(frame_number)) {
        return;
    }

    // Add to buffer
    BufferedFrame bf;
    bf.frame_number = frame_number;
    bf.pixels = pixels;

    // Insert in sorted order (by frame number)
    auto it = std::lower_bound(ring_buffer_.begin(), ring_buffer_.end(), bf,
        [](const BufferedFrame& a, const BufferedFrame& b) {
            return a.frame_number < b.frame_number;
        });
    ring_buffer_.insert(it, bf);
    buffer_frame_set_.insert(frame_number);

    // Update counts
    UpdateBufferCounts();
}

std::shared_ptr<PixelData> GStreamerVideoDecoder::FindInBuffer(int frame_number) const {
    // Buffer is sorted, linear search is fine for typical sizes (~100 frames)
    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number == frame_number) {
            return bf.pixels;
        }
        if (bf.frame_number > frame_number) {
            break;  // Past target in sorted buffer
        }
    }
    return nullptr;
}

std::shared_ptr<PixelData> GStreamerVideoDecoder::FindClosestInBuffer(int frame_number, int* actual_frame) const {
    if (ring_buffer_.empty()) {
        if (actual_frame) *actual_frame = -1;
        return nullptr;
    }

    // Find closest frame
    int best_frame = -1;
    int best_distance = INT_MAX;
    std::shared_ptr<PixelData> best_pixels;

    for (const auto& bf : ring_buffer_) {
        int distance = std::abs(bf.frame_number - frame_number);
        if (distance < best_distance) {
            best_distance = distance;
            best_frame = bf.frame_number;
            best_pixels = bf.pixels;
        }
    }

    if (actual_frame) *actual_frame = best_frame;
    return best_pixels;
}

bool GStreamerVideoDecoder::NeedsMoreFramesAhead() const {
    int ahead = buffer_ahead_count_.load();

    // Need more if we don't have enough ahead of playhead
    return ahead < config_.readAheadFrames;
}

void GStreamerVideoDecoder::EvictOutsideWindow() {
    int playhead = playhead_frame_.load();

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    int behind_limit = playhead - config_.readBehindFrames;
    int ahead_limit = playhead + config_.readAheadFrames;

    // Remove frames outside [behind_limit, ahead_limit]
    auto it = ring_buffer_.begin();
    while (it != ring_buffer_.end()) {
        if (it->frame_number < behind_limit || it->frame_number > ahead_limit) {
            buffer_frame_set_.erase(it->frame_number);
            it = ring_buffer_.erase(it);
        } else {
            ++it;
        }
    }

    // Update counts after eviction
    UpdateBufferCounts();
}

void GStreamerVideoDecoder::UpdateBufferCounts() {
    // Must be called with buffer_mutex_ held
    int playhead = playhead_frame_.load();
    int ahead = 0;
    int behind = 0;

    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number > playhead) {
            ahead++;
        } else if (bf.frame_number < playhead) {
            behind++;
        }
        // Frame at playhead counts as neither ahead nor behind
    }

    buffer_ahead_count_ = ahead;
    buffer_behind_count_ = behind;
}

//=============================================================================
// Frame Pulling from GStreamer
//=============================================================================

std::shared_ptr<PixelData> GStreamerVideoDecoder::PullNextFrame(int* out_frame_number) {
    if (!appsink_) return nullptr;

    GstSample* sample = gst_app_sink_try_pull_sample(
        GST_APP_SINK(appsink_),
        50 * GST_MSECOND  // 50ms timeout
    );

    if (!sample) {
        return nullptr;
    }

    if (out_frame_number) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer && GST_BUFFER_PTS_IS_VALID(buffer)) {
            int64_t pts = GST_BUFFER_PTS(buffer);

            // Capture first frame's PTS as baseline (handles B-frame reorder delay)
            // This is more accurate than container start_time for H.264/H.265
            if (!first_pts_captured_) {
                first_pts_ns_ = pts;
                first_pts_captured_ = true;
                Debug::Log("[BFRAME DEBUG] First PTS captured: " + std::to_string(first_pts_ns_ / 1000000.0) +
                          "ms, current stream_start=" + std::to_string(stream_start_time_ns_ / 1000000.0) +
                          "ms, is_bframe=" + std::to_string(is_bframe_codec_));
                // Use whichever is larger: container start_time or first frame PTS
                // This handles both edit list offsets and B-frame reorder delay
                if (first_pts_ns_ > stream_start_time_ns_) {
                    int64_t old_start = stream_start_time_ns_;
                    stream_start_time_ns_ = first_pts_ns_;
                    Debug::Log("[BFRAME DEBUG] UPDATED stream_start_time: " +
                              std::to_string(old_start / 1000000.0) + "ms -> " +
                              std::to_string(stream_start_time_ns_ / 1000000.0) + "ms (B-frame delay)");
                } else {
                    Debug::Log("[BFRAME DEBUG] NOT updating stream_start (first_pts <= stream_start)");
                }
            }

            *out_frame_number = FrameNumberFromTimestamp(pts);
        } else {
            *out_frame_number = decode_frame_ + 1;
        }
    }

    auto pixels = ConvertSampleToPixelData(sample);
    gst_sample_unref(sample);

    return pixels;
}

std::shared_ptr<PixelData> GStreamerVideoDecoder::ConvertSampleToPixelData(GstSample* sample) {
    if (!sample) return nullptr;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) return nullptr;

    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps) return nullptr;

    GstVideoInfo video_info;
    if (!gst_video_info_from_caps(&video_info, caps)) {
        return nullptr;
    }

    int width = GST_VIDEO_INFO_WIDTH(&video_info);
    int height = GST_VIDEO_INFO_HEIGHT(&video_info);

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        return nullptr;
    }

    auto pixels = std::make_shared<PixelData>();
    pixels->width = width;
    pixels->height = height;

    // Detect format from actual video info and set GL parameters accordingly
    GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&video_info);
    const gchar* fmt_str = gst_video_format_to_string(fmt);
    size_t data_size;

    // Log format detection once per pipeline (not every frame)
    static int logged_format_hash = 0;
    int current_hash = static_cast<int>(fmt) ^ width ^ height;
    if (logged_format_hash != current_hash) {
        logged_format_hash = current_hash;
        Debug::Log("[GStreamerVideoDecoder] Output format: " + std::string(fmt_str ? fmt_str : "unknown") +
                   " (" + std::to_string(width) + "x" + std::to_string(height) + ")" +
                   " requested=" + std::string(PipelineModeToString(pipeline_mode_)));
    }

    // NOTE: GStreamer doesn't support float formats (RGBAF16/RGBAF32)
    // Best available is RGBA64_LE (16-bit integer) for HIGH_RES mode (e.g., ProRes 4444 12-bit)
    if (fmt == GST_VIDEO_FORMAT_RGBA64_LE || fmt == GST_VIDEO_FORMAT_RGBA64_BE) {
        // 16-bit integer per channel (64-bit per pixel) - preserves 10/12-bit content
        pixels->gl_format = GL_RGBA;
        pixels->gl_type = GL_UNSIGNED_SHORT;
        pixels->pipeline_mode = PipelineMode::HIGH_RES;
        data_size = width * height * 8;  // 8 bytes per pixel
    } else {
        // Default: 8-bit per channel (32-bit per pixel) - RGBA
        pixels->gl_format = GL_RGBA;
        pixels->gl_type = GL_UNSIGNED_BYTE;
        pixels->pipeline_mode = PipelineMode::NORMAL;
        data_size = width * height * 4;  // 4 bytes per pixel
    }

    pixels->pixels.resize(data_size);

    if (map.size >= data_size) {
        memcpy(pixels->pixels.data(), map.data, data_size);
    }

    gst_buffer_unmap(buffer, &map);

    return pixels;
}

int GStreamerVideoDecoder::FrameNumberFromTimestamp(int64_t timestamp_ns) const {
    if (fps_ <= 0) return 0;
    // Subtract stream start time offset for correct frame mapping (H.264/H.265 B-frame sync)
    int64_t adjusted_ns = timestamp_ns - stream_start_time_ns_;
    if (adjusted_ns < 0) adjusted_ns = 0;
    double time_seconds = static_cast<double>(adjusted_ns) / GST_SECOND;
    int frame = static_cast<int>(time_seconds * fps_ + 0.5);

    // Debug: log calculation for first few calls
    static int calc_log_count = 0;
    if (calc_log_count < 5) {
        Debug::Log("[BFRAME DEBUG] FrameNumberFromTimestamp: pts=" + std::to_string(timestamp_ns / 1000000.0) +
                  "ms - start_offset=" + std::to_string(stream_start_time_ns_ / 1000000.0) +
                  "ms = adjusted=" + std::to_string(adjusted_ns / 1000000.0) +
                  "ms -> frame " + std::to_string(frame));
        calc_log_count++;
    }
    return frame;
}

//=============================================================================
// Seeking
//=============================================================================

bool GStreamerVideoDecoder::SeekToFrame(int target_frame) {
    if (!pipeline_) return false;

    // Calculate seek timestamp
    // B-frame codecs: subtract frames to account for B-frame reorder delay
    // (GStreamer outputs frames ahead of seek position due to B/P frame decoding order)
    // bframe_delay_ is detected from FFmpeg video_delay parameter
    // Non-B-frame codecs (ProRes): no offset needed
    int seek_frame = target_frame;
    if (is_bframe_codec_ && target_frame >= bframe_delay_) {
        seek_frame = target_frame - bframe_delay_;
    }

    int64_t timestamp_ns;
    if (target_frame == 0) {
        // For frame 0, seek to the absolute start of the stream
        // This ensures we capture frames 0 and 1 which might be missed
        // if we seek to the "center" of frame 0 (past where decoding needs to start)
        timestamp_ns = 0;
    } else {
        // Seek to slightly BEFORE the frame start to ensure we get the target frame
        // GStreamer's GST_SEEK_FLAG_ACCURATE returns the frame AT OR AFTER the timestamp
        // Using +0.5 (center) caused consistent +1 frame offset on ProRes and other codecs
        // Using +0.1 (10% into frame) gives buffer while still landing on correct frame
        double frame_offset = 0.1;
        timestamp_ns = static_cast<int64_t>(
            ((static_cast<double>(seek_frame) + frame_offset) / fps_) * GST_SECOND
        );
        // Add stream start time offset for correct seek position (H.264/H.265 B-frame sync)
        timestamp_ns += stream_start_time_ns_;
    }

    Debug::Log("[BFRAME DEBUG] SeekToFrame: target=" + std::to_string(target_frame) +
              " seek_frame=" + std::to_string(seek_frame) +
              " is_bframe=" + std::to_string(is_bframe_codec_) +
              " start_time=" + std::to_string(stream_start_time_ns_ / 1000000.0) +
              "ms -> seek_ts=" + std::to_string(timestamp_ns / 1000000.0) + "ms");

    gboolean result = gst_element_seek(
        pipeline_,
        1.0,
        GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET,
        timestamp_ns,
        GST_SEEK_TYPE_NONE,
        GST_CLOCK_TIME_NONE
    );

    if (!result) {
        Debug::Log("[GStreamerVideoDecoder] Seek failed");
        return false;
    }

    eof_reached_ = false;
    return true;
}

bool GStreamerVideoDecoder::SeekToTime(int64_t timestamp_ns) {
    int frame = FrameNumberFromTimestamp(timestamp_ns);
    return SeekToFrame(frame);
}

//=============================================================================
// Frame Access - Check buffer first
//=============================================================================

std::shared_ptr<PixelData> GStreamerVideoDecoder::GetFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Debug: log GetFrame requests
    static int getframe_log_count = 0;
    if (getframe_log_count < 20) {
        std::string buffer_contents;
        for (const auto& bf : ring_buffer_) {
            buffer_contents += std::to_string(bf.frame_number) + ",";
        }
        Debug::Log("[BFRAME DEBUG] GetFrame(" + std::to_string(frame_number) +
                  ") buffer=[" + buffer_contents + "] size=" + std::to_string(ring_buffer_.size()));
        getframe_log_count++;
    }

    // Check if we have the exact frame in buffer
    if (buffer_frame_set_.count(frame_number)) {
        auto frame = FindInBuffer(frame_number);
        if (frame) {
            last_displayed_frame_ = frame;
            last_displayed_frame_number_ = frame_number;
            return frame;
        }
    }

    // No fallbacks - only return exact frames
    // This prevents flashing wrong frames during seeks/transitions
    return nullptr;
}

std::shared_ptr<PixelData> GStreamerVideoDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    auto frame = FindClosestInBuffer(frame_number, actual_frame);
    if (frame) {
        last_displayed_frame_ = frame;
        if (actual_frame) {
            last_displayed_frame_number_ = *actual_frame;
        }
        return frame;
    }

    if (actual_frame) {
        *actual_frame = last_displayed_frame_number_;
    }
    return last_displayed_frame_;
}

bool GStreamerVideoDecoder::HasFrame(int frame_number) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return buffer_frame_set_.count(frame_number) > 0;
}

//=============================================================================
// Keyframe Access
//=============================================================================

std::shared_ptr<PixelData> GStreamerVideoDecoder::GetKeyframe(int target_frame, int* actual_keyframe) {
    if (actual_keyframe) *actual_keyframe = target_frame;
    return GetFrame(target_frame);
}

int GStreamerVideoDecoder::GetNearestKeyframePosition(int target_frame) const {
    return target_frame;  // Treat all frames as keyframes
}

//=============================================================================
// Playhead Management
//=============================================================================

void GStreamerVideoDecoder::UpdatePlayhead(int frame_number, SeekQuality quality, bool force_seek) {
    (void)quality;  // Not used currently

    int old_playhead = playhead_frame_.exchange(frame_number);
    int distance = std::abs(frame_number - old_playhead);

    // If force_seek is true, skip all tolerance checks - always seek to exact frame
    // This is used for user-initiated seeks (mouse clicks) that need precise positioning
    if (!force_seek) {
        // OPTIMIZATION: If frame is already buffered, no seek needed
        // This prevents unnecessary buffer clearing during playback
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (buffer_frame_set_.count(frame_number) > 0) {
            // Exact frame buffered - no seek needed
            decode_cv_.notify_one();
            return;
        }
    }

    // Check if we need to seek
    // Seek if: forced, or jumped significantly, or frame is outside our buffer window
    bool need_seek = force_seek;

    if (!need_seek && distance > config_.readAheadFrames / 2) {
        need_seek = true;
    }

    // Also check if the requested frame is in our buffer
    if (!need_seek) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (buffer_frame_set_.count(frame_number) == 0) {
            // Frame not in buffer - check if it's ahead of decode position
            if (frame_number > decode_frame_ + config_.readAheadFrames ||
                frame_number < decode_frame_ - config_.readBehindFrames) {
                need_seek = true;
            }
        }
    }

    if (need_seek) {
        // Debounce: don't re-request same frame within 100ms
        // This prevents seek loops when UI rapidly calls UpdatePlayhead
        // while the decode thread is still processing a seek
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_seek_time_);

        if (frame_number == last_seek_requested_ && elapsed.count() < 100) {
            // Same frame requested recently - just wake thread, don't queue another seek
            decode_cv_.notify_one();
            return;
        }

        last_seek_requested_ = frame_number;
        last_seek_time_ = now;
        seek_target_frame_ = frame_number;
        seek_pending_ = true;
    }

    // Wake decode thread to fill buffer
    decode_cv_.notify_one();
}

//=============================================================================
// Buffer Status
//=============================================================================

int GStreamerVideoDecoder::GetBufferedAhead() const {
    return buffer_ahead_count_.load();
}

int GStreamerVideoDecoder::GetBufferedBehind() const {
    return buffer_behind_count_.load();
}

int GStreamerVideoDecoder::GetBufferSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return static_cast<int>(ring_buffer_.size());
}

void GStreamerVideoDecoder::GetBufferedRange(int& start_frame, int& end_frame) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (ring_buffer_.empty()) {
        start_frame = -1;
        end_frame = -1;
        return;
    }

    start_frame = ring_buffer_.front().frame_number;
    end_frame = ring_buffer_.back().frame_number;
}

void GStreamerVideoDecoder::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    ring_buffer_.clear();
    buffer_frame_set_.clear();
    buffer_ahead_count_ = 0;
    buffer_behind_count_ = 0;
}

DecodeStatus GStreamerVideoDecoder::GetDecodeStatus() const {
    DecodeStatus status;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (const auto& bf : ring_buffer_) {
        status.have.push_back(bf.frame_number);
    }
    status.currently_decoding = decode_frame_;

    return status;
}

void GStreamerVideoDecoder::EvictOutsideWindow(const std::set<int>& keep_frames) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    auto it = ring_buffer_.begin();
    while (it != ring_buffer_.end()) {
        if (keep_frames.count(it->frame_number) == 0) {
            buffer_frame_set_.erase(it->frame_number);
            it = ring_buffer_.erase(it);
        } else {
            ++it;
        }
    }

    UpdateBufferCounts();
}

std::set<int> GStreamerVideoDecoder::GetBufferedFramesSet() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return std::set<int>(buffer_frame_set_.begin(), buffer_frame_set_.end());
}

//=============================================================================
// Loop Points (hints only - app handles actual looping)
//=============================================================================

void GStreamerVideoDecoder::SetLoopPoints(int start_frame, int end_frame) {
    loop_start_frame_ = start_frame;
    loop_end_frame_ = end_frame;
    Debug::Log("[GStreamerVideoDecoder] Loop points noted: " +
              std::to_string(start_frame) + " - " + std::to_string(end_frame));
}

void GStreamerVideoDecoder::ClearLoopPoints() {
    loop_start_frame_ = 0;
    loop_end_frame_ = -1;
    Debug::Log("[GStreamerVideoDecoder] Loop points cleared");
}

} // namespace ump

#endif // WITH_GSTREAMER
