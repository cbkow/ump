# Dual View Virtual Timeline Implementation Plan

## Overview

Implement a virtual timeline system for dual view mode that allows independent positioning (slip/slide) of left and right video clips without requiring EDL generation or dummy videos. The system handles gaps (out-of-range positions) gracefully by holding first/last frames or showing black.

## Core Concept

```
Virtual Timeline:  |-------- Total Duration --------|
                   0                                 max(left_end, right_end)

Left Clip:         [==== LEFT VIDEO ====]
                   ^offset=0            ^offset + duration

Right Clip:              [==== RIGHT VIDEO ====]
                         ^offset=2.0           ^offset + duration

Playhead at T=1.0:
  - Left shows frame at source_time = 1.0 (T - 0 = 1.0)
  - Right shows BLACK or HOLD (T - 2.0 = -1.0, out of range)

Playhead at T=3.0:
  - Left shows frame at source_time = 3.0
  - Right shows frame at source_time = 1.0 (T - 2.0 = 1.0)
```

## Data Model Changes

### DualViewClip (existing, extend)
```cpp
struct DualViewClip {
    std::string source_path;
    double source_in = 0.0;        // Trim in point (seconds into source)
    double source_out = 0.0;       // Trim out point (seconds into source)
    double position_offset = 0.0;  // Where clip starts on virtual timeline

    // Computed
    double source_duration = 0.0;  // Original source duration

    // Helper methods
    double GetEffectiveDuration() const;  // source_out - source_in
    double GetTimelineStart() const { return position_offset; }
    double GetTimelineEnd() const { return position_offset + GetEffectiveDuration(); }

    // NEW: Convert timeline position to source position
    // Returns: source time, or -1 if in gap (before clip)
    //          or source_duration+1 if in gap (after clip)
    double TimelineToSource(double timeline_pos) const;

    // NEW: Check if timeline position is within this clip's range
    bool ContainsTimelinePosition(double timeline_pos) const;
};
```

### DualViewTimeline (existing, extend)
```cpp
struct DualViewTimeline {
    DualViewClip left;
    DualViewClip right;

    // NEW: Virtual timeline duration (union of both clips)
    double GetVirtualDuration() const {
        double left_end = left.IsLoaded() ? left.GetTimelineEnd() : 0.0;
        double right_end = right.IsLoaded() ? right.GetTimelineEnd() : 0.0;
        return std::max(left_end, right_end);
    }

    // NEW: Get the earliest start (for timeline bounds)
    double GetVirtualStart() const {
        // Usually 0, but could be non-zero if we allow negative offsets
        return 0.0;
    }
};
```

## VideoPlayer Changes

### 1. Position Calculation (enhance existing)

```cpp
// Already exists, needs enhancement
double VideoPlayer::CalculateSecondaryPosition(double timeline_position) const {
    const auto& right_clip = dual_view_timeline_.right;

    // Convert timeline position to source position
    double source_pos = right_clip.TimelineToSource(timeline_position);

    // Handle gaps
    if (source_pos < 0) {
        // Before clip - hold first frame or return special value
        return right_clip.source_in;  // Hold at trim in point
    }
    if (source_pos > right_clip.source_duration) {
        // After clip - hold last frame
        return right_clip.source_out;  // Hold at trim out point
    }

    return source_pos;
}
```

### 2. Gap Detection for Visual Feedback

```cpp
enum class ClipPlaybackState {
    PLAYING,        // Within clip range, normal playback
    GAP_BEFORE,     // Timeline position is before clip starts
    GAP_AFTER       // Timeline position is after clip ends
};

ClipPlaybackState VideoPlayer::GetLeftClipState(double timeline_pos) const;
ClipPlaybackState VideoPlayer::GetRightClipState(double timeline_pos) const;
```

### 3. Duration Handling

The virtual timeline duration becomes the playback duration:

```cpp
double VideoPlayer::GetDualViewDuration() const {
    if (IsComparisonModeEnabled()) {
        return dual_view_timeline_.GetVirtualDuration();
    }
    return GetDuration();  // Normal single-video duration
}
```

## Sync Mechanism

### Current Flow (needs modification):
1. Primary video plays normally
2. `UpdateVideo()` syncs secondary to primary's position
3. Loop detection based on primary duration

### New Flow:
1. **Playback is based on virtual timeline**, not primary video
2. Both videos receive seek commands based on their offsets
3. Loop detection based on virtual timeline duration
4. Gap handling per-video

### Key Changes in UpdateVideo():

```cpp
void VideoPlayer::UpdateVideo() {
    if (!IsComparisonModeEnabled()) {
        // Normal single-video update
        return;
    }

    // Get current virtual timeline position
    double timeline_pos = GetPosition();  // This is the primary's position

    // For dual view, we need to think of this as the virtual timeline position
    // and sync BOTH videos appropriately

    // Left video (primary) - already at correct position, but check bounds
    ClipPlaybackState left_state = GetLeftClipState(timeline_pos);
    if (left_state == ClipPlaybackState::GAP_BEFORE) {
        // Could pause or show held frame
    }

    // Right video (secondary) - sync with offset
    double right_source_pos = CalculateSecondaryPosition(timeline_pos);
    comparison_video_->SyncToPosition(right_source_pos);

    ClipPlaybackState right_state = GetRightClipState(timeline_pos);
    // UI can use this to show gap indicators
}
```

