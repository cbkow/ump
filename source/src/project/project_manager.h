#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "media_item.h"
#include "../metadata/adobe_metadata.h"
#include "../metadata/video_metadata.h"
#include "../metadata/exr_metadata.h"
#include "../player/frame_cache.h"
#include "../player/image_sequence_config.h"
#include "../utils/exr_layer_detector.h"
#include "../transcode/transcode_queue.h"
#include "../transcode/transcode_worker_pool.h"
#include "../ui/transcode_queue_window.h"

// Include VideoPlayer (alias to VideoDisplayComponent)
#include "../player/video_player.h"

namespace ump {
    class TimelineView;  // Forward declaration for EXR display
    struct CombinedMetadata;
    
    // VideoCache manager - manages multiple FrameCache instances per video file
    class VideoCache {
    public:
        VideoCache();
        ~VideoCache();
        
        // Core interface
        FrameCache* GetCacheForVideo(const std::string& video_path);
        void SetCurrentVideo(const std::string& video_path);
        bool GetCachedFrame(const std::string& video_path, double timestamp, GLuint& texture_id, int& width, int& height);
        void NotifyVideoChanged(const std::string& video_path, VideoPlayer* video_player);
        void NotifyPlaybackState(bool is_playing);
        
        // Configuration
        void SetCacheConfig(const FrameCache::CacheConfig& config);

        // Pipeline mode access
        PipelineMode GetPipelineMode() const;  // Get pipeline mode from current video cache

        // Statistics
        FrameCache::CacheStats GetTotalStats() const;
        FrameCache::CacheStats GetStatsForVideo(const std::string& video_path) const;
        std::vector<FrameCache::CacheSegment> GetCacheSegments(const std::string& video_path) const;
        
        // Cache management
        void RemoveCacheForVideo(const std::string& video_path);
        std::vector<std::string> GetAllCachedVideoPaths() const;
        size_t GetCacheCount() const;

        // Cache generation control
        void SetCachingEnabled(bool enabled);
        bool IsCachingEnabled() const;
        void RestartCurrentVideoCache();  // Emphatic restart for current video
        // ClearCurrentVideoCache() removed - use ClearAllCaches() instead (state management only allows one video cached)

        // Metadata coordination
        void UpdateVideoMetadata(const std::string& video_path, const VideoMetadata& metadata);

        // Note: Opportunistic GPU caching removed - using window-based extraction only

        // Note: Global memory management removed - using seconds-based cache per video

    private:
        struct VideoCacheEntry {
            std::unique_ptr<FrameCache> cache;
            std::chrono::steady_clock::time_point last_accessed;
            std::string video_path;
            
            VideoCacheEntry(std::unique_ptr<FrameCache> c, const std::string& path)
                : cache(std::move(c)), video_path(path) {
                last_accessed = std::chrono::steady_clock::now();
            }
        };
        
        std::unordered_map<std::string, std::unique_ptr<VideoCacheEntry>> video_caches;
        std::list<std::string> lru_order;  // For eviction - most recent at front
        mutable std::mutex cache_mutex;
        
        std::string current_video_path;
        FrameCache::CacheConfig default_config;
        bool caching_enabled = true;

        // Internal methods
        void UpdateLRUOrder(const std::string& video_path);
        void EvictOldestCache();

        // Note: Global memory management methods removed - using seconds-based cache per video
    };

    class ProjectManager {
    public:
        enum class MetadataState {
            NOT_STARTED,    // No metadata extraction started
            LOADING_VIDEO,  // Video metadata being extracted
            VIDEO_READY,    // Video metadata ready, Adobe loading
            COMPLETE        // All metadata ready
        };

        struct CombinedMetadata {
            std::unique_ptr<VideoMetadata> video_meta;
            std::unique_ptr<AdobeMetadata> adobe_meta;
            std::unique_ptr<EXRMetadata> exr_meta;  // EXR sequence metadata
            MetadataState state = MetadataState::NOT_STARTED;
            std::chrono::steady_clock::time_point start_time;

