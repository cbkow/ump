#include "playlist_panel.h"
#include "../project/project_manager.h"
#include "../player/playlist_controller.h"
#include "../utils/debug_utils.h"
#include <algorithm>

// Icons
#define ICON_LIST       u8"\uE896"
#define ICON_CLOSE      u8"\uE5CD"

extern ImFont* font_icons;

namespace ump {

PlaylistPanel::PlaylistPanel() = default;
PlaylistPanel::~PlaylistPanel() = default;

void PlaylistPanel::Render(bool* p_open, ImVec4 accent_color) {
    if (!p_open || !*p_open) return;

    // Panel styling - match Project Manager background (#121212)
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // Transparent border
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.07f, 1.0f));  // #121212 background

    if (ImGui::Begin("Playlist", p_open)) {
        RenderHeader(p_open, accent_color);

        ImGui::Separator();

        if (!HasLoadedPlaylist()) {
            RenderEmptyState();
        } else {
            RenderItemsList(accent_color);
            ImGui::Spacing();
            RenderControls(accent_color);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
}

void PlaylistPanel::RenderHeader(bool* p_open, ImVec4 accent_color) {
    // Header with icon and title (gray text like Project Manager)
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    if (font_icons) {
        ImGui::PushFont(font_icons);
        ImGui::Text(ICON_LIST);
        ImGui::PopFont();
        ImGui::SameLine();
    }
    ImGui::Text("Playlist");
    ImGui::PopStyleColor();

    // Show "Playing" badge when active
    MediaItem* playlist = GetLoadedPlaylist();
    if (is_active_ && playlist) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, accent_color);
        ImGui::Text("[PLAYING %d/%d]", current_index_ + 1, static_cast<int>(playlist->playlist_items.size()));
        ImGui::PopStyleColor();
    }

    // Close button on the right
    float button_size = ImGui::GetFontSize() + 4.0f;  // Compact size
    ImGui::SameLine(ImGui::GetWindowWidth() - button_size - ImGui::GetStyle().WindowPadding.x);
    ImVec2 button_pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##ClosePlaylist", ImVec2(button_size, button_size));
    bool hovered = ImGui::IsItemHovered();
    // Draw icon centered - disabled color by default, regular on hover
    if (font_icons) {
        ImGui::PushFont(font_icons);
        ImVec2 icon_size = ImGui::CalcTextSize(ICON_CLOSE);
        ImVec2 icon_pos = ImVec2(button_pos.x + (button_size - icon_size.x) / 2,
                                 button_pos.y + (button_size - icon_size.y) / 2 - 1.0f);
        ImU32 icon_col = hovered ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImGui::GetWindowDrawList()->AddText(icon_pos, icon_col, ICON_CLOSE);
        ImGui::PopFont();
    }
    if (clicked && p_open) {
        *p_open = false;
    }
    if (hovered) {
        ImGui::SetTooltip("Close Playlist (Ctrl+7)");
    }
}

void PlaylistPanel::RenderEmptyState() {
    ImGui::Spacing();
    ImGui::Spacing();

    // Center the text
    float window_width = ImGui::GetContentRegionAvail().x;
    const char* text1 = "No playlist loaded.";
    const char* text2 = "Create or open a playlist from the";
    const char* text3 = "Project panel to get started.";

    float text1_width = ImGui::CalcTextSize(text1).x;
    float text2_width = ImGui::CalcTextSize(text2).x;
    float text3_width = ImGui::CalcTextSize(text3).x;

    ImGui::SetCursorPosX((window_width - text1_width) * 0.5f);
    ImGui::TextDisabled("%s", text1);

    ImGui::Spacing();

    ImGui::SetCursorPosX((window_width - text2_width) * 0.5f);
    ImGui::TextDisabled("%s", text2);

    ImGui::SetCursorPosX((window_width - text3_width) * 0.5f);
    ImGui::TextDisabled("%s", text3);

    ImGui::Spacing();
    ImGui::Spacing();
}

