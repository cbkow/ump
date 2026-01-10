#include "dual_view_timeline_widget.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <cmath>

// External mono font for clip names
extern ImFont* font_mono;

namespace ump {

DualViewTimelineWidget::DualViewTimelineWidget() {
}

bool DualViewTimelineWidget::Render(DualViewTimeline& timeline, double current_time,
                                     float available_width, ImFont* font_regular) {
    bool edit_occurred = false;

    // Calculate layout
    float track_lane_width = available_width - TRACK_LABEL_WIDTH;
    float total_height = (TRACK_HEIGHT + TRACK_PADDING) * 2 + TRACK_PADDING;

    // Update timeline duration based on current clip state
    timeline.UpdateDuration();

    // Auto-scale pixels_per_second to fit content
    if (timeline.timeline_duration > 0) {
        float ideal_pps = track_lane_width / static_cast<float>(timeline.timeline_duration + 1.0);
        pixels_per_second_ = std::max(20.0f, std::min(200.0f, ideal_pps));
    }

    ImVec2 widget_pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Background
    draw_list->AddRectFilled(
        widget_pos,
        ImVec2(widget_pos.x + available_width, widget_pos.y + total_height),
        IM_COL32(25, 25, 25, 255)
    );

    // Track labels column background
    draw_list->AddRectFilled(
        widget_pos,
        ImVec2(widget_pos.x + TRACK_LABEL_WIDTH, widget_pos.y + total_height),
        IM_COL32(35, 35, 35, 255)
    );

    // Separator line between labels and tracks
    draw_list->AddLine(
        ImVec2(widget_pos.x + TRACK_LABEL_WIDTH, widget_pos.y),
        ImVec2(widget_pos.x + TRACK_LABEL_WIDTH, widget_pos.y + total_height),
        IM_COL32(50, 50, 50, 255), 1.0f
    );

    // Render Left track
    ImVec2 left_track_pos(widget_pos.x, widget_pos.y + TRACK_PADDING);
    draw_list->AddText(
        ImVec2(left_track_pos.x + 8, left_track_pos.y + (TRACK_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f),
        IM_COL32(180, 180, 180, 255), "Left"
    );
    RenderTrack(timeline.left, true, timeline, current_time,
                ImVec2(widget_pos.x + TRACK_LABEL_WIDTH, left_track_pos.y),
                track_lane_width, draw_list, font_regular);

    // Track separator
    float separator_y = left_track_pos.y + TRACK_HEIGHT + TRACK_PADDING * 0.5f;
    draw_list->AddLine(
        ImVec2(widget_pos.x, separator_y),
        ImVec2(widget_pos.x + available_width, separator_y),
        IM_COL32(50, 50, 50, 255), 1.0f
    );

    // Render Right track
    ImVec2 right_track_pos(widget_pos.x, left_track_pos.y + TRACK_HEIGHT + TRACK_PADDING);
    draw_list->AddText(
        ImVec2(right_track_pos.x + 8, right_track_pos.y + (TRACK_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f),
        IM_COL32(180, 180, 180, 255), "Right"
    );
    RenderTrack(timeline.right, false, timeline, current_time,
                ImVec2(widget_pos.x + TRACK_LABEL_WIDTH, right_track_pos.y),
                track_lane_width, draw_list, font_regular);

    // Draw playhead (vertical line at current time)
    if (timeline.timeline_duration > 0 && current_time >= 0) {
        float playhead_x = widget_pos.x + TRACK_LABEL_WIDTH + TimeToPixel(current_time);
        if (playhead_x >= widget_pos.x + TRACK_LABEL_WIDTH &&
            playhead_x <= widget_pos.x + available_width) {

            // Playhead line
            draw_list->AddLine(
                ImVec2(playhead_x, widget_pos.y),
                ImVec2(playhead_x, widget_pos.y + total_height),
                IM_COL32(255, 255, 255, 200), PLAYHEAD_WIDTH
            );

            // Playhead head (small triangle)
            float head_size = 6.0f;
            draw_list->AddTriangleFilled(
                ImVec2(playhead_x, widget_pos.y),
                ImVec2(playhead_x - head_size, widget_pos.y - head_size),
                ImVec2(playhead_x + head_size, widget_pos.y - head_size),
                IM_COL32(255, 255, 255, 255)
            );
        }
    }

    // Handle global mouse release (end drag operations)
    if (!ImGui::IsMouseDown(0)) {
        if (trim_state_.active) {
            trim_state_.active = false;
            edit_occurred = true;
            if (on_edit_state_changed_) on_edit_state_changed_(false);
        }
        if (slide_state_.active) {
            slide_state_.active = false;
            edit_occurred = true;
            if (on_edit_state_changed_) on_edit_state_changed_(false);
        }
    }

    // Handle ongoing trim operation
    if (trim_state_.active && ImGui::IsMouseDragging(0)) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        float mouse_rel_x = mouse_pos.x - (widget_pos.x + TRACK_LABEL_WIDTH);
        double mouse_time = PixelToTime(mouse_rel_x);

        // Collect snap points
        auto snap_points = CollectSnapPoints(timeline, current_time);
        bool snapped = false;
        double snapped_time = CalculateSnappedPosition(mouse_time, snap_points, &snapped);
        if (snapped) mouse_time = snapped_time;

        // Get the clip being trimmed
        DualViewClip& clip = trim_state_.is_left_track ? timeline.left : timeline.right;

        if (trim_state_.is_left_edge) {
            // Trimming left edge - out point stays fixed on timeline
            // The out point's TIMELINE position must remain constant:
            //   out_timeline = offset + duration = offset + (source_out - source_in)
            //
            // We want:
            //   - new_offset = where user dragged (mouse_time)
            //   - source_out stays fixed (original value)
            //   - source_in adjusts so that: original_out_timeline = new_offset + (source_out - new_source_in)

            // The timeline position where the out point sits (MUST stay fixed)
            double original_out_timeline = trim_state_.original_offset +
                (trim_state_.original_out - trim_state_.original_in);

            // Mouse position is the new clip start on timeline
            double new_offset = mouse_time;

            // Clamp: can't have negative timeline offset
            new_offset = std::max(0.0, new_offset);

            // Clamp: can't go past the out point on timeline (keep minimum duration)
            new_offset = std::min(new_offset, original_out_timeline - 0.1);

            // Calculate new source_in:
            // out_timeline = new_offset + (source_out - new_source_in)
            // new_source_in = source_out - (out_timeline - new_offset)
            // Use ORIGINAL source_out since it shouldn't change during left edge trim
            double new_in = trim_state_.original_out - (original_out_timeline - new_offset);

            // Clamp: can't trim before source start (0)
            if (new_in < 0.0) {
                new_in = 0.0;
                // Recalculate offset to respect this limit
                new_offset = original_out_timeline - (trim_state_.original_out - new_in);
            }

            clip.source_in = new_in;
            clip.source_out = trim_state_.original_out;  // Ensure source_out stays fixed
            clip.position_offset = new_offset;
            clip.duration = clip.source_out - clip.source_in;

            if (on_trim_changed_) {
                on_trim_changed_(trim_state_.is_left_track, clip.source_in, clip.source_out);
            }
        } else {
            // Trimming right edge (source_out)
            double delta = mouse_time - trim_state_.original_offset;
            double original_out_rel = trim_state_.original_out - trim_state_.original_offset;
            double new_out = trim_state_.original_out + (mouse_time - trim_state_.original_offset - original_out_rel +
                            (trim_state_.original_out - trim_state_.original_in));

            // Simpler: map mouse directly to out point
            new_out = clip.source_in + (mouse_time - clip.position_offset);

            // Clamp: can't extend past source duration or trim before source_in
            new_out = std::max(clip.source_in + 0.1, new_out);  // Keep minimum duration
            new_out = std::min(new_out, clip.source_duration);

            clip.source_out = new_out;
            clip.duration = clip.source_out - clip.source_in;

            if (on_trim_changed_) {
                on_trim_changed_(trim_state_.is_left_track, clip.source_in, clip.source_out);
            }
        }
        edit_occurred = true;
    }

    // Handle ongoing slide operation
    if (slide_state_.active && ImGui::IsMouseDragging(0)) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        float mouse_rel_x = mouse_pos.x - (widget_pos.x + TRACK_LABEL_WIDTH);
        double mouse_time = PixelToTime(mouse_rel_x);

        // Calculate new offset
        double delta = mouse_time - slide_state_.drag_start_time;
        double new_offset = slide_state_.original_offset + delta;

        // Clamp: can't slide before 0
        new_offset = std::max(0.0, new_offset);

        // Apply snap
        auto snap_points = CollectSnapPoints(timeline, current_time);
        bool snapped = false;
        double snapped_offset = CalculateSnappedPosition(new_offset, snap_points, &snapped);
        if (snapped) new_offset = snapped_offset;

        // Update the clip
        DualViewClip& clip = slide_state_.is_left_track ? timeline.left : timeline.right;
        clip.position_offset = new_offset;

        if (on_slide_changed_) {
            on_slide_changed_(slide_state_.is_left_track, new_offset);
        }
        edit_occurred = true;
    }

    // Advance cursor
    ImGui::Dummy(ImVec2(available_width, total_height));

    return edit_occurred;
}

void DualViewTimelineWidget::RenderTrack(DualViewClip& clip, bool is_left_track,
                                          const DualViewTimeline& timeline,
                                          double current_time, ImVec2 track_pos,
                                          float track_width, ImDrawList* draw_list,
                                          ImFont* font_regular) {
    // Track background
    draw_list->AddRectFilled(
        track_pos,
        ImVec2(track_pos.x + track_width, track_pos.y + TRACK_HEIGHT),
        IM_COL32(30, 30, 30, 255)
    );

    // Draw gap regions with hatched/dimmed background if clip has an offset
    // This shows where there is "dead space" before the clip starts
    if (clip.IsLoaded() && clip.position_offset > 0.01) {
        float gap_width = TimeToPixel(clip.position_offset);
        if (gap_width > 0) {
            ImVec2 gap_min = track_pos;
            ImVec2 gap_max = ImVec2(track_pos.x + gap_width, track_pos.y + TRACK_HEIGHT);

            // Draw dark gap region
            draw_list->AddRectFilled(gap_min, gap_max, IM_COL32(15, 15, 15, 255));

            // Draw diagonal hatch lines to indicate gap
            float hatch_spacing = 8.0f;
            ImU32 hatch_color = IM_COL32(40, 40, 40, 255);
            for (float x = gap_min.x; x < gap_max.x + TRACK_HEIGHT; x += hatch_spacing) {
                ImVec2 line_start(std::max(x, gap_min.x), gap_min.y);
                ImVec2 line_end(std::max(x - TRACK_HEIGHT, gap_min.x), gap_max.y);

                // Clamp to gap bounds
                if (line_start.x > gap_max.x) continue;
                if (line_end.x < gap_min.x) line_end.x = gap_min.x;
                if (line_start.x < gap_min.x) line_start.x = gap_min.x;
                if (line_end.x > gap_max.x) continue;

                draw_list->AddLine(line_start, line_end, hatch_color, 1.0f);
            }

            // Draw "GAP" text if wide enough
            if (gap_width > 40.0f) {
                const char* gap_label = "GAP";
                ImVec2 text_size = ImGui::CalcTextSize(gap_label);
                ImVec2 text_pos(
                    gap_min.x + (gap_width - text_size.x) * 0.5f,
                    gap_min.y + (TRACK_HEIGHT - text_size.y) * 0.5f
                );
                draw_list->AddText(text_pos, IM_COL32(60, 60, 60, 255), gap_label);
            }
        }
    }

    // Also show gap region after clip ends (if timeline extends past clip)
    if (clip.IsLoaded()) {
        double clip_end = clip.GetTimelineEnd();
        double timeline_end = timeline.GetVirtualDuration();
        if (timeline_end > clip_end + 0.01) {
            float gap_start_x = track_pos.x + TimeToPixel(clip_end);
            float gap_end_x = track_pos.x + TimeToPixel(timeline_end);

            // Clamp to track bounds
            gap_start_x = std::max(gap_start_x, track_pos.x);
            gap_end_x = std::min(gap_end_x, track_pos.x + track_width);

            if (gap_end_x > gap_start_x) {
                ImVec2 gap_min(gap_start_x, track_pos.y);
                ImVec2 gap_max(gap_end_x, track_pos.y + TRACK_HEIGHT);

                // Draw dark gap region
                draw_list->AddRectFilled(gap_min, gap_max, IM_COL32(15, 15, 15, 255));

                // Draw diagonal hatch lines
                float hatch_spacing = 8.0f;
                ImU32 hatch_color = IM_COL32(40, 40, 40, 255);
                float gap_width = gap_end_x - gap_start_x;
                for (float x = gap_min.x; x < gap_max.x + TRACK_HEIGHT; x += hatch_spacing) {
                    ImVec2 line_start(std::max(x, gap_min.x), gap_min.y);
                    ImVec2 line_end(std::max(x - TRACK_HEIGHT, gap_min.x), gap_max.y);

                    if (line_start.x > gap_max.x) continue;
                    if (line_end.x < gap_min.x) line_end.x = gap_min.x;
                    if (line_start.x < gap_min.x) line_start.x = gap_min.x;
                    if (line_end.x > gap_max.x) continue;

                    draw_list->AddLine(line_start, line_end, hatch_color, 1.0f);
                }

                // Draw "GAP" text if wide enough
                if (gap_width > 40.0f) {
                    const char* gap_label = "GAP";
                    ImVec2 text_size = ImGui::CalcTextSize(gap_label);
                    ImVec2 text_pos(
                        gap_min.x + (gap_width - text_size.x) * 0.5f,
                        gap_min.y + (TRACK_HEIGHT - text_size.y) * 0.5f
                    );
                    draw_list->AddText(text_pos, IM_COL32(60, 60, 60, 255), gap_label);
                }
            }
        }
    }

    if (!clip.IsLoaded()) {
        // Empty track - show placeholder
        const char* placeholder = is_left_track ? "Drag Left video here" : "Drag Right video here";
        ImVec2 text_size = ImGui::CalcTextSize(placeholder);
        ImVec2 text_pos(
            track_pos.x + (track_width - text_size.x) * 0.5f,
            track_pos.y + (TRACK_HEIGHT - text_size.y) * 0.5f
        );
        draw_list->AddText(text_pos, IM_COL32(80, 80, 80, 255), placeholder);
        return;
    }

    // Calculate clip visual bounds
    float clip_x = track_pos.x + TimeToPixel(clip.position_offset);
    float clip_w = TimeToPixel(clip.GetEffectiveDuration());
    float clip_y = track_pos.y + TRACK_PADDING;
    float clip_h = TRACK_HEIGHT - TRACK_PADDING * 2;

    // Clamp to visible area
    float render_left = std::max(clip_x, track_pos.x);
    float render_right = std::min(clip_x + clip_w, track_pos.x + track_width);

    if (render_right <= render_left) return;  // Not visible

    // Clip colors
    // LEFT TRACK IS LOCKED - shown dimmer to indicate non-editable
    // RIGHT TRACK uses the standard blueish clip colors
    ImU32 fill_color = is_left_track ?
        IM_COL32(50, 70, 90, 255) :    // Dimmed blueish for locked left
        IM_COL32(70, 100, 140, 255);   // Standard blueish for editable right

    ImU32 border_color = is_left_track ?
        IM_COL32(70, 90, 110, 255) :   // Dimmed border for locked left
        IM_COL32(100, 140, 180, 255);  // Standard border for right

    // Highlight during edit (only right track can be edited)
    bool is_editing_this = !is_left_track &&
                           ((trim_state_.active && !trim_state_.is_left_track) ||
                            (slide_state_.active && !slide_state_.is_left_track));
    if (is_editing_this) {
        fill_color = IM_COL32(90, 130, 180, 255);  // Brighter blue when editing
    }

    // Draw clip rectangle
    ImVec2 clip_min(render_left, clip_y);
    ImVec2 clip_max(render_right, clip_y + clip_h);

    draw_list->AddRectFilled(clip_min, clip_max, fill_color, 3.0f);
    draw_list->AddRect(clip_min, clip_max, border_color, 3.0f, 0, 1.5f);

    // Draw trim handles (edge indicators) - ONLY for right track (left is locked)
    if (!is_left_track) {
        // Left edge handle
        if (clip_x >= track_pos.x) {
            draw_list->AddRectFilled(
                ImVec2(clip_x, clip_y),
                ImVec2(clip_x + 3, clip_y + clip_h),
                IM_COL32(255, 255, 255, 100), 1.0f
            );
        }
        // Right edge handle
        if (clip_x + clip_w <= track_pos.x + track_width) {
            draw_list->AddRectFilled(
                ImVec2(clip_x + clip_w - 3, clip_y),
                ImVec2(clip_x + clip_w, clip_y + clip_h),
                IM_COL32(255, 255, 255, 100), 1.0f
            );
        }
    } else {
        // Draw lock indicator for left track
        float lock_size = 12.0f;
        float lock_x = render_left + 6.0f;
        float lock_y = clip_y + (clip_h - lock_size) * 0.5f;

        // Lock body (rectangle)
        draw_list->AddRectFilled(
            ImVec2(lock_x, lock_y + lock_size * 0.4f),
            ImVec2(lock_x + lock_size, lock_y + lock_size),
            IM_COL32(180, 150, 80, 200), 2.0f
        );
        // Lock shackle (arc/rectangle at top)
        draw_list->AddRect(
            ImVec2(lock_x + lock_size * 0.2f, lock_y),
            ImVec2(lock_x + lock_size * 0.8f, lock_y + lock_size * 0.5f),
            IM_COL32(180, 150, 80, 200), 2.0f, 0, 2.0f
        );
    }

    // Clip name/path (truncated)
    float available_text_width = (render_right - render_left) - 10.0f;
    if (available_text_width > 30.0f) {
        std::string display_name = clip.source_path;

        // Extract filename from path
        size_t last_slash = display_name.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            display_name = display_name.substr(last_slash + 1);
        }

        // Truncate if needed
        if (font_mono) ImGui::PushFont(font_mono);
        ImVec2 text_size = ImGui::CalcTextSize(display_name.c_str());
        if (text_size.x > available_text_width) {
            while (display_name.length() > 4 &&
                   ImGui::CalcTextSize((display_name.substr(0, display_name.length() - 3) + "...").c_str()).x > available_text_width) {
                display_name.pop_back();
            }
            if (display_name.length() > 3) {
                display_name = display_name.substr(0, display_name.length() - 3) + "...";
            }
            text_size = ImGui::CalcTextSize(display_name.c_str());
        }

        ImVec2 text_pos(
            render_left + (render_right - render_left - text_size.x) * 0.5f,
            clip_y + (clip_h - text_size.y) * 0.5f
        );

        // Shadow
        draw_list->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), IM_COL32(0, 0, 0, 180), display_name.c_str());
        draw_list->AddText(text_pos, IM_COL32(255, 255, 255, 255), display_name.c_str());

        if (font_mono) ImGui::PopFont();
    }

    // Handle mouse interaction
    HandleClipInteraction(clip, is_left_track, timeline, clip_min, clip_max, clip_x);
}