            CombinedMetadata() = default;
            CombinedMetadata(CombinedMetadata&& other) noexcept
                : video_meta(std::move(other.video_meta)),
                adobe_meta(std::move(other.adobe_meta)),
                exr_meta(std::move(other.exr_meta)),
                state(other.state),
                start_time(other.start_time) {
            }

            CombinedMetadata& operator=(CombinedMetadata&& other) noexcept {
                if (this != &other) {
                    video_meta = std::move(other.video_meta);
                    adobe_meta = std::move(other.adobe_meta);
                    exr_meta = std::move(other.exr_meta);
                    state = other.state;
                    start_time = other.start_time;
                }
                return *this;
            }

            CombinedMetadata(const CombinedMetadata&) = delete;
            CombinedMetadata& operator=(const CombinedMetadata&) = delete;
        };

        // ========================================================================
        // CONSTRUCTION / DESTRUCTION
        // ========================================================================

        ProjectManager(VideoPlayer* player, std::string* current_file, bool* inspector_panel_flag, bool cache_preference);
        ~ProjectManager();

        // ========================================================================
        // PROJECT MANAGEMENT
        // ========================================================================

        void CreateNewProject(const std::string& name, const std::string& path);
        void CreateNewProjectFromMenu();  // Called from File menu - resets project state
        void SaveProject();
        void SaveProjectAs();  // Always shows save dialog for new location
        void LoadProject(const std::string& file_path = "");  // Empty path triggers file dialog
        void OnVideoLoaded(const std::string& file_path);

        // Menu callbacks for File menu
        void ShowNewTimelineDialog() { show_new_timeline_dialog = true; }
        void ShowNewDualViewDialog() { show_new_dual_view_dialog = true; }
        void ShowNewPlaylistDialog() { show_new_playlist_dialog = true; }
        void ImportTimelineFromMenu();
        std::string GetProjectPath() const { return current_project_path; }

        // Pending dialog flags (set by context menus, checked by main.cpp)
        bool IsPendingOpenMediaDialog() const { return pending_open_media_dialog; }
        void ClearPendingOpenMediaDialog() { pending_open_media_dialog = false; }
        bool IsPendingOpenProjectDialog() const { return pending_open_project_dialog; }
        void ClearPendingOpenProjectDialog() { pending_open_project_dialog = false; }

        // ========================================================================
        // UI RENDERING
        // ========================================================================

        void CreateProjectPanel(bool* show_project_panel);
        void CreatePropertiesSection();  // Video metadata display
        void DisplayEXRPropertiesForItem(MediaItem* item, TimelineView* timeline_view);  // For EXR in timeline mode

        // ========================================================================
        // MEDIA MANAGEMENT
        // ========================================================================

        // LoadMediaFiles() removed - use drag & drop instead
        void AddMediaFileToProject(const std::string& file_path);
        void AddCurrentVideoToProject();
        MediaItem* GetMediaItem(const std::string& media_id);

        // ========================================================================
        // SELECTION MANAGEMENT
        // ========================================================================

        void SelectMediaItem(const std::string& item_id, bool ctrl_held, bool shift_held);
        void ClearSelection();
        bool IsItemSelected(const std::string& item_id) const;
        std::vector<MediaItem> GetSelectedItems() const;
        bool HasSelectedItems() const { return !selected_media_items.empty(); }
        int GetSelectedItemsCount() const { return static_cast<int>(selected_media_items.size()); }

        // ========================================================================
        // ITEM OPERATIONS
        // ========================================================================

        void DeleteSelectedItems();
        void StartRenaming(const std::string& item_id);
        void StartRenamingSelected();
        void ShowInExplorer(const std::string& file_path);
        void ShowItemProperties(const std::string& item_id);
        void LaunchUFB(const std::string& path);  // Launch Universal File Browser with path
        void RenderPathButtons(const std::string& path, const char* id_suffix);  // Render Open/Copy/u.f.b. buttons
        void CreateTimelineFromSelection();  // Create a new timeline with selected items on V1
        void SetVideoChangeCallback(std::function<void(const std::string&)> callback) {
            video_change_callback = callback;
        }

