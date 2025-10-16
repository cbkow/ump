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
#include <vector> 

#include "media_item.h"
#include "../metadata/adobe_metadata.h"
#include "../metadata/video_metadata.h"
#include "../metadata/exr_metadata.h"
#include "../player/frame_cache.h"
#include "../player/image_sequence_config.h"
#include "../utils/exr_layer_detector.h"

// Forward declarations
class VideoPlayer;

namespace ump {
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
        struct SequencePosition {
            int clip_index = -1;
            double clip_position = 0.0;
            std::string clip_path;
        };

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
        void SaveProject();
        void LoadProject(const std::string& file_path = "");  // Empty path triggers file dialog
        void OnVideoLoaded(const std::string& file_path);
        std::string GetProjectPath() const { return current_project_path; }

        // ========================================================================
        // UI RENDERING
        // ========================================================================

        void CreateProjectPanel(bool* show_project_panel);
        void CreatePropertiesSection();

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
        void CreatePlaylistFromSelection();
        void SetVideoChangeCallback(std::function<void(const std::string&)> callback) {
            video_change_callback = callback;
        }

        // Color preset callback for auto 1-2-1 detection
        void SetColorPresetCallback(std::function<void(const std::string&)> callback) {
            color_preset_callback = callback;
        }

        // ========================================================================
        // SEQUENCE MANAGEMENT
        // ========================================================================

        void CreateNewSequence(const std::string& name = "");
        void AddSequenceToProject(const std::string& sequence_id);
        void LoadSequenceFromBin(const std::string& media_item_id);
        void SwitchToSequence(const std::string& sequence_id);
        void ClearSequenceMode();
        bool IsSequenceMode() const;
        Sequence* GetCurrentSequence() const;
        bool IsInSequenceMode() const { return IsSequenceMode(); }

        // ========================================================================
        // PLAYLIST OPERATIONS
        // ========================================================================

        void AddToPlaylist(const std::string& media_id);
        void AddMultipleToPlaylist(const std::string& payload_string);
        void ClearCurrentPlaylist();
        void RemoveFromPlaylist(int index);
        void RebuildPlaylistInMPV();
        void ReloadCurrentPlaylist();
        void RemoveDuplicatesFromPlaylist();

        // ========================================================================
        // PLAYLIST PLAYBACK CONTROL
        // ========================================================================

        int GetCurrentPlaylistIndex() const;
        int GetPlaylistLength() const;
        std::string GetCurrentClipName() const;
        void GoToNextInPlaylist();
        void GoToPreviousInPlaylist();
        void JumpToPlaylistIndex(int index);
        void SyncPlaylistPosition();

        // ========================================================================
        // PLAYLIST SELECTION MANAGEMENT
        // ========================================================================

        void SelectPlaylistItem(int index, bool ctrl_held, bool shift_held);
        void ClearPlaylistSelection();
        bool IsPlaylistItemSelected(int index) const;
        int GetSelectedPlaylistItemsCount() const;
        void DeleteSelectedPlaylistItems();
        void MoveSelectedPlaylistItemsUp();
        void MoveSelectedPlaylistItemsDown();
        void MovePlaylistItem(int from_index, int to_index);

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
                                               const std::string& exr_layer, int max_width, int compression);
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
        SequencePosition CalculateSequencePosition(double global_position) const;
        const CombinedMetadata* GetCachedMetadata(const std::string& file_path) const;
        void ExtractMetadataForClip(const std::string& file_path);  // Deprecated: use QueueVideoMetadataExtraction
        void QueueVideoMetadataExtraction(const std::string& file_path, bool high_priority = false);

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

    private:
        // Constants
        static const int SEQUENCES_BIN_INDEX = 3;

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

        // Project data
        std::string current_project_path;
        std::vector<ProjectBin> bins;
        std::vector<MediaItem> media_pool;
        std::vector<Sequence> sequences;
        std::string current_sequence_id;

        // Selection state
        std::set<std::string> selected_media_items;
        std::string last_selected_item;
        std::set<int> selected_playlist_indices;
        int last_selected_playlist_index = -1;
        mutable int cached_playlist_position = -1;

        // Dialog state
        bool show_new_project_dialog = false;
        bool show_new_sequence_dialog = false;
        bool show_create_playlist_dialog = false;
        bool show_rename_dialog = false;
        char rename_buffer[256] = "";
        char new_playlist_name_buffer[256] = "";
        std::string renaming_item_id;
        std::vector<MediaItem> pending_playlist_items;

        // Image sequence dialog state
        bool show_frame_rate_dialog = false;
        bool frame_rate_dialog_opened = false;
        std::string pending_sequence_path;
        double selected_frame_rate = 24.0;

        // EXR layer selection state
        bool is_exr_sequence = false;
        std::vector<std::string> exr_layer_names;
        std::vector<std::string> exr_layer_display_names;
        int selected_exr_layer_index = 0;
        int hidden_cryptomatte_count = 0;
        std::mutex exr_layers_mutex;  // Protects exr_layer_names and exr_layer_display_names

        // TIFF/PNG sequence detection
        bool is_tiff_png_sequence = false;

        // EXR transcode settings (also used for TIFF/PNG → EXR transcode)
        bool exr_transcode_enabled = false;
        int exr_transcode_max_width = 0;  // 0 = native
        int exr_transcode_compression = 4; // PIZ_COMPRESSION default

        // Metadata management
        std::unordered_map<std::string, CombinedMetadata> metadata_cache;
        std::queue<std::string> adobe_metadata_queue;
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

        void CreateProjectToolbar();
        void CreateProjectInfo();
        void CreateMediaPool();
        void CreateBinUI(ProjectBin& bin);
        void CreateMediaItemUI(const MediaItem& item);
        void CreateSequencesBinToolbar();

        // ========================================================================
        // MEDIA ITEM INTERACTION HANDLERS
        // ========================================================================

        void HandleMediaItemClick(const MediaItem& item);
        void HandleMediaItemRightClick(const MediaItem& item);
        void HandleMediaItemDragDrop(const MediaItem& item, bool is_selected);
        void ShowMediaItemContextMenu(const MediaItem& item);
        void LoadSingleMediaItem(const MediaItem& item);

        // ========================================================================
        // ITEM OPERATION HELPERS
        // ========================================================================

        void ProcessRenameItem();
        void ProcessCreatePlaylistFromSelection();
        void SelectItemRange(const std::string& start_id, const std::string& end_id);

        // ========================================================================
        // SEQUENCE MANAGEMENT HELPERS
        // ========================================================================

        void LoadSequenceIntoPlayer(const Sequence& sequence, bool auto_play = true);
        void UpdateSequenceActiveStates(const std::string& active_sequence_id);
        void UpdateSequenceInBin(const std::string& sequence_id);
        MediaItem CreateSequenceMediaItem(const Sequence& sequence);
        Sequence* GetOrCreateCurrentSequence();

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
        std::function<void(const std::string&)> color_preset_callback;  // Callback for auto 1-2-1 OCIO preset

        // Cloud sync helper - waits for file to become readable
        bool WaitForFileReadable(const std::string& file_path, int timeout_seconds = 30);

        // ========================================================================
        // OBSOLETE METHODS (TO BE REMOVED)
        // ========================================================================

        // Note: This method is redundant with the simplified playlist approach
        // Consider removing after confirming no external dependencies
        std::string GenerateEDLFromSequence(const Sequence& sequence);
    };

} 