void DualViewTimelineWidget::HandleClipInteraction(DualViewClip& clip, bool is_left_track,
                                                    const DualViewTimeline& timeline,
                                                    ImVec2 clip_min, ImVec2 clip_max,
                                                    float clip_x_offset) {
    if (!clip.IsLoaded()) return;

    // LEFT TRACK IS LOCKED - no editing allowed
    // The left/primary video defines the timeline duration and cannot be trimmed/offset.
    // This ensures MPV can handle audio and seeking without virtual timeline extension issues.
    if (is_left_track) {
        // Show tooltip explaining the lock
        if (ImGui::IsMouseHoveringRect(clip_min, clip_max)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "LOCKED");
            ImGui::Text("Left video defines the timeline.");
            ImGui::Text("Duration: %.2fs", clip.source_duration);
            ImGui::Text("Edit the Right video to align it.");
            ImGui::EndTooltip();
        }
        return;  // No interaction for left track
    }

    ImVec2 mouse_pos = ImGui::GetMousePos();

    // Check if mouse is over clip
    if (!ImGui::IsMouseHoveringRect(clip_min, clip_max)) return;

    // Detect edge zones
    bool near_left_edge = (mouse_pos.x >= clip_min.x && mouse_pos.x <= clip_min.x + EDGE_HANDLE_WIDTH);
    bool near_right_edge = (mouse_pos.x >= clip_max.x - EDGE_HANDLE_WIDTH && mouse_pos.x <= clip_max.x);

    // Set cursor based on position
    if (near_left_edge || near_right_edge) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Handle click
    if (ImGui::IsMouseClicked(0) && !trim_state_.active && !slide_state_.active) {
        float track_lane_left = clip_min.x - TimeToPixel(clip.position_offset);
        double click_time = PixelToTime(mouse_pos.x - track_lane_left);

        if (near_left_edge) {
            // Start left edge trim
            Debug::Log("DualViewTimeline: Starting left edge trim for Right track");
            trim_state_.active = true;
            trim_state_.is_left_track = false;  // Always right track now
            trim_state_.is_left_edge = true;
            trim_state_.original_in = clip.source_in;
            trim_state_.original_out = clip.source_out;
            trim_state_.original_offset = clip.position_offset;

            if (on_edit_state_changed_) on_edit_state_changed_(true);
        } else if (near_right_edge) {
            // Start right edge trim
            Debug::Log("DualViewTimeline: Starting right edge trim for Right track");
            trim_state_.active = true;
            trim_state_.is_left_track = false;  // Always right track now
            trim_state_.is_left_edge = false;
            trim_state_.original_in = clip.source_in;
            trim_state_.original_out = clip.source_out;
            trim_state_.original_offset = clip.position_offset;

            if (on_edit_state_changed_) on_edit_state_changed_(true);
        } else {
            // Start slide/drag
            Debug::Log("DualViewTimeline: Starting slide for Right track");
            slide_state_.active = true;
            slide_state_.is_left_track = false;  // Always right track now
            slide_state_.original_offset = clip.position_offset;
            slide_state_.drag_start_time = click_time;

            if (on_edit_state_changed_) on_edit_state_changed_(true);
        }
    }

    // Tooltip on hover (when not editing)
    if (!trim_state_.active && !slide_state_.active && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Duration: %.2fs", clip.GetEffectiveDuration());
        ImGui::Text("Source: %.2fs - %.2fs", clip.source_in, clip.source_out);
        if (clip.position_offset > 0) {
            ImGui::Text("Offset: %.2fs", clip.position_offset);
        }
        ImGui::EndTooltip();
    }
}