        // Pre-video change callback - called BEFORE loading new media
        // Used to cache view state of current media before switching
        // Parameters: old_path (the path that is about to be unloaded)
        void SetPreVideoChangeCallback(std::function<void(const std::string&)> callback) {
            pre_video_change_callback = callback;
        }

        // View state callback - called when loading media to restore zoom/pan/playhead
        // Parameters: zoom_level, scroll_offset, playhead_position (from MediaItem.view_state)
        void SetViewStateCallback(std::function<void(float, float, double)> callback) {
            view_state_callback = callback;
        }

        // Color preset callback for auto 1-2-1 detection
        void SetColorPresetCallback(std::function<void(const std::string&)> callback) {
            color_preset_callback = callback;
        }

        // Auto-play request callback - called when media wants to auto-play
        // Main.cpp handles this to respect the app-wide auto_play_on_load setting
        void SetAutoPlayRequestCallback(std::function<void()> callback) {
            auto_play_request_callback = callback;
        }

        // Loading modal callback - shows "Loading..." overlay before blocking operations
        // Parameters: message to display, callback to execute after overlay is shown
        void SetLoadingModalCallback(std::function<void(const std::string&, std::function<void()>)> callback) {
            loading_modal_callback = callback;
        }

        // ========================================================================
        // MEDIA ITEM LOADING
        // ========================================================================

        void LoadSingleMediaItem(const MediaItem& item);  // Load any single media item (video, image sequence, etc.)
        void SetSkipLoadingModal(bool skip) { skip_loading_modal_ = skip; }  // Skip modal for next load (playlist mode)

        // ========================================================================
        // TIMELINE MANAGEMENT (OTIO/EDL multi-track timelines)
        // ========================================================================

        bool ImportTimeline(const std::string& file_path);  // Auto-detect format
        bool ImportOTIOFile(const std::string& file_path);
        bool ImportEDLFile(const std::string& file_path);  // Shows settings dialog
        bool ImportAAFFile(const std::string& file_path);  // AAF via Python adapter
        bool ImportXMLFile(const std::string& file_path);  // FCP/Premiere XML via Python adapter
        bool CompleteEDLImport();  // Called after user confirms EDL import settings
        void OpenTimelineInEditor(const std::string& timeline_id);
        MediaItem* GetTimelineItem(const std::string& timeline_id_or_path);  // Accepts timeline_id or file path
        MediaItem* GetCurrentTimelineItem();  // Returns current active timeline's MediaItem
        MediaItem* GetCurrentPlayingMediaItem();  // Returns MediaItem for currently playing file (works for single items and sequences)
        MediaItem* GetMediaItemFromCurrentPath();  // Find MediaItem corresponding to current_file_path
        std::string GetCurrentTimelineId() const { return current_timeline_id; }
        int GetTimelineCount() const;

        // Create new empty timeline (1 hour duration for editing room)
        std::string CreateNewTimeline(const std::string& name = "",
                                      int width = 1920, int height = 1080,
                                      double fps = 23.976, double duration = 3600.0);

        // Clip link cache management (for persistent media linking)
        void UpdateTimelineClipLinks(const std::string& timeline_id,
                                     const std::vector<MediaItem::CachedClipLink>& links);
        const std::vector<MediaItem::CachedClipLink>* GetTimelineClipLinks(const std::string& timeline_id) const;

        // Track metadata cache management (for persistent track structure)
        void UpdateTimelineTrackMetadata(const std::string& timeline_id,
                                         const std::vector<MediaItem::CachedTrackMetadata>& tracks);
        const std::vector<MediaItem::CachedTrackMetadata>* GetTimelineTrackMetadata(const std::string& timeline_id) const;

