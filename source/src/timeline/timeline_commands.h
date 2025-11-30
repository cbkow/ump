#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "timeline_view.h"

namespace ump {

// Forward declarations
class TimelineView;

// Base command interface for undo/redo system
class ITimelineCommand {
public:
    virtual ~ITimelineCommand() = default;
    virtual void Execute() = 0;
    virtual void Undo() = 0;
    virtual std::string GetDescription() const = 0;
};

// Command manager with undo/redo stacks
class TimelineCommandManager {
public:
    TimelineCommandManager() = default;
    ~TimelineCommandManager() = default;

    // Execute a command and add to undo stack
    void Execute(std::unique_ptr<ITimelineCommand> cmd);

    // Undo/Redo operations
    void Undo();
    void Redo();

    // Query state
    bool CanUndo() const;
    bool CanRedo() const;
    std::string GetUndoDescription() const;
    std::string GetRedoDescription() const;

    // Clear all history
    void Clear();

    // Get stack sizes (for UI display)
    size_t GetUndoStackSize() const { return undo_stack_.size(); }
    size_t GetRedoStackSize() const { return redo_stack_.size(); }

private:
    std::vector<std::unique_ptr<ITimelineCommand>> undo_stack_;
    std::vector<std::unique_ptr<ITimelineCommand>> redo_stack_;
    static constexpr size_t MAX_UNDO_STACK_SIZE = 100;
};

// Cut a clip at a specific time, creating two clips
class CutClipCommand : public ITimelineCommand {
public:
    CutClipCommand(TimelineView* view, const std::string& clip_id,
                   int track_index, double cut_time);

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

private:
    TimelineView* view_;
    std::string clip_id_;
    int track_index_;
    double cut_time_;

    // State for undo
    OTIOClip original_clip_;
    std::string new_clip_id_;  // ID of the second clip created by cut
    bool executed_ = false;
};

// Delete a clip from the timeline
class DeleteClipCommand : public ITimelineCommand {
public:
    DeleteClipCommand(TimelineView* view, const std::string& clip_id, int track_index);

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

private:
    TimelineView* view_;
    std::string clip_id_;
    int track_index_;

    // State for undo
    OTIOClip deleted_clip_;
    size_t original_index_;  // Position in clips vector
    bool executed_ = false;
};

// Move a clip to a new start time (horizontal drag)
class MoveClipCommand : public ITimelineCommand {
public:
    MoveClipCommand(TimelineView* view, const std::string& clip_id,
                    int track_index, double new_start_time);

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

private:
    TimelineView* view_;
    std::string clip_id_;
    int track_index_;
    double old_start_time_;
    double new_start_time_;
    bool executed_ = false;
};

// Trim clip start (left edge drag)
class TrimClipStartCommand : public ITimelineCommand {
public:
    TrimClipStartCommand(TimelineView* view, const std::string& clip_id,
                         int track_index, double new_start_time);

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

private:
    TimelineView* view_;
    std::string clip_id_;
    int track_index_;
    double old_start_time_;
    double old_source_in_;
    double old_duration_;
    double new_start_time_;
    bool executed_ = false;
};

// Trim clip end (right edge drag)
class TrimClipEndCommand : public ITimelineCommand {
public:
    TrimClipEndCommand(TimelineView* view, const std::string& clip_id,
                       int track_index, double new_end_time);

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

private:
    TimelineView* view_;
    std::string clip_id_;
    int track_index_;
    double old_duration_;
    double old_source_out_;
    double new_end_time_;
    bool executed_ = false;
};

// Insert a new clip into the timeline
class InsertClipCommand : public ITimelineCommand {
public:
    InsertClipCommand(TimelineView* view, int track_index, const OTIOClip& clip);

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

private:
    TimelineView* view_;
    int track_index_;
    OTIOClip clip_;
    bool executed_ = false;
};

// Move clip to a different track (vertical drag)
class MoveClipToTrackCommand : public ITimelineCommand {
public:
    MoveClipToTrackCommand(TimelineView* view, const std::string& clip_id,
                           int old_track_index, int new_track_index,
                           double new_start_time = -1.0);  // -1 = keep same time

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

private:
    TimelineView* view_;
    std::string clip_id_;
    int old_track_index_;
    int new_track_index_;
    double old_start_time_;
    double new_start_time_;
    size_t old_clip_index_;
    bool executed_ = false;
};

// Composite command for batch operations (e.g., delete multiple selected clips)
class CompositeCommand : public ITimelineCommand {
public:
    CompositeCommand(const std::string& description);

    void AddCommand(std::unique_ptr<ITimelineCommand> cmd);
    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override;

    bool IsEmpty() const { return commands_.empty(); }

private:
    std::string description_;
    std::vector<std::unique_ptr<ITimelineCommand>> commands_;
    bool executed_ = false;
};

} // namespace ump