std::vector<double> DualViewTimelineWidget::CollectSnapPoints(const DualViewTimeline& timeline,
                                                               double current_time) {
    std::vector<double> points;

    // Add playhead position
    if (current_time >= 0) {
        points.push_back(current_time);
    }

    // Add 0 (timeline start)
    points.push_back(0.0);

    // Add left clip edges
    if (timeline.left.IsLoaded()) {
        points.push_back(timeline.left.position_offset);
        points.push_back(timeline.left.position_offset + timeline.left.GetEffectiveDuration());
    }

    // Add right clip edges
    if (timeline.right.IsLoaded()) {
        points.push_back(timeline.right.position_offset);
        points.push_back(timeline.right.position_offset + timeline.right.GetEffectiveDuration());
    }

    // Sort and remove duplicates
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());

    return points;
}

double DualViewTimelineWidget::CalculateSnappedPosition(double pos,
                                                         const std::vector<double>& snap_points,
                                                         bool* snapped) {
    if (!snap_enabled_ || snap_points.empty()) {
        if (snapped) *snapped = false;
        return pos;
    }

    double tolerance_time = PixelToTime(snap_tolerance_);
    double best_snap = pos;
    double best_distance = tolerance_time;

    for (double snap_point : snap_points) {
        double distance = std::abs(pos - snap_point);
        if (distance < best_distance) {
            best_distance = distance;
            best_snap = snap_point;
        }
    }

    if (best_snap != pos) {
        if (snapped) *snapped = true;
        return best_snap;
    }

    if (snapped) *snapped = false;
    return pos;
}

} // namespace ump
