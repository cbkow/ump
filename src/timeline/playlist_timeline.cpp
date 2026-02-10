#include "playlist_timeline.h"
#include "timeline_types.h"
#include "../project/media_item.h"
#include "../utils/debug_utils.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace ump {

PlaylistTimeline::PlaylistTimeline() = default;
PlaylistTimeline::~PlaylistTimeline() = default;

//=============================================================================
// Clip Access
//=============================================================================

PlaylistClip* PlaylistTimeline::GetClip(int index) {
    if (index < 0 || index >= static_cast<int>(clips_.size())) {
        return nullptr;
    }
    return &clips_[index];
}

const PlaylistClip* PlaylistTimeline::GetClip(int index) const {
    if (index < 0 || index >= static_cast<int>(clips_.size())) {
        return nullptr;
    }
    return &clips_[index];
}

PlaylistClip* PlaylistTimeline::GetClipById(const std::string& id) {
    auto it = std::find_if(clips_.begin(), clips_.end(),
        [&id](const PlaylistClip& clip) { return clip.id == id; });
    return it != clips_.end() ? &(*it) : nullptr;
}

const PlaylistClip* PlaylistTimeline::GetClipById(const std::string& id) const {
    auto it = std::find_if(clips_.begin(), clips_.end(),
        [&id](const PlaylistClip& clip) { return clip.id == id; });
    return it != clips_.end() ? &(*it) : nullptr;
}

PlaylistClip* PlaylistTimeline::GetClipAtTime(double time) {
    for (auto& clip : clips_) {
        if (clip.ContainsTime(time)) {
            return &clip;
        }
    }
    return nullptr;
}

const PlaylistClip* PlaylistTimeline::GetClipAtTime(double time) const {
    for (const auto& clip : clips_) {
        if (clip.ContainsTime(time)) {
            return &clip;
        }
    }
    return nullptr;
}