void PlaylistPanel::RenderItemsList(ImVec4 accent_color) {
    MediaItem* playlist = GetLoadedPlaylist();
    if (!playlist) return;

    // Reserve space for controls at bottom
    float item_spacing = ImGui::GetStyle().ItemSpacing.y;
    float separator_height = 1.0f;
    float frame_height = ImGui::GetFrameHeightWithSpacing();
    float controls_height = item_spacing + separator_height + item_spacing + frame_height + 6.0f;
    float available_height = ImGui::GetContentRegionAvail().y - controls_height;
    if (available_height < 100.0f) available_height = 100.0f;

    float list_width = ImGui::GetContentRegionAvail().x;

    // Single child window containing items + drop zone
    ImVec2 child_pos = ImGui::GetCursorScreenPos();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.07f, 1.0f));
    ImGui::BeginChild("PlaylistItems", ImVec2(list_width, available_height), false);

    // Render items
    for (int i = 0; i < static_cast<int>(playlist->playlist_items.size()); i++) {
        const auto& entry = playlist->playlist_items[i];
        MediaItem* media_item = project_manager_ ? project_manager_->GetMediaItem(entry.media_id) : nullptr;
        RenderItem(i, entry, media_item, accent_color);
    }

    // Drop zone button fills remaining space inside child
    ImVec2 remaining = ImGui::GetContentRegionAvail();
    float drop_height = std::max(remaining.y, 40.0f);
    float drop_width = ImGui::GetContentRegionAvail().x;
    ImVec2 drop_zone_pos = ImGui::GetCursorScreenPos();

    // Invisible button for drop zone
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    ImGui::Button("##DropZone", ImVec2(drop_width, drop_height));

    ImGui::PopStyleColor(3);

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            const char* media_id = static_cast<const char*>(payload->Data);
            HandleDropPayload(media_id);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEMS_MULTI")) {
            const char* payload_str = static_cast<const char*>(payload->Data);
            HandleDropPayload(payload_str);
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void PlaylistPanel::RenderItem(int index, const PlaylistItemEntry& entry, MediaItem* media_item, ImVec4 accent_color) {
    ImGui::PushID(index);

    bool is_current = (index == current_index_) && is_active_;
    bool is_missing = (media_item == nullptr);

    // Item background
    if (is_current) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(accent_color.x * 0.3f, accent_color.y * 0.3f, accent_color.z * 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accent_color.x * 0.4f, accent_color.y * 0.4f, accent_color.z * 0.4f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accent_color.x * 0.5f, accent_color.y * 0.5f, accent_color.z * 0.5f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    }

    // Build display name
    std::string display_name = std::to_string(index + 1) + ". ";
    if (is_missing) {
        display_name += "[Missing: " + entry.media_id + "]";
    } else {
        display_name += media_item->name;
    }

    // Text color
    if (is_missing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));  // Red for missing
    } else if (is_current) {
        ImGui::PushStyleColor(ImGuiCol_Text, accent_color);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    bool clicked = ImGui::Selectable(display_name.c_str(), is_current, ImGuiSelectableFlags_AllowDoubleClick);

    ImGui::PopStyleColor();  // Text color
    ImGui::PopStyleColor(3); // Header colors

    // Single-click to jump and play
    if (clicked && !is_missing && play_item_callback_) {
        play_item_callback_(entry.media_id, index);
    }

    // Drag source for reordering
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        drag_source_index_ = index;
        ImGui::SetDragDropPayload("PLAYLIST_REORDER", &index, sizeof(int));
        ImGui::Text("Move: %s", is_missing ? "[Missing]" : media_item->name.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target for reordering AND adding from Project panel
    if (ImGui::BeginDragDropTarget()) {
        // Reorder within playlist
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLAYLIST_REORDER")) {
            int source_index = *(const int*)payload->Data;
            if (source_index != index) {
                MoveItem(source_index, index);
            }
        }
        // Add from Project panel (drops add to end of playlist)
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            const char* media_id = static_cast<const char*>(payload->Data);
            HandleDropPayload(media_id);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEMS_MULTI")) {
            const char* payload_str = static_cast<const char*>(payload->Data);
            HandleDropPayload(payload_str);
        }
        ImGui::EndDragDropTarget();
    }

    // Right-click context menu
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Remove from Playlist")) {
            RemoveItem(index);
        }
        if (index > 0 && ImGui::MenuItem("Move Up")) {
            MoveItem(index, index - 1);
        }
        MediaItem* playlist = GetLoadedPlaylist();
        if (playlist && index < static_cast<int>(playlist->playlist_items.size()) - 1 && ImGui::MenuItem("Move Down")) {
            MoveItem(index, index + 1);
        }
        ImGui::EndPopup();
    }

    // Show duration/type info
    if (!is_missing && media_item) {
        ImGui::SameLine();
        std::string type_str;
        switch (media_item->type) {
        case MediaType::VIDEO: type_str = "video"; break;
        case MediaType::AUDIO: type_str = "audio"; break;
        case MediaType::IMAGE_SEQUENCE: type_str = "sequence"; break;
        case MediaType::EXR_SEQUENCE: type_str = "EXR"; break;
        case MediaType::TIMELINE: type_str = "timeline"; break;
        case MediaType::DUAL_VIEW: type_str = "dual"; break;
        default: type_str = "media"; break;
        }

        if (media_item->duration > 0) {
            int mins = static_cast<int>(media_item->duration) / 60;
            int secs = static_cast<int>(media_item->duration) % 60;
            ImGui::TextDisabled("[%s %d:%02d]", type_str.c_str(), mins, secs);
        } else {
            ImGui::TextDisabled("[%s]", type_str.c_str());
        }
    }

    ImGui::PopID();
}