        // View state management (for persistent zoom/pan/playhead per-media)
        void CacheCurrentViewState(const std::string& media_path, float zoom, float scroll, double playhead);
        void CacheTimelineViewState(const std::string& timeline_id, float zoom, float scroll,
                                   double playhead, double in_point, double out_point);
        bool GetCachedViewState(const std::string& media_path, float& zoom, float& scroll, double& playhead);
        bool GetTimelineViewState(const std::string& timeline_id, float& zoom, float& scroll,
                                 double& playhead, double& in_point, double& out_point);

        // Timeline editor mode callback
        void SetTimelineEditorCallback(std::function<void(const std::string&)> callback) {
            timeline_editor_callback = callback;
        }

        // Exit timeline mode callback (called when loading non-timeline media)
        void SetExitTimelineModeCallback(std::function<void()> callback) {
            exit_timeline_mode_callback = callback;
        }

        // Flush timeline edits callback (called before saving project)
        // This ensures current timeline edits are captured in MediaItem.cached_tracks
        void SetFlushTimelineEditsCallback(std::function<void()> callback) {
            flush_timeline_edits_callback = callback;
        }

        // Exit comparison/dual-view mode callback (called when loading new project)
        void SetExitComparisonModeCallback(std::function<void()> callback) {
            exit_comparison_mode_callback = callback;
        }

        // Image sequence timeline callback (loads EXR/image sequences into OTIO timeline view)
        void SetImageSequenceTimelineCallback(std::function<void(MediaItem*)> callback) {
            image_sequence_timeline_callback = callback;
        }

        // Video file timeline callback (loads video files into OTIO timeline view)
        void SetVideoFileTimelineCallback(std::function<void(MediaItem*)> callback) {
            video_file_timeline_callback = callback;
        }

        // Audio file timeline callback (loads audio files into OTIO timeline view)
        void SetAudioFileTimelineCallback(std::function<void(MediaItem*)> callback) {
            audio_file_timeline_callback = callback;
        }

        // Project saved callback (called after project is successfully saved)
        void SetProjectSavedCallback(std::function<void()> callback) {
            project_saved_callback = callback;
        }

        // ========================================================================
        // DRAG & DROP OPERATIONS
        // ========================================================================

        void LoadSingleFileFromDrop(const std::string& file_path);
        void LoadMultipleFilesFromDrop(const std::vector<std::string>& file_paths);
        bool IsValidMediaFile(const std::string& file_path);

        // Image sequence handling
        bool IsPartOfImageSequence(const std::string& file_path) const;
        std::vector<std::string> DetectImageSequence(const std::string& file_path);
        void ShowFrameRateDialog(const std::string& sequence_path);
        void ProcessImageSequence(const std::string& sequence_path, double frame_rate, const std::string& exr_layer = "");
        void ProcessImageSequenceWithTranscode(const std::string& sequence_path, double frame_rate,
                                               const std::string& exr_layer, int part_index, int max_width, int compression);
        void CancelTranscode();  // Cancel ongoing EXR transcode
        bool IsInImageSequenceMode() const;
        PipelineMode GetImageSequencePipelineMode() const;  // Get auto-detected pipeline mode from frame cache
        std::string GetAnnotationPathForMedia(const std::string& media_path) const;  // Resolve actual path for annotations from mf:// or exr:// URLs
        bool HasLoadedEXRSequences() const;  // Check if any EXR sequences are loaded in project

        // ========================================================================
        // TIMELINE UTILITIES
        // ========================================================================

        double GetTimelineDuration() const;
        double GetTimelinePosition() const;
        const CombinedMetadata* GetCachedMetadata(const std::string& file_path) const;
        void CopyMetadataToEDL(const std::string& original_path, const std::string& edl_path);  // Copy metadata from original to EDL path
        void ExtractMetadataForClip(const std::string& file_path);  // Deprecated: use QueueVideoMetadataExtraction
        void QueueVideoMetadataExtraction(const std::string& file_path, bool high_priority = false);

        // ========================================================================
        // CODEC DETECTION
        // ========================================================================

        // Check if codec is inter-frame (H.264/H.265) - these codecs have issues with random-access frame decoding
        static bool IsInterFrameCodec(const std::string& codec);

