#include "playlist_controller.h"
#include "../project/project_manager.h"
#include "../ui/playlist_panel.h"
#include "../utils/debug_utils.h"

namespace ump {

PlaylistController::PlaylistController() = default;
PlaylistController::~PlaylistController() = default;

void PlaylistController::StartPlaylist(const std::string& playlist_id, int start_index) {
    if (!project_manager_) {
        Debug::Log("PlaylistController: Cannot start - no project manager");
        return;
    }

    MediaItem* playlist = project_manager_->GetPlaylistItem(playlist_id);
    if (!playlist) {
        Debug::Log("PlaylistController: Playlist not found: " + playlist_id);
        return;
    }

    if (playlist->playlist_items.empty()) {
        Debug::Log("PlaylistController: Playlist is empty: " + playlist->name);
        if (warning_callback_) {
            warning_callback_("Playlist is empty");
        }
        return;
    }

    // Activate playlist
    is_active_ = true;
    active_playlist_id_ = playlist_id;
    current_index_ = start_index;

    // Mark playlist as active in project (for UI highlighting)
    playlist->is_active = true;

    // Update panel state
    UpdatePanelState();

    Debug::Log("PlaylistController: Started playlist '" + playlist->name + "' at index " + std::to_string(start_index));

    // Load and play the first item
    LoadAndPlayItem(current_index_);
}

void PlaylistController::StopPlaylist() {
    if (!is_active_) return;

    MediaItem* playlist = GetActivePlaylist();
    if (playlist) {
        playlist->is_active = false;
    }

    is_active_ = false;
    active_playlist_id_.clear();
    current_index_ = 0;

    // Update panel state
    UpdatePanelState();

    Debug::Log("PlaylistController: Stopped playlist");

    if (playlist_finished_callback_) {
        playlist_finished_callback_();
    }
}

void PlaylistController::PlayItem(int index) {
    if (!is_active_) return;

    MediaItem* playlist = GetActivePlaylist();
    if (!playlist) return;

    if (index < 0 || index >= static_cast<int>(playlist->playlist_items.size())) {
        Debug::Log("PlaylistController: Invalid index: " + std::to_string(index));
        return;
    }

    current_index_ = index;
    UpdatePanelState();
    LoadAndPlayItem(index);
}

void PlaylistController::PlayNext() {
    if (!is_active_) return;

    MediaItem* playlist = GetActivePlaylist();
    if (!playlist) return;

    int next_index = FindNextValidItem(current_index_ + 1);

    Debug::Log("PlaylistController::PlayNext - current: " + std::to_string(current_index_) +
               ", next_valid: " + std::to_string(next_index) +
               ", loop: " + std::string(playlist->playlist_loop ? "ON" : "OFF") +
               ", total: " + std::to_string(playlist->playlist_items.size()));

    if (next_index >= 0) {
        current_index_ = next_index;
        UpdatePanelState();
        LoadAndPlayItem(next_index);
    } else if (playlist->playlist_loop) {
        // Loop back to beginning
        next_index = FindNextValidItem(0);
        if (next_index >= 0) {
            current_index_ = next_index;
            UpdatePanelState();
            LoadAndPlayItem(next_index);
            Debug::Log("PlaylistController: Looped back to beginning");
        } else {
            // All items missing
            Debug::Log("PlaylistController: All items unavailable");
            if (warning_callback_) {
                warning_callback_("All playlist items are unavailable");
            }
            StopPlaylist();
        }
    } else {
        // End of playlist - loop is OFF
        Debug::Log("PlaylistController: Reached end of playlist (loop OFF)");
        StopPlaylist();
    }
}

void PlaylistController::PlayPrevious() {
    if (!is_active_) return;

    int prev_index = FindPreviousValidItem(current_index_ - 1);

    if (prev_index >= 0) {
        current_index_ = prev_index;
        UpdatePanelState();
        LoadAndPlayItem(prev_index);
    } else {
        Debug::Log("PlaylistController: Already at beginning of playlist");
    }
}

void PlaylistController::OnItemEnded() {
    if (!is_active_) return;

    Debug::Log("PlaylistController: Item ended at index " + std::to_string(current_index_));

    // Pause current playback
    if (pause_callback_) {
        pause_callback_();
    }

    // Advance to next item
    PlayNext();
}

double PlaylistController::GetCurrentInPoint() const {
    if (!is_active_) return -1.0;

    MediaItem* playlist = const_cast<PlaylistController*>(this)->GetActivePlaylist();
    if (!playlist) return -1.0;

    if (current_index_ < 0 || current_index_ >= static_cast<int>(playlist->playlist_items.size())) {
        return -1.0;
    }

    const auto& entry = playlist->playlist_items[current_index_];

    // If override is set, use it; otherwise use default
    if (entry.in_point >= 0) {
        return entry.in_point;
    }

    // Check if the media item has its own in point
    if (project_manager_) {
        MediaItem* media = project_manager_->GetMediaItem(entry.media_id);
        if (media && media->in_point >= 0) {
            return media->in_point;
        }
    }

    return -1.0;  // No in point set
}

double PlaylistController::GetCurrentOutPoint() const {
    if (!is_active_) return -1.0;

    MediaItem* playlist = const_cast<PlaylistController*>(this)->GetActivePlaylist();
    if (!playlist) return -1.0;

    if (current_index_ < 0 || current_index_ >= static_cast<int>(playlist->playlist_items.size())) {
        return -1.0;
    }

    const auto& entry = playlist->playlist_items[current_index_];

    // If override is set, use it; otherwise use default
    if (entry.out_point >= 0) {
        return entry.out_point;
    }

    // Check if the media item has its own out point
    if (project_manager_) {
        MediaItem* media = project_manager_->GetMediaItem(entry.media_id);
        if (media && media->out_point >= 0) {
            return media->out_point;
        }
    }

    return -1.0;  // No out point set
}

bool PlaylistController::HasNextItem() const {
    if (!is_active_) return false;

    MediaItem* playlist = const_cast<PlaylistController*>(this)->GetActivePlaylist();
    if (!playlist) return false;

    int next = const_cast<PlaylistController*>(this)->FindNextValidItem(current_index_ + 1);
    return next >= 0 || playlist->playlist_loop;
}

bool PlaylistController::HasPreviousItem() const {
    if (!is_active_) return false;

    int prev = const_cast<PlaylistController*>(this)->FindPreviousValidItem(current_index_ - 1);
    return prev >= 0;
}

MediaItem* PlaylistController::GetActivePlaylist() {
    if (active_playlist_id_.empty() || !project_manager_) {
        return nullptr;
    }
    return project_manager_->GetPlaylistItem(active_playlist_id_);
}

void PlaylistController::LoadAndPlayItem(int index) {
    MediaItem* playlist = GetActivePlaylist();
    if (!playlist) return;

    if (index < 0 || index >= static_cast<int>(playlist->playlist_items.size())) {
        Debug::Log("PlaylistController: Invalid item index: " + std::to_string(index));
        return;
    }

    const auto& entry = playlist->playlist_items[index];

    // Get the media item
    if (!project_manager_) return;

    MediaItem* media_item = project_manager_->GetMediaItem(entry.media_id);
    if (!media_item) {
        Debug::Log("PlaylistController: Media item not found: " + entry.media_id);
        if (warning_callback_) {
            warning_callback_("Skipping missing item");
        }
        // Skip to next
        PlayNext();
        return;
    }

    Debug::Log("PlaylistController: Loading item " + std::to_string(index + 1) + "/" +
               std::to_string(playlist->playlist_items.size()) + ": " + media_item->name);

    // Load the media - this is async; TriggerAutoPlay() in main.cpp handles
    // buffer wait and auto-play after the media finishes loading
    if (load_media_callback_) {
        load_media_callback_(*media_item);
    }
}

int PlaylistController::FindNextValidItem(int from_index) {
    MediaItem* playlist = GetActivePlaylist();
    if (!playlist || !project_manager_) return -1;

    int size = static_cast<int>(playlist->playlist_items.size());

    for (int i = from_index; i < size; i++) {
        const auto& entry = playlist->playlist_items[i];
        MediaItem* media = project_manager_->GetMediaItem(entry.media_id);
        if (media) {
            return i;  // Found valid item
        }
        Debug::Log("PlaylistController: Skipping missing item at index " + std::to_string(i));
    }

    return -1;  // No valid items found
}

int PlaylistController::FindPreviousValidItem(int from_index) {
    MediaItem* playlist = GetActivePlaylist();
    if (!playlist || !project_manager_) return -1;

    for (int i = from_index; i >= 0; i--) {
        const auto& entry = playlist->playlist_items[i];
        MediaItem* media = project_manager_->GetMediaItem(entry.media_id);
        if (media) {
            return i;  // Found valid item
        }
        Debug::Log("PlaylistController: Skipping missing item at index " + std::to_string(i));
    }

    return -1;  // No valid items found
}

void PlaylistController::UpdatePanelState() {
    if (playlist_panel_) {
        playlist_panel_->SetActive(is_active_);
        playlist_panel_->SetCurrentIndex(current_index_);
    }
}

void PlaylistController::OnItemMoved(int from_index, int to_index) {
    if (!is_active_) return;

    // Adjust current_index_ based on the move
    if (current_index_ == from_index) {
        // The currently playing item was moved
        current_index_ = to_index;
    } else if (from_index < current_index_ && to_index >= current_index_) {
        // Item moved from before current to after current
        current_index_--;
    } else if (from_index > current_index_ && to_index <= current_index_) {
        // Item moved from after current to before current
        current_index_++;
    }

    Debug::Log("PlaylistController::OnItemMoved - from: " + std::to_string(from_index) +
               ", to: " + std::to_string(to_index) + ", current now: " + std::to_string(current_index_));
}

void PlaylistController::OnItemDeleted(int deleted_index) {
    if (!is_active_) return;

    MediaItem* playlist = GetActivePlaylist();
    if (!playlist) return;

    int item_count = static_cast<int>(playlist->playlist_items.size());

    if (deleted_index < current_index_) {
        // Deleted item was before current - shift index down
        current_index_--;
    } else if (deleted_index == current_index_) {
        // Currently playing item was deleted - try to play next valid item
        if (item_count > 0) {
            // Find next valid item from current position
            int next_index = FindNextValidItem(current_index_);
            if (next_index < 0) {
                // No items after, try from beginning
                next_index = FindNextValidItem(0);
            }
            if (next_index >= 0) {
                current_index_ = next_index;
                LoadAndPlayItem(current_index_);
            } else {
                // No valid items left
                StopPlaylist();
            }
        } else {
            // Playlist is now empty
            StopPlaylist();
        }
    }
    // If deleted_index > current_index_, no adjustment needed

    Debug::Log("PlaylistController::OnItemDeleted - index: " + std::to_string(deleted_index) +
               ", current now: " + std::to_string(current_index_));
}

} // namespace ump