void PlaylistPanel::RenderControls(ImVec4 accent_color) {
    MediaItem* playlist = GetLoadedPlaylist();
    if (!playlist) return;

    ImGui::Separator();
    ImGui::Spacing();

    // Slightly lighter background for checkbox
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));

    // Loop toggle - affects the loaded playlist (which should be the active one when playing)
    bool loop = playlist->playlist_loop;
    if (ImGui::Checkbox("Loop Playlist", &loop)) {
        playlist->playlist_loop = loop;
        SyncToProject();
        Debug::Log("PlaylistPanel: Loop " + std::string(loop ? "enabled" : "disabled") +
                   " for playlist: " + playlist->name);
    }

    ImGui::PopStyleColor(3);

    ImGui::SameLine();

    // Item count
    ImGui::TextDisabled("%d items", static_cast<int>(playlist->playlist_items.size()));
}

void PlaylistPanel::LoadPlaylist(const std::string& playlist_id) {
    loaded_playlist_id_ = playlist_id;
    current_index_ = 0;
    is_active_ = false;

    MediaItem* playlist = GetLoadedPlaylist();
    if (playlist) {
        Debug::Log("PlaylistPanel: Loaded playlist '" + playlist->name + "' with " +
                   std::to_string(playlist->playlist_items.size()) + " items");
    }
}

MediaItem* PlaylistPanel::GetCurrentPlaylistItem() {
    MediaItem* playlist = GetLoadedPlaylist();
    if (!playlist || playlist->playlist_items.empty()) return nullptr;

    if (current_index_ < 0 || current_index_ >= static_cast<int>(playlist->playlist_items.size())) {
        return nullptr;
    }

    const auto& entry = playlist->playlist_items[current_index_];
    return project_manager_ ? project_manager_->GetMediaItem(entry.media_id) : nullptr;
}

int PlaylistPanel::GetCurrentDisplayIndex() const {
    return current_index_ + 1;  // 1-based for display
}

int PlaylistPanel::GetTotalItemCount() const {
    MediaItem* playlist = const_cast<PlaylistPanel*>(this)->GetLoadedPlaylist();
    return playlist ? static_cast<int>(playlist->playlist_items.size()) : 0;
}

void PlaylistPanel::SetLooping(bool loop) {
    MediaItem* playlist = GetLoadedPlaylist();
    if (playlist) {
        playlist->playlist_loop = loop;
        SyncToProject();
    }
}

bool PlaylistPanel::IsLooping() const {
    MediaItem* playlist = const_cast<PlaylistPanel*>(this)->GetLoadedPlaylist();
    return playlist ? playlist->playlist_loop : false;
}

void PlaylistPanel::Clear() {
    loaded_playlist_id_.clear();
    current_index_ = 0;
    is_active_ = false;
}