        // ========================================================================
        // VIDEO CACHE MANAGEMENT
        // ========================================================================

        FrameCache* GetCurrentVideoCache() const;
        bool GetCachedFrame(double timestamp, GLuint& texture_id, int& width, int& height);
        void NotifyVideoChanged(const std::string& video_path);
        void SetCacheConfig(const FrameCache::CacheConfig& config);
        void RemoveVideoFromCache(const std::string& video_path);
        void ClearAllCaches();
        void RestartCache();  // Emphatic restart for current video
        // ClearCurrentVideoCache() removed - use ClearAllCaches() instead
        void SetCacheEnabled(bool enabled);
        void SetUserCachePreference(bool enabled);  // Update user's saved preference
        bool IsCacheEnabled() const;
        void NotifyPlaybackState(bool is_playing);  // Delegate to video cache
        FrameCache::CacheStats GetCacheStats() const;
        std::vector<FrameCache::CacheSegment> GetCacheSegments() const;

        // Note: Opportunistic GPU caching removed - using window-based extraction only

        // Note: Global memory management removed - using seconds-based cache per video

        // UI Dialog Management
        void HandleProjectDialogs();

        // ========================================================================
        // TRANSCODE QUEUE MANAGEMENT
        // ========================================================================

        void ShowTranscodeQueueWindow();
        void ToggleTranscodeQueueWindow();
        bool IsTranscodeQueueWindowOpen() const;
        void RenderTranscodeQueueWindow();  // Call from main render loop
        TranscodeQueueWindow* GetTranscodeQueueWindow() { return transcode_queue_window_.get(); }
        void AddSelectedItemsToTranscodeQueue();
        void ShowTranscodeSettingsDialog();
        TranscodeQueue* GetTranscodeQueue() { return transcode_queue_.get(); }
        TranscodeWorkerPool* GetTranscodeWorkerPool() { return transcode_worker_pool_.get(); }

        // OCIO node graph access (for extracting settings during transcode)
        void SetNodeManager(void* node_mgr) { node_manager_ptr_ = node_mgr; }
        void* GetNodeManager() const { return node_manager_ptr_; }

        // ========================================================================
        // IN/OUT POINT MANAGEMENT (Per-video)
        // ========================================================================

        void SetInPoint(double timestamp);
        void SetOutPoint(double timestamp);
        double GetInPoint() const;
        double GetOutPoint() const;
        bool HasInPoint() const;
        bool HasOutPoint() const;
        bool HasBothInOutPoints() const;
        void ClearInOutPoints();

        // ========================================================================
        // DUAL VIEW MANAGEMENT
        // ========================================================================

        // Create a new dual view (empty LEFT/RIGHT tracks)
        std::string CreateDualView(const std::string& name = "",
                                   int width = 1920, int height = 1080,
                                   double fps = 23.976);

        // Get count of dual views in project
        int GetDualViewCount() const;

        // Get dual view item by ID
        MediaItem* GetDualViewItem(const std::string& dual_view_id);

        // Get the currently active dual view (the one with is_active=true)
        MediaItem* GetActiveDualViewItem();

        // Open dual view in editor (like OpenTimelineInEditor but for dual views)
        void OpenDualViewInEditor(const std::string& dual_view_id);

        // Clear active state from all dual views (call when loading other media)
        void ClearDualViewActiveStates();

        // Dual view editor callback
        void SetDualViewEditorCallback(std::function<void(const std::string&)> callback) {
            dual_view_editor_callback = callback;
        }

        // ========================================================================
        // PLAYLIST MANAGEMENT
        // ========================================================================

        // Create a new empty playlist
        std::string CreateNewPlaylist(const std::string& name = "");

        // Create a new playlist from selected items in project
        std::string CreatePlaylistFromSelection();

        // Get playlist item by ID
        MediaItem* GetPlaylistItem(const std::string& playlist_id);

        // Get count of playlists in project
        int GetPlaylistCount() const;

        // Open playlist in panel (triggers callback to show playlist panel with this playlist)
        void OpenPlaylistInPanel(const std::string& playlist_id);

