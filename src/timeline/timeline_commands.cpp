#include "timeline_commands.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
#include "../utils/debug_utils.h"

namespace ump {

// Helper to generate unique clip IDs
static std::string GenerateClipId() {
    static int counter = 0;
    std::ostringstream oss;
    oss << "clip_" << std::setfill('0') << std::setw(6) << ++counter
        << "_" << std::chrono::steady_clock::now().time_since_epoch().count();
    return oss.str();
}

// Helper to find clip in tracks
static OTIOClip* FindClipById(std::vector<OTIOTrack>& tracks,
                               const std::string& clip_id,
                               int* out_track_index = nullptr,
                               size_t* out_clip_index = nullptr) {
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        auto& track = tracks[ti];
        for (size_t ci = 0; ci < track.clips.size(); ++ci) {
            if (track.clips[ci].id == clip_id) {
                if (out_track_index) *out_track_index = static_cast<int>(ti);
                if (out_clip_index) *out_clip_index = ci;
                return &track.clips[ci];
            }
        }
    }
    return nullptr;
}

//-----------------------------------------------------------------------------
// TimelineCommandManager
//-----------------------------------------------------------------------------

void TimelineCommandManager::Execute(std::unique_ptr<ITimelineCommand> cmd) {
    if (!cmd) return;

    // Pause playback during edit for thread safety
    // This prevents race conditions with CacheManagementThread and AudioMixer
    if (timeline_view_) {
        timeline_view_->BeginEdit();
    }

    cmd->Execute();
    undo_stack_.push_back(std::move(cmd));

    // Clear redo stack on new action
    redo_stack_.clear();

    // Resume playback if it was playing before
    if (timeline_view_) {
        timeline_view_->EndEdit(true);
    }

    // Limit undo stack size
    while (undo_stack_.size() > MAX_UNDO_STACK_SIZE) {
        undo_stack_.erase(undo_stack_.begin());
    }
}

void TimelineCommandManager::PushExecuted(std::unique_ptr<ITimelineCommand> cmd) {
    if (!cmd) return;

    // The command is already executed (live preview), just add to undo stack
    // No BeginEdit/EndEdit since caller manages that
    undo_stack_.push_back(std::move(cmd));

    // Clear redo stack on new action
    redo_stack_.clear();

    // Limit undo stack size
    while (undo_stack_.size() > MAX_UNDO_STACK_SIZE) {
        undo_stack_.erase(undo_stack_.begin());
    }
}

void TimelineCommandManager::Undo() {
    if (!CanUndo()) return;

    // Pause playback during undo for thread safety
    if (timeline_view_) {
        timeline_view_->BeginEdit();
    }

    auto cmd = std::move(undo_stack_.back());
    undo_stack_.pop_back();

    cmd->Undo();
    redo_stack_.push_back(std::move(cmd));

    // Resume playback if it was playing before
    if (timeline_view_) {
        timeline_view_->EndEdit(true);
    }
}

void TimelineCommandManager::Redo() {
    if (!CanRedo()) return;

    // Pause playback during redo for thread safety
    if (timeline_view_) {
        timeline_view_->BeginEdit();
    }

    auto cmd = std::move(redo_stack_.back());
    redo_stack_.pop_back();

    cmd->Execute();
    undo_stack_.push_back(std::move(cmd));

    // Resume playback if it was playing before
    if (timeline_view_) {
        timeline_view_->EndEdit(true);
    }
}

bool TimelineCommandManager::CanUndo() const {
    return !undo_stack_.empty();
}

bool TimelineCommandManager::CanRedo() const {
    return !redo_stack_.empty();
}

std::string TimelineCommandManager::GetUndoDescription() const {
    if (undo_stack_.empty()) return "";
    return undo_stack_.back()->GetDescription();
}

std::string TimelineCommandManager::GetRedoDescription() const {
    if (redo_stack_.empty()) return "";
    return redo_stack_.back()->GetDescription();
}

void TimelineCommandManager::Clear() {
    undo_stack_.clear();
    redo_stack_.clear();
}