void PlaylistPanel::AddItems(const std::vector<std::string>& media_ids) {
    MediaItem* playlist = GetLoadedPlaylist();
    if (!playlist) return;

    for (const auto& media_id : media_ids) {
        // Validate that the media item exists and is playable
        if (!project_manager_) continue;

        MediaItem* media_item = project_manager_->GetMediaItem(media_id);
        if (!media_item) continue;

        // Don't add playlists, single images, timelines, or dual views
        if (media_item->type == MediaType::PLAYLIST || media_item->type == MediaType::IMAGE ||
            media_item->type == MediaType::TIMELINE || media_item->type == MediaType::DUAL_VIEW) {
            continue;
        }

        PlaylistItemEntry entry;
        entry.media_id = media_id;
        entry.in_point = -1.0;
        entry.out_point = -1.0;
        playlist->playlist_items.push_back(entry);
    }

    SyncToProject();

    if (playlist_modified_callback_) {
        playlist_modified_callback_();
    }

    Debug::Log("PlaylistPanel: Added " + std::to_string(media_ids.size()) + " items to playlist");
}

void PlaylistPanel::RemoveItem(int index) {
    MediaItem* playlist = GetLoadedPlaylist();
    if (!playlist) return;

    if (index < 0 || index >= static_cast<int>(playlist->playlist_items.size())) {
        return;
    }

    // Notify controller BEFORE modifying the data (it needs the old size)
    if (playlist_controller_ && playlist_controller_->IsActive()) {
        playlist_controller_->OnItemDeleted(index);
    }

    playlist->playlist_items.erase(playlist->playlist_items.begin() + index);

    // Adjust panel's current index if needed (for non-active playback display)
    if (current_index_ >= static_cast<int>(playlist->playlist_items.size())) {
        current_index_ = std::max(0, static_cast<int>(playlist->playlist_items.size()) - 1);
    }

    SyncToProject();

    if (playlist_modified_callback_) {
        playlist_modified_callback_();
    }
}

void PlaylistPanel::MoveItem(int from_index, int to_index) {
    MediaItem* playlist = GetLoadedPlaylist();
    if (!playlist) return;

    int size = static_cast<int>(playlist->playlist_items.size());
    if (from_index < 0 || from_index >= size || to_index < 0 || to_index >= size) {
        return;
    }

    PlaylistItemEntry item = playlist->playlist_items[from_index];
    playlist->playlist_items.erase(playlist->playlist_items.begin() + from_index);
    playlist->playlist_items.insert(playlist->playlist_items.begin() + to_index, item);

    // Notify controller about the move (it adjusts its own index)
    if (playlist_controller_ && playlist_controller_->IsActive()) {
        playlist_controller_->OnItemMoved(from_index, to_index);
    }

    // Adjust panel's current index for display (mirrors controller logic)
    if (current_index_ == from_index) {
        current_index_ = to_index;
    } else if (from_index < current_index_ && to_index >= current_index_) {
        current_index_--;
    } else if (from_index > current_index_ && to_index <= current_index_) {
        current_index_++;
    }

    SyncToProject();

    if (playlist_modified_callback_) {
        playlist_modified_callback_();
    }
}

MediaItem* PlaylistPanel::GetLoadedPlaylist() {
    if (loaded_playlist_id_.empty() || !project_manager_) {
        return nullptr;
    }
    return project_manager_->GetPlaylistItem(loaded_playlist_id_);
}

void PlaylistPanel::HandleDropPayload(const char* payload) {
    if (!payload) return;

    // Parse payload - could be single ID or semicolon-separated list
    std::vector<std::string> media_ids;
    std::string payload_str(payload);

    size_t pos = 0;
    while (pos < payload_str.length()) {
        size_t next_pos = payload_str.find(';', pos);
        if (next_pos == std::string::npos) {
            std::string id = payload_str.substr(pos);
            if (!id.empty()) {
                media_ids.push_back(id);
            }
            break;
        } else {
            std::string id = payload_str.substr(pos, next_pos - pos);
            if (!id.empty()) {
                media_ids.push_back(id);
            }
            pos = next_pos + 1;
        }
    }

    if (!media_ids.empty()) {
        AddItems(media_ids);
    }
}

void PlaylistPanel::SyncToProject() {
    // Sync playlist changes back to project bins
    MediaItem* playlist = GetLoadedPlaylist();
    if (!playlist || !project_manager_) return;

    // The playlist is already modified in media_pool, but we need to update bins too
    // This is handled by the project manager's internal structure
    // For now, just mark the project as modified (if there's such a flag)
}

} // namespace ump