        // Playlist panel callback
        void SetPlaylistPanelCallback(std::function<void(const std::string&)> callback) {
            playlist_panel_callback = callback;
        }

    private:
        // Constants
        static const int TIMELINES_BIN_INDEX = 3;
        static const int DUAL_VIEWS_BIN_INDEX = 4;
        static const int PLAYLISTS_BIN_INDEX = 5;

        // ========================================================================
        // MEMBER VARIABLES
        // ========================================================================

        // Core references
        VideoPlayer* video_player = nullptr;
        std::string* current_file_path = nullptr;
        bool* show_inspector_panel = nullptr;
        
        // Video cache management
        std::unique_ptr<VideoCache> video_cache_manager;
        bool cache_enabled = true;                    // Current runtime cache state
        bool user_cache_preference = true;            // User's saved preference (for restoration after codec auto-disable)
        bool cache_auto_disabled_for_codec = false;   // Track if cache was auto-disabled for H.264/H.265
        std::string current_video_codec = "";         // Track current video codec for logging

        // Transcode queue management
        std::unique_ptr<TranscodeQueue> transcode_queue_;
        std::unique_ptr<TranscodeWorkerPool> transcode_worker_pool_;
        std::unique_ptr<TranscodeQueueWindow> transcode_queue_window_;

        // OCIO node manager (for extracting current OCIO settings)
        void* node_manager_ptr_ = nullptr;

        // Project data
        std::string current_project_path;
        std::vector<ProjectBin> bins;
        std::vector<MediaItem> media_pool;
        std::string current_timeline_id;  // Track which timeline is currently active in editor

        // Selection state
        std::set<std::string> selected_media_items;
        std::string last_selected_item;

        // Dialog state
        bool show_new_project_dialog = false;
        bool show_rename_dialog = false;
        char rename_buffer[256] = "";
        std::string renaming_item_id;

        // Create timeline from selection state
        bool show_create_timeline_from_selection_dialog = false;
        std::vector<MediaItem> pending_timeline_items;

        // New timeline dialog state
        bool show_new_timeline_dialog = false;
        char new_timeline_name_buffer[256] = "";
        int new_timeline_width = 1920;
        int new_timeline_height = 1080;
        double new_timeline_fps = 23.976;
        int new_timeline_resolution_preset = 0;  // 0=1080p, 1=2K, 2=4K UHD, 3=4K DCI, 4=Custom
        // Duration removed - timelines auto-extend as clips are added

        // New dual view dialog state
        bool show_new_dual_view_dialog = false;
        char new_dual_view_name_buffer[256] = "";
        int new_dual_view_width = 1920;
        int new_dual_view_height = 1080;
        double new_dual_view_fps = 23.976;
        int new_dual_view_resolution_preset = 0;  // 0=1080p, 1=4K, 2=1080x1080, 3=1080x1920, 4=Custom

        // New playlist dialog state
        bool show_new_playlist_dialog = false;
        char new_playlist_name_buffer[256] = "";

        // Pending dialog flags (triggered from context menus, handled by main.cpp)
        bool pending_open_media_dialog = false;
        bool pending_open_project_dialog = false;

        // EDL import settings dialog state
        bool show_edl_import_dialog = false;
        std::string pending_edl_path;
        int edl_import_width = 1920;
        int edl_import_height = 1080;
        double edl_import_fps = 23.976;

        // Image sequence dialog state
        bool show_frame_rate_dialog = false;
        bool frame_rate_dialog_opened = false;
        std::string pending_sequence_path;
        double selected_frame_rate = 23.976;

        // EXR layer selection state
        bool is_exr_sequence = false;
        std::vector<std::string> exr_layer_names;
        std::vector<std::string> exr_layer_display_names;
        std::vector<int> exr_layer_part_indices;  // Part index for each layer (multi-part EXR support)
        int selected_exr_layer_index = 0;
        int hidden_cryptomatte_count = 0;
        std::mutex exr_layers_mutex;  // Protects exr_layer_names, exr_layer_display_names, and exr_layer_part_indices

