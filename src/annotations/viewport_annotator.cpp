#include "viewport_annotator.h"
#include "ink_stroke_modeler_wrapper.h"
#include <algorithm>

namespace ump {
namespace Annotations {

ViewportAnnotator::ViewportAnnotator()
    : stroke_modeler_(std::make_unique<InkStrokeModelerWrapper>()) {
    // Initialize with default values
}

ViewportAnnotator::~ViewportAnnotator() = default;

void ViewportAnnotator::SetMode(ViewportMode new_mode) {
    if (mode_ == new_mode) {
        return;
    }

    // Clear any active drawing when switching modes
    if (mode_ == ViewportMode::ANNOTATION && new_mode == ViewportMode::PLAYBACK) {
        ClearActiveStroke();
        is_drawing_ = false;
    }

    mode_ = new_mode;
}

ViewportMode ViewportAnnotator::ToggleMode() {
    ViewportMode new_mode = (mode_ == ViewportMode::PLAYBACK)
        ? ViewportMode::ANNOTATION
        : ViewportMode::PLAYBACK;
    SetMode(new_mode);
    return mode_;
}

bool ViewportAnnotator::ProcessInput(const ImVec2& display_pos, const ImVec2& display_size) {
    // Only process input in annotation mode
    if (mode_ != ViewportMode::ANNOTATION) {
        return false;
    }

    // Don't process drawing input if a popup is open (e.g., color picker)
    // Exception: allow input when editing in modal popup
    if (!allow_input_in_popup_ && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return false;
    }

    // No tool selected
    if (active_tool_ == DrawingTool::NONE) {
        return false;
    }

    // Dispatch to tool-specific handlers
    switch (active_tool_) {
        case DrawingTool::FREEHAND:
            ProcessFreehandInput(display_pos, display_size);
            return true;

        case DrawingTool::RECTANGLE:
            ProcessRectangleInput(display_pos, display_size);
            return true;

        case DrawingTool::OVAL:
            ProcessOvalInput(display_pos, display_size);
            return true;

        case DrawingTool::ARROW:
            ProcessArrowInput(display_pos, display_size);
            return true;

        case DrawingTool::LINE:
            ProcessLineInput(display_pos, display_size);
            return true;

        default:
            return false;
    }
}

std::unique_ptr<ActiveStroke> ViewportAnnotator::FinalizeStroke() {
    if (active_stroke_.tool == DrawingTool::NONE || active_stroke_.points.empty()) {
        return nullptr;
    }

    // Create a copy of the active stroke
    auto stroke = std::make_unique<ActiveStroke>(active_stroke_);
    stroke->is_complete = true;

    // Clear the active stroke
    ClearActiveStroke();
    is_drawing_ = false;

    return stroke;
}

ImVec2 ViewportAnnotator::ScreenToNormalized(
    const ImVec2& screen_pos,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    ImVec2 normalized;
    normalized.x = (screen_pos.x - display_pos.x) / display_size.x;
    normalized.y = (screen_pos.y - display_pos.y) / display_size.y;

    // Clamp to [0, 1] range
    normalized.x = std::clamp(normalized.x, 0.0f, 1.0f);
    normalized.y = std::clamp(normalized.y, 0.0f, 1.0f);

    return normalized;
}

ImVec2 ViewportAnnotator::NormalizedToScreen(
    const ImVec2& normalized_pos,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    ImVec2 screen;
    screen.x = display_pos.x + (normalized_pos.x * display_size.x);
    screen.y = display_pos.y + (normalized_pos.y * display_size.y);
    return screen;
}

bool ViewportAnnotator::IsInsideDisplayArea(
    const ImVec2& screen_pos,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    return screen_pos.x >= display_pos.x &&
           screen_pos.x <= display_pos.x + display_size.x &&
           screen_pos.y >= display_pos.y &&
           screen_pos.y <= display_pos.y + display_size.y;
}

void ViewportAnnotator::ProcessFreehandInput(const ImVec2& display_pos, const ImVec2& display_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse_pos = io.MousePos;

    // Check if mouse is inside display area
    if (!IsInsideDisplayArea(mouse_pos, display_pos, display_size)) {
        // If we were drawing and mouse left the area, finalize the stroke
        if (is_drawing_) {
            // End the stroke with the modeler
            if (stroke_modeler_ && stroke_modeler_->IsActive()) {
                ImVec2 last_pos = active_stroke_.points.empty()
                    ? ScreenToNormalized(mouse_pos, display_pos, display_size)
                    : active_stroke_.points.back();
                double timestamp = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - stroke_start_time_).count();
                auto final_points = stroke_modeler_->EndStroke(last_pos, timestamp);
                for (const auto& pt : final_points) {
                    active_stroke_.points.push_back(pt);
                    active_stroke_.timestamps.push_back(timestamp);
                }
            }
            active_stroke_.is_complete = true;
            is_drawing_ = false;
        }
        return;
    }

