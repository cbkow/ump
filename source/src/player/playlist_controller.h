#pragma once

#include "../project/media_item.h"
#include <string>
#include <functional>

namespace ump {

class ProjectManager;
class PlaylistPanel;

/**
 * PlaylistController - Controls playlist playback state and transitions
 *
 * Responsibilities:
 * - Track active playlist and current index
 * - Handle Play/Pause/Stop/PlayNext/PlayPrevious
 * - OnItemEnded() - called when playback reaches end boundary
 * - Force auto-play ON regardless of user setting
 * - Override loop behavior (play to end, then advance)
 * - Skip missing items with warning
 */
class PlaylistController {
public:
    PlaylistController();
    ~PlaylistController();

    // Set dependencies
    void SetProjectManager(ProjectManager* manager) { project_manager_ = manager; }
    void SetPlaylistPanel(PlaylistPanel* panel) { playlist_panel_ = panel; }

    // Playlist activation
    void StartPlaylist(const std::string& playlist_id, int start_index = 0);
    void StopPlaylist();
    bool IsActive() const { return is_active_; }
    std::string GetActivePlaylistId() const { return active_playlist_id_; }

    // Playback control
    void PlayItem(int index);
    void PlayNext();
    void PlayPrevious();
    void SetCurrentIndex(int index) { current_index_ = index; }
    int GetCurrentIndex() const { return current_index_; }

    // Called by panel when items are reordered/deleted
    void OnItemMoved(int from_index, int to_index);
    void OnItemDeleted(int deleted_index);

    // Called when current item reaches end boundary
    // This is the main integration point with main.cpp playback loop
    void OnItemEnded();

    // Callbacks for media loading
    using LoadMediaCallback = std::function<void(const MediaItem& item)>;
    void SetLoadMediaCallback(LoadMediaCallback callback) { load_media_callback_ = callback; }

    // Callback for triggering buffer wait after loading
    using BufferWaitCallback = std::function<void()>;
    void SetBufferWaitCallback(BufferWaitCallback callback) { buffer_wait_callback_ = callback; }

    // Callback for pausing playback
    using PauseCallback = std::function<void()>;
    void SetPauseCallback(PauseCallback callback) { pause_callback_ = callback; }

    // Callback for auto-play (forced ON during playlist)
    using AutoPlayCallback = std::function<void()>;
    void SetAutoPlayCallback(AutoPlayCallback callback) { auto_play_callback_ = callback; }

    // Callback for showing warning (e.g., missing items)
    using WarningCallback = std::function<void(const std::string& message)>;
    void SetWarningCallback(WarningCallback callback) { warning_callback_ = callback; }

    // Callback when playlist finishes
    using PlaylistFinishedCallback = std::function<void()>;
    void SetPlaylistFinishedCallback(PlaylistFinishedCallback callback) { playlist_finished_callback_ = callback; }

    // Get in/out points for current item (respecting overrides)
    double GetCurrentInPoint() const;
    double GetCurrentOutPoint() const;

    // Check if there are more items after current
    bool HasNextItem() const;
    bool HasPreviousItem() const;

private:
    ProjectManager* project_manager_ = nullptr;
    PlaylistPanel* playlist_panel_ = nullptr;

    bool is_active_ = false;
    std::string active_playlist_id_;
    int current_index_ = 0;

    // Callbacks
    LoadMediaCallback load_media_callback_;
    BufferWaitCallback buffer_wait_callback_;
    PauseCallback pause_callback_;
    AutoPlayCallback auto_play_callback_;
    WarningCallback warning_callback_;
    PlaylistFinishedCallback playlist_finished_callback_;

    // Get the active playlist MediaItem
    MediaItem* GetActivePlaylist();

    // Load and play item at index
    void LoadAndPlayItem(int index);

    // Find next valid (non-missing) item
    int FindNextValidItem(int from_index);
    int FindPreviousValidItem(int from_index);

    // Update panel state
    void UpdatePanelState();
};

} // namespace ump
