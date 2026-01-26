#pragma once

#include "../project/media_item.h"
#include <string>
#include <functional>
#include <vector>
#include <imgui.h>

namespace ump {

class ProjectManager;  // Forward declaration
class PlaylistController;  // Forward declaration

/**
 * PlaylistPanel - UI panel for displaying and controlling playlist playback
 *
 * Features:
 * - Displays ordered list of playable media items
 * - Drag-and-drop reordering between items
 * - Dropzone at bottom for adding new items
 * - Click to jump and play any item
 * - Shows "Playing" badge when playlist is active
 */
class PlaylistPanel {
public:
    PlaylistPanel();
    ~PlaylistPanel();

    // Render the panel
    void Render(bool* p_open, ImVec4 accent_regular = ImVec4(0.3f, 0.6f, 1.0f, 1.0f));

    // Set project manager for accessing media items
    void SetProjectManager(ProjectManager* manager) { project_manager_ = manager; }

    // Set playlist controller for sync during playback
    void SetPlaylistController(PlaylistController* controller) { playlist_controller_ = controller; }

    // Load a playlist by ID (called when user double-clicks playlist in Project)
    void LoadPlaylist(const std::string& playlist_id);

    // Get currently loaded playlist ID
    std::string GetLoadedPlaylistId() const { return loaded_playlist_id_; }

    // Check if a playlist is loaded
    bool HasLoadedPlaylist() const { return !loaded_playlist_id_.empty(); }

    // Get current playlist item (for display in transport bar)
    MediaItem* GetCurrentPlaylistItem();

    // Get current index (1-based for display)
    int GetCurrentDisplayIndex() const;

    // Get total item count
    int GetTotalItemCount() const;

    // Callbacks for playback control
    using PlayItemCallback = std::function<void(const std::string& media_id, int playlist_index)>;
    void SetPlayItemCallback(PlayItemCallback callback) { play_item_callback_ = callback; }

    using PlayNextCallback = std::function<void()>;
    void SetPlayNextCallback(PlayNextCallback callback) { play_next_callback_ = callback; }

    using PlayPreviousCallback = std::function<void()>;
    void SetPlayPreviousCallback(PlayPreviousCallback callback) { play_previous_callback_ = callback; }

    // Callback when playlist is modified (items added/removed/reordered)
    using PlaylistModifiedCallback = std::function<void()>;
    void SetPlaylistModifiedCallback(PlaylistModifiedCallback callback) { playlist_modified_callback_ = callback; }

    // Active state (set by playlist controller)
    void SetActive(bool active) { is_active_ = active; }
    bool IsActive() const { return is_active_; }

    // Current playing index (set by playlist controller)
    void SetCurrentIndex(int index) { current_index_ = index; }
    int GetCurrentIndex() const { return current_index_; }

    // Loop state
    void SetLooping(bool loop);
    bool IsLooping() const;

    // Clear the loaded playlist
    void Clear();

    // Add items to playlist (from drag-drop)
    void AddItems(const std::vector<std::string>& media_ids);

    // Remove item at index
    void RemoveItem(int index);

    // Move item from one index to another
    void MoveItem(int from_index, int to_index);

private:
    ProjectManager* project_manager_ = nullptr;
    PlaylistController* playlist_controller_ = nullptr;
    std::string loaded_playlist_id_;
    bool is_active_ = false;
    int current_index_ = 0;

    // Drag-drop state for reordering
    int drag_source_index_ = -1;
    int drag_target_index_ = -1;

    // Callbacks
    PlayItemCallback play_item_callback_;
    PlayNextCallback play_next_callback_;
    PlayPreviousCallback play_previous_callback_;
    PlaylistModifiedCallback playlist_modified_callback_;

    // UI helpers
    void RenderHeader(bool* p_open, ImVec4 accent_color);
    void RenderEmptyState();
    void RenderItemsList(ImVec4 accent_color);
    void RenderItem(int index, const PlaylistItemEntry& entry, MediaItem* media_item, ImVec4 accent_color);
    void RenderControls(ImVec4 accent_color);

    // Get the loaded playlist MediaItem
    MediaItem* GetLoadedPlaylist();

    // Handle drop payload (from Project panel)
    void HandleDropPayload(const char* payload);

    // Sync changes back to project
    void SyncToProject();
};

} // namespace ump