    auto now = std::chrono::steady_clock::now();

    // Start drawing on mouse down
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        is_drawing_ = true;
        active_stroke_.Clear();
        active_stroke_.tool = DrawingTool::FREEHAND;
        active_stroke_.color = drawing_color_;
        active_stroke_.stroke_width = stroke_width_;
        active_stroke_.is_modeled = true;  // Points will be smoothed by ink-stroke-modeler

        // Initialize stroke timing
        stroke_start_time_ = now;

        // Initialize the stroke modeler
        if (stroke_modeler_) {
            InkStrokeModelerWrapper::Config config;
            // Use default config tuned for video annotation
            stroke_modeler_->BeginStroke(config);
        }

        // Add first point through the modeler
        ImVec2 normalized = ScreenToNormalized(mouse_pos, display_pos, display_size);
        double timestamp = 0.0;  // First point at t=0

        if (stroke_modeler_ && stroke_modeler_->IsActive()) {
            auto modeled_points = stroke_modeler_->AddPoint(normalized, timestamp, true);
            for (const auto& pt : modeled_points) {
                active_stroke_.points.push_back(pt);
                active_stroke_.timestamps.push_back(timestamp);
            }
        } else {
            // Fallback: store raw point if modeler isn't available
            active_stroke_.points.push_back(normalized);
            active_stroke_.timestamps.push_back(timestamp);
            active_stroke_.is_modeled = false;
        }
    }

    // Continue drawing while mouse is down
    if (is_drawing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 normalized = ScreenToNormalized(mouse_pos, display_pos, display_size);

        // Calculate timestamp relative to stroke start
        double timestamp = std::chrono::duration<double>(now - stroke_start_time_).count();

        if (stroke_modeler_ && stroke_modeler_->IsActive()) {
            auto modeled_points = stroke_modeler_->AddPoint(normalized, timestamp, false);
            for (const auto& pt : modeled_points) {
                active_stroke_.points.push_back(pt);
                active_stroke_.timestamps.push_back(timestamp);
            }
        } else {
            // Fallback: store raw point
            if (active_stroke_.points.empty() ||
                (normalized.x != active_stroke_.points.back().x ||
                 normalized.y != active_stroke_.points.back().y)) {
                active_stroke_.points.push_back(normalized);
                active_stroke_.timestamps.push_back(timestamp);
            }
        }
    }

    // Finish drawing on mouse release
    if (is_drawing_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        ImVec2 normalized = ScreenToNormalized(mouse_pos, display_pos, display_size);
        double timestamp = std::chrono::duration<double>(now - stroke_start_time_).count();

        // Get final smoothed points from the modeler
        if (stroke_modeler_ && stroke_modeler_->IsActive()) {
            auto final_points = stroke_modeler_->EndStroke(normalized, timestamp);
            for (const auto& pt : final_points) {
                active_stroke_.points.push_back(pt);
                active_stroke_.timestamps.push_back(timestamp);
            }
        }

        active_stroke_.is_complete = true;
        is_drawing_ = false;
    }
}

void ViewportAnnotator::ProcessRectangleInput(const ImVec2& display_pos, const ImVec2& display_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse_pos = io.MousePos;

    // Check if mouse is inside display area
    if (!IsInsideDisplayArea(mouse_pos, display_pos, display_size)) {
        return;
    }

    // Start drawing on mouse down
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        is_drawing_ = true;
        active_stroke_.Clear();
        active_stroke_.tool = DrawingTool::RECTANGLE;
        active_stroke_.color = drawing_color_;
        active_stroke_.stroke_width = stroke_width_;
        active_stroke_.filled = fill_enabled_;

        drag_start_pos_ = ScreenToNormalized(mouse_pos, display_pos, display_size);
    }

    // Update rectangle while dragging
    if (is_drawing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 current_pos = ScreenToNormalized(mouse_pos, display_pos, display_size);

        // Store as 4 corners: top-left, top-right, bottom-right, bottom-left
        active_stroke_.points.clear();
        active_stroke_.points.push_back(ImVec2(drag_start_pos_.x, drag_start_pos_.y));      // Top-left
        active_stroke_.points.push_back(ImVec2(current_pos.x, drag_start_pos_.y));          // Top-right
        active_stroke_.points.push_back(ImVec2(current_pos.x, current_pos.y));              // Bottom-right
        active_stroke_.points.push_back(ImVec2(drag_start_pos_.x, current_pos.y));          // Bottom-left
    }

    // Finish drawing on mouse release
    if (is_drawing_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        active_stroke_.is_complete = true;
        is_drawing_ = false;
    }
}