//-----------------------------------------------------------------------------
// CutClipCommand
//-----------------------------------------------------------------------------

CutClipCommand::CutClipCommand(TimelineView* view, const std::string& clip_id,
                               int track_index, double cut_time)
    : view_(view), clip_id_(clip_id), track_index_(track_index), cut_time_(cut_time) {
}

void CutClipCommand::Execute() {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    // Find the clip
    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it == track.clips.end()) return;

    // Store original for undo (only on first execution)
    if (!executed_) {
        original_clip_ = *it;
        new_clip_id_ = GenerateClipId();
    }

    // Validate cut position is within clip
    double clip_end = it->start_time + it->duration;
    if (cut_time_ <= it->start_time || cut_time_ >= clip_end) return;

    // Calculate durations
    double first_duration = cut_time_ - it->start_time;
    double second_duration = clip_end - cut_time_;

    // Create second clip (after cut point)
    OTIOClip second_clip = *it;
    second_clip.id = new_clip_id_;
    second_clip.start_time = cut_time_;
    second_clip.duration = second_duration;
    second_clip.source_in = it->source_in + first_duration;
    // source_out stays the same

    // Modify first clip (before cut point)
    it->duration = first_duration;
    it->source_out = it->source_in + first_duration;

    // Insert second clip after first
    auto insert_pos = it + 1;
    track.clips.insert(insert_pos, second_clip);

    // Lightweight sync - cut doesn't change frame mappings, only metadata
    // No need to invalidate cache since the same source frames are used
    view_->SyncFlattenerOnly();

    executed_ = true;
}

void CutClipCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    // Remove the second clip (created by cut)
    auto second_it = std::find_if(track.clips.begin(), track.clips.end(),
                                   [this](const OTIOClip& c) { return c.id == new_clip_id_; });
    if (second_it != track.clips.end()) {
        track.clips.erase(second_it);
    }

    // Restore original clip
    auto first_it = std::find_if(track.clips.begin(), track.clips.end(),
                                  [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (first_it != track.clips.end()) {
        *first_it = original_clip_;
    }

    // Lightweight sync - undo cut doesn't change frame mappings either
    view_->SyncFlattenerOnly();
}

std::string CutClipCommand::GetDescription() const {
    return "Cut Clip";
}

//-----------------------------------------------------------------------------
// DeleteClipCommand
//-----------------------------------------------------------------------------

DeleteClipCommand::DeleteClipCommand(TimelineView* view, const std::string& clip_id,
                                     int track_index)
    : view_(view), clip_id_(clip_id), track_index_(track_index) {
}

void DeleteClipCommand::Execute() {
    Debug::Log("DeleteClipCommand::Execute - clip_id=" + clip_id_ + ", track=" + std::to_string(track_index_));

    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    // Find and store the clip
    for (size_t i = 0; i < track.clips.size(); ++i) {
        if (track.clips[i].id == clip_id_) {
            if (!executed_) {
                deleted_clip_ = track.clips[i];
                original_index_ = i;
            }
            Debug::Log("DeleteClipCommand::Execute - DELETING clip: " + track.clips[i].name);
            track.clips.erase(track.clips.begin() + i);
            view_->SyncFlattenerAndInvalidate();
            executed_ = true;
            return;
        }
    }
}

void DeleteClipCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    // Restore clip at original position
    if (original_index_ <= track.clips.size()) {
        track.clips.insert(track.clips.begin() + original_index_, deleted_clip_);
    } else {
        track.clips.push_back(deleted_clip_);
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string DeleteClipCommand::GetDescription() const {
    return "Delete Clip";
}

//-----------------------------------------------------------------------------
// MoveClipCommand
//-----------------------------------------------------------------------------

MoveClipCommand::MoveClipCommand(TimelineView* view, const std::string& clip_id,
                                 int track_index, double new_start_time)
    : view_(view), clip_id_(clip_id), track_index_(track_index),
      new_start_time_(new_start_time) {
}

void MoveClipCommand::Execute() {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it == track.clips.end()) return;

    if (!executed_) {
        old_start_time_ = it->start_time;
    }

    // Clamp to non-negative
    it->start_time = std::max(0.0, new_start_time_);

    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void MoveClipCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it != track.clips.end()) {
        it->start_time = old_start_time_;
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string MoveClipCommand::GetDescription() const {
    return "Move Clip";
}

//-----------------------------------------------------------------------------
// TrimClipStartCommand
//-----------------------------------------------------------------------------

TrimClipStartCommand::TrimClipStartCommand(TimelineView* view, const std::string& clip_id,
                                           int track_index, double new_start_time)
    : view_(view), clip_id_(clip_id), track_index_(track_index),
      new_start_time_(new_start_time) {
}

void TrimClipStartCommand::Execute() {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it == track.clips.end()) return;

    if (!executed_) {
        old_start_time_ = it->start_time;
        old_source_in_ = it->source_in;
        old_duration_ = it->duration;
    }

    double clip_end = it->start_time + it->duration;
    double clamped_start = std::max(0.0, new_start_time_);
    clamped_start = std::min(clamped_start, clip_end - 0.001); // Keep minimum duration

    // Calculate the trim delta
    double trim_delta = clamped_start - it->start_time;
    double new_source_in = it->source_in + trim_delta;

    // Clamp source_in to valid range [0, source_duration - min_clip_length]
    // Can't trim past the beginning of the source media
    if (new_source_in < 0.0) {
        // Trying to extend beyond source start - clamp
        double adjustment = -new_source_in;
        clamped_start += adjustment;
        trim_delta = clamped_start - it->start_time;
        new_source_in = 0.0;
    }

    it->source_in = new_source_in;
    it->duration = clip_end - clamped_start;
    it->start_time = clamped_start;

    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void TrimClipStartCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it != track.clips.end()) {
        it->start_time = old_start_time_;
        it->source_in = old_source_in_;
        it->duration = old_duration_;
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string TrimClipStartCommand::GetDescription() const {
    return "Trim Clip Start";
}

//-----------------------------------------------------------------------------
// TrimClipEndCommand
//-----------------------------------------------------------------------------

TrimClipEndCommand::TrimClipEndCommand(TimelineView* view, const std::string& clip_id,
                                       int track_index, double new_end_time)
    : view_(view), clip_id_(clip_id), track_index_(track_index),
      new_end_time_(new_end_time) {
}

void TrimClipEndCommand::Execute() {
    Debug::Log("TrimClipEndCommand::Execute - clip_id=" + clip_id_ +
               ", track=" + std::to_string(track_index_) +
               ", new_end_time=" + std::to_string(new_end_time_));

    if (!view_) {
        Debug::Log("TrimClipEndCommand::Execute - view_ is null!");
        return;
    }

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) {
        Debug::Log("TrimClipEndCommand::Execute - invalid track_index!");
        return;
    }

    auto& track = tracks[track_index_];

    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it == track.clips.end()) {
        Debug::Log("TrimClipEndCommand::Execute - clip not found in track!");
        return;
    }

    Debug::Log("TrimClipEndCommand::Execute - found clip: start_time=" + std::to_string(it->start_time) +
               ", old_duration=" + std::to_string(it->duration) +
               ", source_in=" + std::to_string(it->source_in) +
               ", source_out=" + std::to_string(it->source_out) +
               ", source_duration=" + std::to_string(it->source_duration));

    if (!executed_) {
        old_duration_ = it->duration;
        old_source_out_ = it->source_out;
    }

    // Clamp new_end_time to maintain minimum clip duration (~1 frame at 24fps)
    // This prevents the clip from being shrunk to invisibility when dragging
    // the right edge past the left edge
    static constexpr double MIN_CLIP_DURATION = 0.042; // ~1 frame at 24fps
    double clamped_end_time = std::max(new_end_time_, it->start_time + MIN_CLIP_DURATION);
    double new_duration = clamped_end_time - it->start_time;

    // Calculate new source_out
    double new_source_out = it->source_in + new_duration;

    // Clamp source_out to not exceed source_duration (if known)
    // Can't extend beyond the end of the source media
    // Note: source_in may be an absolute timecode (e.g., 01:00:02:04 = 3602.166s)
    // so we need to check if the clip duration would exceed what's available
    if (it->source_duration > 0.0) {
        // Calculate max possible duration from source_in to end of source
        double max_available_duration = it->source_duration - it->source_in;
        if (max_available_duration > 0.0 && new_duration > max_available_duration) {
            new_duration = max_available_duration;
            new_source_out = it->source_duration;
        }
    }

    // Final safety clamp - duration must always be positive
    new_duration = std::max(MIN_CLIP_DURATION, new_duration);
    new_source_out = it->source_in + new_duration;

    Debug::Log("TrimClipEndCommand::Execute - setting new_duration=" + std::to_string(new_duration) +
               ", new_source_out=" + std::to_string(new_source_out));

    it->source_out = new_source_out;
    it->duration = new_duration;

    view_->SyncFlattenerAndInvalidate();
    executed_ = true;

    Debug::Log("TrimClipEndCommand::Execute - complete");
}

void TrimClipEndCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it != track.clips.end()) {
        it->duration = old_duration_;
        it->source_out = old_source_out_;
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string TrimClipEndCommand::GetDescription() const {
    return "Trim Clip End";
}

//-----------------------------------------------------------------------------
// InsertClipCommand
//-----------------------------------------------------------------------------

InsertClipCommand::InsertClipCommand(TimelineView* view, int track_index,
                                     const OTIOClip& clip)
    : view_(view), track_index_(track_index), clip_(clip) {
    // Ensure clip has a valid ID
    if (clip_.id.empty()) {
        clip_.id = GenerateClipId();
    }
}

void InsertClipCommand::Execute() {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];
    track.clips.push_back(clip_);

    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void InsertClipCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    // Remove the inserted clip
    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_.id; });
    if (it != track.clips.end()) {
        track.clips.erase(it);
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string InsertClipCommand::GetDescription() const {
    return "Insert Clip";
}

//-----------------------------------------------------------------------------
// MoveClipToTrackCommand
//-----------------------------------------------------------------------------

MoveClipToTrackCommand::MoveClipToTrackCommand(TimelineView* view,
                                               const std::string& clip_id,
                                               int old_track_index,
                                               int new_track_index,
                                               double new_start_time)
    : view_(view), clip_id_(clip_id), old_track_index_(old_track_index),
      new_track_index_(new_track_index), new_start_time_(new_start_time) {
}

void MoveClipToTrackCommand::Execute() {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (old_track_index_ < 0 || old_track_index_ >= static_cast<int>(tracks.size())) return;
    if (new_track_index_ < 0 || new_track_index_ >= static_cast<int>(tracks.size())) return;

    auto& old_track = tracks[old_track_index_];
    auto& new_track = tracks[new_track_index_];

    // Find clip in old track
    auto it = std::find_if(old_track.clips.begin(), old_track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it == old_track.clips.end()) return;

    if (!executed_) {
        old_start_time_ = it->start_time;
        old_clip_index_ = std::distance(old_track.clips.begin(), it);
    }

    // Copy clip and optionally update start time
    OTIOClip clip_copy = *it;
    if (new_start_time_ >= 0) {
        clip_copy.start_time = new_start_time_;
    }

    // Remove from old track
    old_track.clips.erase(it);

    // Add to new track
    new_track.clips.push_back(clip_copy);

    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void MoveClipToTrackCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (old_track_index_ < 0 || old_track_index_ >= static_cast<int>(tracks.size())) return;
    if (new_track_index_ < 0 || new_track_index_ >= static_cast<int>(tracks.size())) return;

    auto& old_track = tracks[old_track_index_];
    auto& new_track = tracks[new_track_index_];

    // Find clip in new track
    auto it = std::find_if(new_track.clips.begin(), new_track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it == new_track.clips.end()) return;

    // Restore original start time
    OTIOClip clip_copy = *it;
    clip_copy.start_time = old_start_time_;

    // Remove from new track
    new_track.clips.erase(it);

    // Insert back at original position in old track
    if (old_clip_index_ <= old_track.clips.size()) {
        old_track.clips.insert(old_track.clips.begin() + old_clip_index_, clip_copy);
    } else {
        old_track.clips.push_back(clip_copy);
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string MoveClipToTrackCommand::GetDescription() const {
    return "Move Clip to Track";
}

//-----------------------------------------------------------------------------
// CompositeCommand
//-----------------------------------------------------------------------------

CompositeCommand::CompositeCommand(const std::string& description)
    : description_(description) {
}

void CompositeCommand::AddCommand(std::unique_ptr<ITimelineCommand> cmd) {
    if (cmd) {
        commands_.push_back(std::move(cmd));
    }
}

void CompositeCommand::Execute() {
    for (auto& cmd : commands_) {
        cmd->Execute();
    }
    executed_ = true;
}

void CompositeCommand::Undo() {
    // Undo in reverse order
    for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
        (*it)->Undo();
    }
}

std::string CompositeCommand::GetDescription() const {
    return description_;
}

//-----------------------------------------------------------------------------
// MoveMultipleClipsCommand
//-----------------------------------------------------------------------------

MoveMultipleClipsCommand::MoveMultipleClipsCommand(TimelineView* view, std::vector<ClipMoveInfo> moves)
    : view_(view), moves_(std::move(moves)) {
}

void MoveMultipleClipsCommand::Execute() {
    if (!view_) return;
    auto& tracks = view_->GetTracks();

    for (auto& move : moves_) {
        if (move.track_index < 0 || move.track_index >= static_cast<int>(tracks.size())) continue;

        auto& track = tracks[move.track_index];
        auto it = std::find_if(track.clips.begin(), track.clips.end(),
                               [&](const OTIOClip& c) { return c.id == move.clip_id; });
        if (it == track.clips.end()) continue;

        if (!executed_) {
            move.old_start_time = it->start_time;  // Capture for undo
        }

        it->start_time = std::max(0.0, move.new_start_time);
    }

    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void MoveMultipleClipsCommand::Undo() {
    if (!view_) return;
    auto& tracks = view_->GetTracks();

    for (const auto& move : moves_) {
        if (move.track_index < 0 || move.track_index >= static_cast<int>(tracks.size())) continue;

        auto& track = tracks[move.track_index];
        auto it = std::find_if(track.clips.begin(), track.clips.end(),
                               [&](const OTIOClip& c) { return c.id == move.clip_id; });
        if (it == track.clips.end()) continue;

        it->start_time = move.old_start_time;
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string MoveMultipleClipsCommand::GetDescription() const {
    return "Move " + std::to_string(moves_.size()) + " clips";
}

//-----------------------------------------------------------------------------
// OverwriteEditCommand
//-----------------------------------------------------------------------------

OverwriteEditCommand::OverwriteEditCommand(TimelineView* view, int track_index,
                                           const OTIOClip& clip)
    : view_(view), track_index_(track_index), clip_(clip), is_move_(false) {
    // Ensure clip has a valid ID
    if (clip_.id.empty()) {
        clip_.id = GenerateClipId();
    }
}

OverwriteEditCommand::OverwriteEditCommand(TimelineView* view, int track_index,
                                           const OTIOClip& clip,
                                           const std::string& moving_clip_id)
    : view_(view), track_index_(track_index), clip_(clip),
      moving_clip_id_(moving_clip_id), is_move_(true) {
}

void OverwriteEditCommand::Execute() {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];

    // Snapshot original state for undo (only on first execution)
    if (!executed_) {
        original_clips_ = track.clips;
        // For moves, also store the original position
        if (is_move_) {
            for (const auto& c : track.clips) {
                if (c.id == moving_clip_id_) {
                    original_move_start_time_ = c.start_time;
                    break;
                }
            }
        }
    }

    double new_start = clip_.start_time;
    double new_end = clip_.start_time + clip_.duration;
    constexpr double EPSILON = 0.001;

    // Collect clips to process (we'll modify the vector, so collect IDs first)
    struct OverlapInfo {
        size_t index;
        std::string clip_id;
        double start;
        double end;
        double source_in;
        OTIOClip full_clip;
    };
    std::vector<OverlapInfo> overlaps;

    for (size_t i = 0; i < track.clips.size(); ++i) {
        const auto& existing = track.clips[i];
        if (is_move_ && existing.id == moving_clip_id_) continue;
        if (existing.is_gap) continue;

        double existing_start = existing.start_time;
        double existing_end = existing.start_time + existing.duration;

        bool has_overlap = (new_start < existing_end - EPSILON) &&
                          (new_end > existing_start + EPSILON);

        if (has_overlap) {
            OverlapInfo info;
            info.index = i;
            info.clip_id = existing.id;
            info.start = existing_start;
            info.end = existing_end;
            info.source_in = existing.source_in;
            info.full_clip = existing;
            overlaps.push_back(info);
        }
    }

    // Process overlaps - collect clips to delete and clips to add
    std::vector<std::string> clips_to_delete;
    std::vector<OTIOClip> clips_to_add;

    for (const auto& info : overlaps) {
        // Case 1: Fully covered - mark for deletion
        if (info.start >= new_start - EPSILON && info.end <= new_end + EPSILON) {
            clips_to_delete.push_back(info.clip_id);
        }
        // Case 2: Straddle - trim end and create tail
        else if (info.start < new_start - EPSILON && info.end > new_end + EPSILON) {
            // Find and modify the clip directly
            for (auto& c : track.clips) {
                if (c.id == info.clip_id) {
                    // Trim end to new_start
                    c.duration = new_start - c.start_time;
                    c.source_out = c.source_in + c.duration;
                    break;
                }
            }
            // Create tail clip
            OTIOClip tail_clip = info.full_clip;
            tail_clip.id = GenerateClipId();
            tail_clip.start_time = new_end;
            tail_clip.duration = info.end - new_end;
            tail_clip.source_in = info.source_in + (new_end - info.start);
            clips_to_add.push_back(tail_clip);
        }
        // Case 3: End overlaps - trim end
        else if (info.start < new_start - EPSILON && info.end > new_start + EPSILON) {
            for (auto& c : track.clips) {
                if (c.id == info.clip_id) {
                    c.duration = new_start - c.start_time;
                    c.source_out = c.source_in + c.duration;
                    break;
                }
            }
        }
        // Case 4: Start overlaps - trim start
        else if (info.start < new_end - EPSILON && info.end > new_end + EPSILON) {
            for (auto& c : track.clips) {
                if (c.id == info.clip_id) {
                    double trim_amount = new_end - c.start_time;
                    c.source_in += trim_amount;
                    c.duration -= trim_amount;
                    c.start_time = new_end;
                    break;
                }
            }
        }
    }

    // Delete marked clips
    for (const auto& id : clips_to_delete) {
        track.clips.erase(
            std::remove_if(track.clips.begin(), track.clips.end(),
                [&id](const OTIOClip& c) { return c.id == id; }),
            track.clips.end());
    }

    // Add tail clips
    for (const auto& c : clips_to_add) {
        track.clips.push_back(c);
    }

    // Insert or move the new clip
    if (is_move_) {
        for (auto& c : track.clips) {
            if (c.id == moving_clip_id_) {
                c.start_time = clip_.start_time;
                break;
            }
        }
    } else {
        track.clips.push_back(clip_);
    }

    // Single sync at the end
    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void OverwriteEditCommand::Undo() {
    if (!executed_ || !view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    // Restore original clips
    tracks[track_index_].clips = original_clips_;

    // Single sync
    view_->SyncFlattenerAndInvalidate();
}

std::string OverwriteEditCommand::GetDescription() const {
    return is_move_ ? "Move Clip (Overwrite)" : "Insert Clip (Overwrite)";
}

//-----------------------------------------------------------------------------
// TrimPlaylistClipCommand
//-----------------------------------------------------------------------------

TrimPlaylistClipCommand::TrimPlaylistClipCommand(TimelineView* view, const std::string& clip_id,
                                                 double old_source_in, double old_source_out, double old_duration,
                                                 double new_source_in, double new_source_out, double new_duration)
    : view_(view), clip_id_(clip_id),
      old_source_in_(old_source_in), old_source_out_(old_source_out), old_duration_(old_duration),
      new_source_in_(new_source_in), new_source_out_(new_source_out), new_duration_(new_duration) {
}

void TrimPlaylistClipCommand::ApplyAndRipple(double source_in, double source_out, double duration) {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (tracks.empty()) return;

    // Find and update the clip
    for (auto& clip : tracks[0].clips) {
        if (clip.id == clip_id_ && !clip.is_gap) {
            clip.source_in = source_in;
            clip.source_out = source_out;
            clip.duration = duration;
            break;
        }
    }

    // Ripple: recalculate all clip positions
    double current_time = 0.0;
    for (auto& c : tracks[0].clips) {
        c.start_time = current_time;
        current_time += c.duration;
    }

    // Update MediaItem for persistence
    MediaItem* playlist_item = view_->GetSourceMediaItem();
    if (playlist_item) {
        int playlist_index = 0;
        for (const auto& c : tracks[0].clips) {
            if (c.is_gap) continue;
            if (playlist_index < static_cast<int>(playlist_item->playlist_items.size())) {
                playlist_item->playlist_items[playlist_index].in_point = c.source_in;
                playlist_item->playlist_items[playlist_index].out_point = c.source_out;
            }
            playlist_index++;
        }
    }

    view_->SyncFlattenerAndInvalidate();
}

void TrimPlaylistClipCommand::Execute() {
    ApplyAndRipple(new_source_in_, new_source_out_, new_duration_);
    executed_ = true;
}

void TrimPlaylistClipCommand::Undo() {
    if (!executed_) return;
    ApplyAndRipple(old_source_in_, old_source_out_, old_duration_);
}

std::string TrimPlaylistClipCommand::GetDescription() const {
    return "Trim Playlist Clip";
}

//-----------------------------------------------------------------------------
// SlipClipCommand
//-----------------------------------------------------------------------------

SlipClipCommand::SlipClipCommand(TimelineView* view, const std::string& clip_id,
                                 int track_index, double new_source_in, double new_source_out)
    : view_(view), clip_id_(clip_id), track_index_(track_index),
      new_source_in_(new_source_in), new_source_out_(new_source_out) {
}

void SlipClipCommand::Execute() {
    if (!view_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];
    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it == track.clips.end()) return;

    if (!executed_) {
        old_source_in_ = it->source_in;
        old_source_out_ = it->source_out;
    }

    it->source_in = new_source_in_;
    it->source_out = new_source_out_;

    // Also update MediaItem for persistence (PLAYLIST mode)
    MediaItem* playlist_item = view_->GetSourceMediaItem();
    if (playlist_item) {
        // Find playlist index by counting non-gap clips
        int playlist_index = 0;
        for (const auto& c : track.clips) {
            if (c.id == clip_id_) break;
            if (!c.is_gap) playlist_index++;
        }
        if (playlist_index < static_cast<int>(playlist_item->playlist_items.size())) {
            playlist_item->playlist_items[playlist_index].in_point = new_source_in_;
            playlist_item->playlist_items[playlist_index].out_point = new_source_out_;
        }
    }

    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void SlipClipCommand::Undo() {
    if (!view_ || !executed_) return;

    auto& tracks = view_->GetTracks();
    if (track_index_ < 0 || track_index_ >= static_cast<int>(tracks.size())) return;

    auto& track = tracks[track_index_];
    auto it = std::find_if(track.clips.begin(), track.clips.end(),
                           [this](const OTIOClip& c) { return c.id == clip_id_; });
    if (it != track.clips.end()) {
        it->source_in = old_source_in_;
        it->source_out = old_source_out_;

        // Also update MediaItem for persistence (PLAYLIST mode)
        MediaItem* playlist_item = view_->GetSourceMediaItem();
        if (playlist_item) {
            // Find playlist index by counting non-gap clips
            int playlist_index = 0;
            for (const auto& c : track.clips) {
                if (c.id == clip_id_) break;
                if (!c.is_gap) playlist_index++;
            }
            if (playlist_index < static_cast<int>(playlist_item->playlist_items.size())) {
                playlist_item->playlist_items[playlist_index].in_point = old_source_in_;
                playlist_item->playlist_items[playlist_index].out_point = old_source_out_;
            }
        }
    }

    view_->SyncFlattenerAndInvalidate();
}

std::string SlipClipCommand::GetDescription() const {
    return "Slip Clip";
}

//-----------------------------------------------------------------------------
// ReorderPlaylistCommand
//-----------------------------------------------------------------------------

ReorderPlaylistCommand::ReorderPlaylistCommand(TimelineView* view,
                                               int source_index, int target_index)
    : view_(view), source_index_(source_index), target_index_(target_index) {
}

void ReorderPlaylistCommand::Execute() {
    if (!view_) return;

    // Get the MediaItem (playlist)
    MediaItem* playlist_item = view_->GetSourceMediaItem();
    if (!playlist_item) return;

    if (source_index_ < 0 || source_index_ >= static_cast<int>(playlist_item->playlist_items.size())) return;
    if (source_index_ == target_index_) return;

    // Extract the item
    auto moving_item = playlist_item->playlist_items[source_index_];
    playlist_item->playlist_items.erase(playlist_item->playlist_items.begin() + source_index_);

    // Adjust target index if source was before target
    int adjusted_target = target_index_;
    if (source_index_ < target_index_) {
        adjusted_target--;
    }
    adjusted_target = std::clamp(adjusted_target, 0, static_cast<int>(playlist_item->playlist_items.size()));

    // Insert at new position
    playlist_item->playlist_items.insert(
        playlist_item->playlist_items.begin() + adjusted_target, moving_item);

    Debug::Log("ReorderPlaylistCommand::Execute - moved from " + std::to_string(source_index_) +
               " to " + std::to_string(adjusted_target));

    // Reload the playlist to rebuild the timeline with new order
    view_->LoadPlaylistAsTimeline(playlist_item);
    view_->SyncFlattenerAndInvalidate();
    executed_ = true;
}

void ReorderPlaylistCommand::Undo() {
    if (!view_ || !executed_) return;

    // Get the MediaItem (playlist)
    MediaItem* playlist_item = view_->GetSourceMediaItem();
    if (!playlist_item) return;

    // Reverse the operation - move from where it ended up back to original position
    int adjusted_target = target_index_;
    if (source_index_ < target_index_) {
        adjusted_target--;
    }
    adjusted_target = std::clamp(adjusted_target, 0, static_cast<int>(playlist_item->playlist_items.size()) - 1);

    if (adjusted_target < 0 || adjusted_target >= static_cast<int>(playlist_item->playlist_items.size())) return;

    // Extract from current position
    auto moving_item = playlist_item->playlist_items[adjusted_target];
    playlist_item->playlist_items.erase(playlist_item->playlist_items.begin() + adjusted_target);

    // Reinsert at original position
    int restore_index = source_index_;
    if (adjusted_target < source_index_) {
        restore_index--;
    }
    restore_index = std::clamp(restore_index, 0, static_cast<int>(playlist_item->playlist_items.size()));

    playlist_item->playlist_items.insert(
        playlist_item->playlist_items.begin() + restore_index, moving_item);

    Debug::Log("ReorderPlaylistCommand::Undo - restored to position " + std::to_string(restore_index));

    // Reload the playlist to rebuild the timeline
    view_->LoadPlaylistAsTimeline(playlist_item);
    view_->SyncFlattenerAndInvalidate();
}

std::string ReorderPlaylistCommand::GetDescription() const {
    return "Reorder Playlist";
}

} // namespace ump