        // TIFF/PNG sequence detection
        bool is_tiff_png_sequence = false;

        // Single image detection
        bool is_single_image = false;

        // Pending transcode load (for deferred loading after async transcode completes)
        std::atomic<bool> pending_transcode_load{false};
        std::string pending_transcode_first_file;
        double pending_transcode_frame_rate = 23.976;
        std::mutex pending_transcode_mutex;

        // EXR transcode settings (also used for TIFF/PNG → EXR transcode)
        bool exr_transcode_enabled = false;
        int exr_transcode_max_width = 0;  // 0 = native
        int exr_transcode_compression = 4; // PIZ_COMPRESSION default

        // Transcode settings dialog state
        bool show_transcode_settings_dialog = false;
        bool transcode_settings_dialog_opened = false;
        struct TranscodeDialogSettings {
            int codec_index = 0;  // 0=H.264, 1=H.265, 2=ProRes422LT, 3=ProRes422HQ, 4=ProRes4444
            int crf = 18;
            int preset_index = 2;  // 0=ultrafast, 1=fast, 2=slow, 3=veryslow
            int priority_index = 1;  // 0=Low, 1=Normal, 2=High, 3=Urgent
            int resolution_mode = 0;  // 0=Source, 1=4K, 2=1080p, 3=Custom
            int custom_width = 1920;
            int custom_height = 1080;
            char output_directory[512] = "";
            bool use_current_ocio = true;
        } transcode_dialog_settings;

        // Metadata management
        std::unordered_map<std::string, CombinedMetadata> metadata_cache;
        std::queue<std::string> adobe_metadata_queue;
        std::unordered_set<std::string> adobe_metadata_queued;  // Deduplication: files already queued/in-progress
        std::queue<std::pair<std::string, bool>> video_metadata_queue;  // <file_path, high_priority>
        std::thread adobe_worker_thread;
        std::thread video_metadata_worker_thread;
        std::mutex queue_mutex;
        std::mutex video_queue_mutex;
        std::atomic<bool> worker_running{ false };
        std::atomic<bool> video_worker_running{ false };

        // ========================================================================
        // UI RENDERING HELPERS
        // ========================================================================

        void CreateProjectInfo();
        void CreateMediaPool();
        void CreateBinUI(ProjectBin& bin);
        void CreateMediaItemUI(const MediaItem& item);

        // Transcode dialogs
        void RenderTranscodeSettingsDialog();
        void ProcessAddToTranscodeQueue();

        // ========================================================================
        // MEDIA ITEM INTERACTION HANDLERS
        // ========================================================================

        void HandleMediaItemClick(const MediaItem& item);
        void HandleMediaItemRightClick(const MediaItem& item);
        void HandleMediaItemDragDrop(const MediaItem& item, bool is_selected);
        void ShowMediaItemContextMenu(const MediaItem& item);
        void LoadSingleMediaItemInternal(const MediaItem& item);  // Internal: actual loading logic
        void OpenTimelineInEditorInternal(const std::string& timeline_id);  // Internal: actual timeline loading

        // ========================================================================
        // ITEM OPERATION HELPERS
        // ========================================================================

        void ProcessRenameItem();
        void ProcessCreateTimelineFromSelection();  // Process the timeline creation dialog
        void SelectItemRange(const std::string& start_id, const std::string& end_id);

        // ========================================================================
        // METADATA PROCESSING
        // ========================================================================

        void StartAdobeWorkerThread();
        void StopAdobeWorkerThread();
        void AdobeWorkerLoop();
        void ProcessAdobeMetadata(const std::string& file_path);
        void QueueAdobeMetadata(const std::string& file_path);

        // Video metadata background processing
        void StartVideoMetadataWorkerThread();
        void StopVideoMetadataWorkerThread();
        void VideoMetadataWorkerLoop();
        void ProcessVideoMetadata(const std::string& file_path);

        // Metadata filtering
        bool ShouldSkipAdobeMetadataExtraction(const std::string& file_path);