void ViewportAnnotator::ProcessOvalInput(const ImVec2& display_pos, const ImVec2& display_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse_pos = io.MousePos;

    // Check if mouse is inside display area
    if (!IsInsideDisplayArea(mouse_pos, display_pos, display_size)) {
        return;
    }

    // Start drawing on mouse down
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        is_drawing_ = true;
        active_stroke_.Clear();
        active_stroke_.tool = DrawingTool::OVAL;
        active_stroke_.color = drawing_color_;
        active_stroke_.stroke_width = stroke_width_;
        active_stroke_.filled = fill_enabled_;

        drag_start_pos_ = ScreenToNormalized(mouse_pos, display_pos, display_size);
    }

    // Update oval while dragging
    if (is_drawing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 current_pos = ScreenToNormalized(mouse_pos, display_pos, display_size);

        // Store as center + radii (3 points: center, radius_x as x offset, radius_y as y offset)
        ImVec2 center;
        center.x = (drag_start_pos_.x + current_pos.x) * 0.5f;
        center.y = (drag_start_pos_.y + current_pos.y) * 0.5f;

        ImVec2 radii;
        radii.x = std::abs(current_pos.x - drag_start_pos_.x) * 0.5f;
        radii.y = std::abs(current_pos.y - drag_start_pos_.y) * 0.5f;

        active_stroke_.points.clear();
        active_stroke_.points.push_back(center);   // Point 0: center
        active_stroke_.points.push_back(radii);    // Point 1: radii (x=radius_x, y=radius_y)
    }

    // Finish drawing on mouse release
    if (is_drawing_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        active_stroke_.is_complete = true;
        is_drawing_ = false;
    }
}

void ViewportAnnotator::ProcessArrowInput(const ImVec2& display_pos, const ImVec2& display_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse_pos = io.MousePos;

    // Check if mouse is inside display area
    if (!IsInsideDisplayArea(mouse_pos, display_pos, display_size)) {
        return;
    }

    // Start drawing on mouse down
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        is_drawing_ = true;
        active_stroke_.Clear();
        active_stroke_.tool = DrawingTool::ARROW;
        active_stroke_.color = drawing_color_;
        active_stroke_.stroke_width = stroke_width_;

        drag_start_pos_ = ScreenToNormalized(mouse_pos, display_pos, display_size);
        active_stroke_.points.push_back(drag_start_pos_);  // Start point
    }

    // Update arrow while dragging
    if (is_drawing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 current_pos = ScreenToNormalized(mouse_pos, display_pos, display_size);

        // Store as 2 points: start and end
        if (active_stroke_.points.size() < 2) {
            active_stroke_.points.push_back(current_pos);
        } else {
            active_stroke_.points[1] = current_pos;  // Update end point
        }
    }

    // Finish drawing on mouse release
    if (is_drawing_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        active_stroke_.is_complete = true;
        is_drawing_ = false;
    }
}

void ViewportAnnotator::ProcessLineInput(const ImVec2& display_pos, const ImVec2& display_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse_pos = io.MousePos;

    // Check if mouse is inside display area
    if (!IsInsideDisplayArea(mouse_pos, display_pos, display_size)) {
        return;
    }

    // Start drawing on mouse down
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        is_drawing_ = true;
        active_stroke_.Clear();
        active_stroke_.tool = DrawingTool::LINE;
        active_stroke_.color = drawing_color_;
        active_stroke_.stroke_width = stroke_width_;

        drag_start_pos_ = ScreenToNormalized(mouse_pos, display_pos, display_size);
        active_stroke_.points.push_back(drag_start_pos_);  // Start point
    }

    // Update line while dragging
    if (is_drawing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 current_pos = ScreenToNormalized(mouse_pos, display_pos, display_size);

        // Store as 2 points: start and end
        if (active_stroke_.points.size() < 2) {
            active_stroke_.points.push_back(current_pos);
        } else {
            active_stroke_.points[1] = current_pos;  // Update end point
        }
    }

    // Finish drawing on mouse release
    if (is_drawing_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        active_stroke_.is_complete = true;
        is_drawing_ = false;
    }
}

} // namespace Annotations
} // namespace ump