## Visual Gap Handling Options

### Option A: Hold First/Last Frame (Simplest)
When playhead is in a gap, hold the nearest valid frame:
- Gap before clip: show first frame (at source_in)
- Gap after clip: show last frame (at source_out)

**Pros:** Simple, no shader changes
**Cons:** May be confusing - video appears "frozen"

### Option B: Dim/Tint the Held Frame
Same as Option A but apply a visual treatment:
- Reduce brightness to 50%
- Or add a subtle color tint
- Or add a "GAP" text overlay

**Pros:** Clear visual feedback
**Cons:** Requires shader modification or overlay

### Option C: Show Black/Transparent
Replace video with black (or checkerboard for transparency):

**Pros:** Very clear that it's a gap
**Cons:** More jarring, may require different rendering path

### Recommendation: Option A + UI Indicator
- Hold the frame (simple implementation)
- Show a visual indicator in the timeline UI (clip boundary marker)
- Show text overlay "Gap" or dim the viewport region

## Timeline UI Updates

### Visual Indicators Needed:
1. **Clip boundaries** - Clear start/end markers on each track
2. **Gap regions** - Hatched or dimmed areas where no clip exists
3. **Playhead position** - Shows where we are on virtual timeline
4. **Out-of-range indicator** - When playhead is in a gap for that track

### Example Rendering:
```
Left Track:   [========= VIDEO =========]
              0                         10s

Right Track:       [========= VIDEO =========]
                   2s                        12s

Virtual TL:   |--------------------------------|
              0                               12s

Playhead at 1s: Left=playing, Right=gap (hold first frame)
Playhead at 5s: Both playing
Playhead at 11s: Left=gap (hold last frame), Right=playing
```

## Implementation Phases

### Phase 1: Core Data Model
- [ ] Extend DualViewClip with TimelineToSource() and ContainsTimelinePosition()
- [ ] Extend DualViewTimeline with GetVirtualDuration()
- [ ] Update VideoPlayer::GetDuration() to return virtual duration in dual view mode

### Phase 2: Seek/Sync Updates
- [ ] Enhance CalculateSecondaryPosition() with proper gap handling
- [ ] Add GetLeftClipState() and GetRightClipState()
- [ ] Update all sync points to use virtual timeline position
- [ ] Handle loop boundaries based on virtual duration

### Phase 3: Primary Video Offset Support
- [ ] Currently only right video has offset
- [ ] Add offset support for left video too (both can be positioned independently)
- [ ] Update primary video seek to account for its offset

### Phase 4: Visual Feedback
- [ ] Timeline UI: Show gap regions with different background
- [ ] Timeline UI: Clear clip boundary markers
- [ ] Video viewport: Optional dim/overlay for gap regions
- [ ] Status indicator showing which video(s) are in gap

### Phase 5: Playback Edge Cases
- [ ] What happens when BOTH videos are in gap? (Pause? Show black?)
- [ ] Loop behavior - loop the virtual timeline or individual clips?
- [ ] Step frame behavior in gaps

## Lavfi Mode Considerations

Lavfi modes (Difference, Split views with filters) require both videos to have valid frames at the same time. Options:

1. **Disable lavfi modes when offsets create gaps at playhead**
   - Auto-revert to Side-by-Side when gap detected
   - Show warning to user

2. **Generate lavfi filter with pad/trim**
   - Complex but possible
   - Would require MPV reconstruction

3. **Restrict lavfi to "aligned" mode only**
   - Lavfi only works when both clips are fully overlapping
   - Simplest approach

### Recommendation: Option 1
Auto-revert from lavfi to edit mode when playhead enters a gap region. This preserves functionality while keeping implementation simple.

## Questions to Resolve

1. **Should left video also support offset?**
   - Currently only right video can be slipped
   - For full flexibility, both should be adjustable
   - But adds complexity

2. **Negative offsets?**
   - Should a clip be able to start before timeline 0?
   - Simpler if we say no - clips can only start at 0 or later

3. **What is the "primary" for playback control?**
   - Currently primary video drives playback
   - With virtual timeline, should we have an independent playback controller?
   - Or keep primary as the "master" and just apply offsets?

4. **Trim interaction with offset**
   - When you trim a clip, does the offset stay the same (clip gets shorter)?
   - Or does the clip stay at same timeline end position?

## Files to Modify

1. `src/player/dual_view_types.h` - Extend data structures
2. `src/player/dual_view_types.cpp` - Implement new methods (may need to create)
3. `src/player/video_player.h` - Add gap state methods
4. `src/player/video_player.cpp` - Implement virtual timeline logic
5. `src/main.cpp` - Update timeline UI to show gaps
6. `src/ui/dual_view_timeline_widget.cpp` - Gap rendering (if used)