int PlaylistTimeline::GetClipIndexAtTime(double time) const {
    for (size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].ContainsTime(time)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

PlaylistClip* PlaylistTimeline::GetNextClip(double time) {
    for (auto& clip : clips_) {
        if (clip.timeline_start > time && !clip.is_gap) {
            return &clip;
        }
    }
    return nullptr;
}

const PlaylistClip* PlaylistTimeline::GetNextClip(double time) const {
    for (const auto& clip : clips_) {
        if (clip.timeline_start > time && !clip.is_gap) {
            return &clip;
        }
    }
    return nullptr;
}

//=============================================================================
// Timeline Properties
//=============================================================================

double PlaylistTimeline::GetDuration() const {
    if (clips_.empty()) {
        return 0.0;
    }
    return clips_.back().GetTimelineEnd();
}

int PlaylistTimeline::GetTotalFrames() const {
    return static_cast<int>(std::ceil(GetDuration() * frame_rate_));
}

//=============================================================================
// Editing Operations
//=============================================================================

int PlaylistTimeline::InsertClip(int index, const MediaItem& item) {
    PlaylistClip clip;
    clip.id = GenerateClipId();
    clip.media_item_id = item.id;
    clip.name = item.name;

    // Set source duration from MediaItem
    clip.source_duration = item.duration;
    clip.source_in = 0.0;
    clip.source_out = item.duration;
    clip.duration = item.duration;

    // Copy metadata from MediaItem
    clip.source_path = item.path;
    clip.has_audio = item.has_audio;

    // Handle different media types
    if (item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) {
        clip.is_sequence = true;
        clip.sequence_directory = item.image_seq.directory;
        clip.sequence_pattern = item.image_seq.pattern;
        clip.sequence_start_frame = item.image_seq.start_frame;
        clip.sequence_end_frame = item.image_seq.end_frame;
        clip.sequence_exr_layer = item.image_seq.layer;
        clip.sequence_audio_file = item.image_seq.audio_file;
        clip.source_fps = item.image_seq.frame_rate;
        clip.source_width = item.image_seq.width;
        clip.source_height = item.image_seq.height;
        clip.source_duration = item.image_seq.duration;
        clip.source_out = clip.source_duration;
        clip.duration = clip.source_duration;
    } else if (item.type == MediaType::VIDEO) {
        clip.source_fps = item.frame_rate;
        // Dimensions would be populated when linked
    }

    // Clamp index
    if (index < 0 || index > static_cast<int>(clips_.size())) {
        index = static_cast<int>(clips_.size());
    }

    // Insert gap before clip if there are existing clips and this isn't the first
    double gap_duration = GetGapDuration();

    if (index == 0 && !clips_.empty()) {
        // Inserting at start - add gap after new clip
        clip.timeline_start = 0.0;
        clips_.insert(clips_.begin(), clip);

        if (gap_duration > 0 && clips_.size() > 1) {
            PlaylistClip gap = PlaylistClip::CreateGap(clip.GetTimelineEnd(), gap_duration);
            clips_.insert(clips_.begin() + 1, gap);
        }
    } else if (index > 0) {
        // Inserting after existing clip - add gap before new clip
        if (gap_duration > 0) {
            double gap_start = clips_[index - 1].GetTimelineEnd();
            PlaylistClip gap = PlaylistClip::CreateGap(gap_start, gap_duration);
            clips_.insert(clips_.begin() + index, gap);
            index++; // Adjust for inserted gap
        }
        clips_.insert(clips_.begin() + index, clip);
    } else {
        // First clip - no gap needed
        clip.timeline_start = gap_duration; // Start with initial gap

        if (gap_duration > 0) {
            PlaylistClip initial_gap = PlaylistClip::CreateGap(0.0, gap_duration);
            clips_.push_back(initial_gap);
        }
        clips_.push_back(clip);
        index = static_cast<int>(clips_.size()) - 1;
    }

    // Recalculate all positions
    RecalculatePositions(0);
    UpdateCanvasDimensions();
    NotifyEdit();

    Debug::Log("[PlaylistTimeline] Inserted clip '" + clip.name + "' at index " +
               std::to_string(index) + ", duration: " + std::to_string(clip.duration) + "s");

    return index;
}

int PlaylistTimeline::InsertClipAfterTime(double time, const MediaItem& item) {
    int index = GetClipIndexAtTime(time);
    if (index >= 0) {
        return InsertClip(index + 1, item);
    }
    return AppendClip(item);
}

int PlaylistTimeline::AppendClip(const MediaItem& item) {
    return InsertClip(static_cast<int>(clips_.size()), item);
}

bool PlaylistTimeline::DeleteClip(int index) {
    if (index < 0 || index >= static_cast<int>(clips_.size())) {
        return false;
    }

    const auto& clip = clips_[index];
    if (clip.is_gap) {
        // Gaps are managed automatically, don't allow direct deletion
        return false;
    }

    Debug::Log("[PlaylistTimeline] Deleting clip '" + clip.name + "' at index " + std::to_string(index));

    // Remove clip
    clips_.erase(clips_.begin() + index);

    // Remove adjacent gap if present
    if (index > 0 && index <= static_cast<int>(clips_.size()) && clips_[index - 1].is_gap) {
        clips_.erase(clips_.begin() + index - 1);
    } else if (index < static_cast<int>(clips_.size()) && clips_[index].is_gap) {
        clips_.erase(clips_.begin() + index);
    }

    // Recalculate positions (ripple collapse)
    RecalculatePositions(0);
    NotifyEdit();

    return true;
}

bool PlaylistTimeline::MoveClip(int from_index, int to_index) {
    if (from_index < 0 || from_index >= static_cast<int>(clips_.size())) {
        return false;
    }
    if (to_index < 0 || to_index >= static_cast<int>(clips_.size())) {
        return false;
    }
    if (from_index == to_index) {
        return true; // No-op
    }

    // Don't allow moving gaps directly
    if (clips_[from_index].is_gap) {
        return false;
    }

    Debug::Log("[PlaylistTimeline] Moving clip from index " + std::to_string(from_index) + " to " + std::to_string(to_index));

    // Extract clip (with its preceding gap if present)
    PlaylistClip clip = clips_[from_index];
    clips_.erase(clips_.begin() + from_index);

    // Adjust to_index if needed after erasure
    if (to_index > from_index) {
        to_index--;
    }

    // Insert at new position
    clips_.insert(clips_.begin() + to_index, clip);

    // Recalculate all positions (ripple)
    RecalculatePositions(0);
    NotifyEdit();

    return true;
}

bool PlaylistTimeline::TrimClipStart(int index, double new_source_in) {
    PlaylistClip* clip = GetClip(index);
    if (!clip || clip->is_gap) {
        return false;
    }

    // Clamp to valid range
    new_source_in = std::max(0.0, std::min(new_source_in, clip->source_out - 0.01));

    double delta = new_source_in - clip->source_in;
    clip->source_in = new_source_in;
    clip->duration = clip->source_out - clip->source_in;

    Debug::Log("[PlaylistTimeline] Trimmed clip '" + clip->name + "' start to " +
               std::to_string(new_source_in) + "s, new duration: " + std::to_string(clip->duration) + "s");

    // Recalculate positions (ripple)
    RecalculatePositions(0);
    NotifyEdit();

    return true;
}

bool PlaylistTimeline::TrimClipEnd(int index, double new_source_out) {
    PlaylistClip* clip = GetClip(index);
    if (!clip || clip->is_gap) {
        return false;
    }

    // Clamp to valid range
    new_source_out = std::max(clip->source_in + 0.01, std::min(new_source_out, clip->source_duration));

    clip->source_out = new_source_out;
    clip->duration = clip->source_out - clip->source_in;

    Debug::Log("[PlaylistTimeline] Trimmed clip '" + clip->name + "' end to " +
               std::to_string(new_source_out) + "s, new duration: " + std::to_string(clip->duration) + "s");

    // Recalculate positions (ripple)
    RecalculatePositions(0);
    NotifyEdit();

    return true;
}

bool PlaylistTimeline::SlipClip(int index, double offset) {
    PlaylistClip* clip = GetClip(index);
    if (!clip || clip->is_gap) {
        return false;
    }

    // Calculate new source in/out
    double new_in = clip->source_in + offset;
    double new_out = clip->source_out + offset;

    // Clamp to source boundaries
    if (new_in < 0.0) {
        double shift = -new_in;
        new_in = 0.0;
        new_out += shift;
    }
    if (new_out > clip->source_duration) {
        double shift = new_out - clip->source_duration;
        new_out = clip->source_duration;
        new_in -= shift;
        new_in = std::max(0.0, new_in);
    }

    clip->source_in = new_in;
    clip->source_out = new_out;
    // Duration stays the same for slip edit

    Debug::Log("[PlaylistTimeline] Slipped clip '" + clip->name + "' by " +
               std::to_string(offset) + "s, new source range: " + std::to_string(new_in) + " - " + std::to_string(new_out));

    NotifyEdit();

    return true;
}

int PlaylistTimeline::CutClipAtTime(double time) {
    int index = GetClipIndexAtTime(time);
    if (index < 0) {
        return -1;
    }

    PlaylistClip* clip = GetClip(index);
    if (!clip || clip->is_gap) {
        return -1;
    }

    // Calculate cut point in source time
    double offset_in_clip = time - clip->timeline_start;
    double cut_source_time = clip->source_in + offset_in_clip;

    // Create two clips from the original
    PlaylistClip first_half = *clip;
    first_half.source_out = cut_source_time;
    first_half.duration = first_half.source_out - first_half.source_in;
    first_half.id = GenerateClipId();

    PlaylistClip second_half = *clip;
    second_half.source_in = cut_source_time;
    second_half.duration = second_half.source_out - second_half.source_in;
    second_half.id = GenerateClipId();

    // Replace original with first half
    clips_[index] = first_half;

    // Insert gap and second half
    double gap_duration = GetGapDuration();
    if (gap_duration > 0) {
        PlaylistClip gap = PlaylistClip::CreateGap(first_half.GetTimelineEnd(), gap_duration);
        clips_.insert(clips_.begin() + index + 1, gap);
        clips_.insert(clips_.begin() + index + 2, second_half);
    } else {
        clips_.insert(clips_.begin() + index + 1, second_half);
    }

    Debug::Log("[PlaylistTimeline] Cut clip '" + clip->name + "' at " + std::to_string(time) + "s into two clips");

    RecalculatePositions(0);
    NotifyEdit();

    return index;
}

void PlaylistTimeline::SetClips(const std::vector<PlaylistClip>& clips) {
    clips_ = clips;
    RecalculatePositions(0);
    UpdateCanvasDimensions();
    NotifyEdit();
}

void PlaylistTimeline::Clear() {
    clips_.clear();
    NotifyEdit();
}

//=============================================================================
// Conversion to OTIOTrack format
//=============================================================================

OTIOTrack PlaylistTimeline::ToOTIOTrack() const {
    OTIOTrack track;
    track.id = "V1";
    track.name = "Playlist";
    track.is_video = true;
    track.visible = true;
    track.locked = false;
    track.z_index = 0;

    for (const auto& clip : clips_) {
        OTIOClip otio_clip;
        otio_clip.id = clip.id;
        otio_clip.name = clip.name;
        otio_clip.file_path = clip.source_path;
        otio_clip.start_time = clip.timeline_start;
        otio_clip.duration = clip.duration;
        otio_clip.source_in = clip.source_in;
        otio_clip.source_out = clip.source_out;
        otio_clip.is_gap = clip.is_gap;

        otio_clip.linked_path = clip.source_path;
        otio_clip.is_linked = !clip.source_path.empty();
        otio_clip.source_fps = clip.source_fps;
        otio_clip.source_width = clip.source_width;
        otio_clip.source_height = clip.source_height;
        otio_clip.source_duration = clip.source_duration;
        otio_clip.has_audio = clip.has_audio;

        // Image sequence fields
        otio_clip.is_sequence = clip.is_sequence;
        otio_clip.sequence_directory = clip.sequence_directory;
        otio_clip.sequence_pattern = clip.sequence_pattern;
        otio_clip.sequence_start_frame = clip.sequence_start_frame;
        otio_clip.sequence_end_frame = clip.sequence_end_frame;
        otio_clip.sequence_exr_layer = clip.sequence_exr_layer;

        track.clips.push_back(otio_clip);
    }

    return track;
}

//=============================================================================
// Project Integration
//=============================================================================

bool PlaylistTimeline::LinkClipToMedia(PlaylistClip& clip) {
    if (!project_manager_ || clip.media_item_id.empty()) {
        return false;
    }

    // This would call project_manager_->GetMediaItem(clip.media_item_id)
    // and populate clip metadata from the MediaItem
    // Implementation depends on ProjectManager API

    return true;
}

void PlaylistTimeline::RelinkAllClips() {
    for (auto& clip : clips_) {
        if (!clip.is_gap) {
            LinkClipToMedia(clip);
        }
    }
    UpdateCanvasDimensions();
}

//=============================================================================
// Internal Helpers
//=============================================================================

void PlaylistTimeline::RecalculatePositions(int start_index) {
    if (clips_.empty()) {
        return;
    }

    double current_time = 0.0;

    // If starting from 0, add initial gap
    if (start_index == 0 && !clips_.empty()) {
        double gap_duration = GetGapDuration();
        current_time = gap_duration;
    } else if (start_index > 0) {
        current_time = clips_[start_index - 1].GetTimelineEnd();
    }

    double gap_duration = GetGapDuration();

    for (size_t i = start_index; i < clips_.size(); ++i) {
        auto& clip = clips_[i];

        // Add gap before non-gap clips (except first clip which already has initial gap)
        if (i > 0 && !clip.is_gap && !clips_[i - 1].is_gap && gap_duration > 0) {
            // Previous clip wasn't a gap, so we need to add gap time
            current_time += gap_duration;
        }

        clip.timeline_start = current_time;
        current_time = clip.GetTimelineEnd();
    }
}

std::string PlaylistTimeline::GenerateClipId() const {
    std::ostringstream oss;
    oss << "playlist_clip_" << std::setfill('0') << std::setw(4) << next_clip_id_++;
    return oss.str();
}

void PlaylistTimeline::UpdateCanvasDimensions() {
    canvas_width_ = 1920;
    canvas_height_ = 1080;

    for (const auto& clip : clips_) {
        if (!clip.is_gap && clip.source_width > 0 && clip.source_height > 0) {
            canvas_width_ = std::max(canvas_width_, clip.source_width);
            canvas_height_ = std::max(canvas_height_, clip.source_height);
        }
    }
}

void PlaylistTimeline::NotifyEdit() {
    if (edit_callback_) {
        edit_callback_();
    }
}

} // namespace ump