        // ========================================================================
        // METADATA DISPLAY HELPERS
        // ========================================================================

        void DisplayVideoMetadata(const VideoMetadata* video_meta);
        void DisplayAdobeMetadata(const AdobeMetadata* adobe_meta);
        void DisplayFileInfoTable(const VideoMetadata* video_meta);
        void DisplayVideoPropertiesTable(const VideoMetadata* video_meta);
        void DisplayColorPropertiesTable(const VideoMetadata* video_meta);
        void DisplayAudioPropertiesTable(const VideoMetadata* video_meta);
        void DisplayAdobeProjectsTable(const AdobeMetadata* adobe_meta);
        void DisplayAdobeProjectRow(const std::string& app_name, const std::string& project_path, const std::string& button_suffix);
        void DisplayTimecodeTable(const AdobeMetadata* adobe_meta);

        // EXR sequence metadata display
        void DisplayEXRMetadata(const EXRMetadata* exr_meta);
        void DisplayEXRFileInfoTable(const EXRMetadata* exr_meta);
        void DisplayEXRImagePropertiesTable(const EXRMetadata* exr_meta);
        void DisplayEXRChannelsTable(const EXRMetadata* exr_meta);  // Displays layers (RGB/RGBA groupings)

        // ========================================================================
        // UTILITY HELPERS
        // ========================================================================

        void CreateNewBin(const std::string& name = "");
        std::string GenerateUniqueID();
        void UpdateIDCounter();  // Update counter after loading project to avoid duplicate IDs
        std::string GetProjectName(const std::string& path);
        std::string GetFileName(const std::string& path);
        MediaType GetMediaType(const std::string& path) const;
        int GetBinIndexForMediaType(MediaType type);
        double GetDefaultDurationForType(MediaType type);
        std::vector<std::string> ParsePayloadString(const std::string& payload_string);
        bool HasColorInfo(const VideoMetadata* video_meta);
        bool HasAudioInfo(const VideoMetadata* video_meta);
        bool IsAudioOnlyFile(const VideoMetadata* video_meta);
        void OpenFileInExplorer(const std::string& file_path);
        void CopyToClipboard(const std::string& text);
        std::function<void(const std::string&)> video_change_callback;
        std::function<void(const std::string&)> pre_video_change_callback;  // Callback before video change (for caching view state)
        std::function<void(float, float, double)> view_state_callback;  // Callback for view state (zoom, scroll, playhead)
        std::function<void(const std::string&)> color_preset_callback;  // Callback for auto 1-2-1 OCIO preset
        std::function<void(const std::string&)> timeline_editor_callback;  // Callback to open timeline in editor
        std::function<void()> exit_timeline_mode_callback;  // Callback to exit timeline editor mode
        std::function<void()> flush_timeline_edits_callback;  // Callback to flush current timeline edits before save
        std::function<void()> exit_comparison_mode_callback;  // Callback to exit comparison/dual-view mode
        std::function<void()> auto_play_request_callback;  // Callback to request auto-play (respects app settings)
        std::function<void(const std::string&, std::function<void()>)> loading_modal_callback;  // Callback to show loading modal
        std::function<void(MediaItem*)> image_sequence_timeline_callback;  // Callback to load image sequences into OTIO timeline view
        std::function<void(MediaItem*)> video_file_timeline_callback;  // Callback to load video files into OTIO timeline view
        std::function<void(MediaItem*)> audio_file_timeline_callback;  // Callback to load audio files into OTIO timeline view
        std::function<void()> project_saved_callback;  // Callback when project is successfully saved
        std::function<void(const std::string&)> dual_view_editor_callback;  // Callback to open dual view in editor
        std::function<void(const std::string&)> playlist_panel_callback;  // Callback to open playlist in panel

        // Playlist mode flag - skip loading modal for smoother transitions
        bool skip_loading_modal_ = false;

        // Cloud sync helper - waits for file to become readable
        bool WaitForFileReadable(const std::string& file_path, int timeout_seconds = 30);

    };

} 