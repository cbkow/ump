#pragma once

#include <vector>
#include <memory>
#include <chrono>
#include <imgui.h>

// Forward declarations to minimize header includes
namespace ink {
namespace stroke_model {
class StrokeModeler;
}
}

namespace qcview {
namespace Annotations {

/**
 * Wrapper around Google's ink-stroke-modeler for real-time stroke smoothing.
 *
 * This class handles:
 * - Coordinate transformation between normalized (0-1) and modeler space
 * - Configuration of wobble smoothing and spring-mass parameters
 * - Real-time input processing with timestamp-based smoothing
 *
 * Usage:
 *   InkStrokeModelerWrapper modeler;
 *   modeler.BeginStroke();
 *
 *   // On mouse down:
 *   auto points = modeler.AddPoint(normalized_pos, timestamp, true);
 *
 *   // On mouse move:
 *   auto points = modeler.AddPoint(normalized_pos, timestamp, false);
 *
 *   // On mouse up:
 *   auto final_points = modeler.EndStroke(normalized_pos, timestamp);
 */
class InkStrokeModelerWrapper {
public:
    InkStrokeModelerWrapper();
    ~InkStrokeModelerWrapper();

    // Non-copyable, movable
    InkStrokeModelerWrapper(const InkStrokeModelerWrapper&) = delete;
    InkStrokeModelerWrapper& operator=(const InkStrokeModelerWrapper&) = delete;
    InkStrokeModelerWrapper(InkStrokeModelerWrapper&&) noexcept;
    InkStrokeModelerWrapper& operator=(InkStrokeModelerWrapper&&) noexcept;

    /**
     * Configuration for the stroke modeler.
     * Tuned for video annotation use case.
     */
    struct Config {
        // Wobble smoothing parameters
        float wobble_timeout_seconds = 0.04f;     // ~40ms window for wobble smoothing
        float wobble_speed_floor = 31.0f;         // Speed below which max smoothing is applied
        float wobble_speed_ceiling = 620.0f;      // Speed above which no smoothing is applied

        // Spring-mass simulation parameters
        float spring_mass_constant = 11.0f / 32400.0f;  // Default from ink-stroke-modeler
        float drag_constant = 72.0f;                     // Default drag

        // Prediction (disabled by default for annotation accuracy)
        bool enable_prediction = false;

        Config() = default;
    };

    /**
     * Initialize/reset the modeler for a new stroke.
     * Must be called before AddPoint.
     */
    void BeginStroke(const Config& config);
    inline void BeginStroke();

    /**
     * Process a new input point. Returns modeled points generated so far.
     *
     * @param normalized_pos Position in normalized coordinates (0-1 range)
     * @param timestamp Timestamp in seconds (relative to stroke start)
     * @param is_down True for first point (pen down), false for subsequent move events
     * @return Newly generated modeled points in normalized coordinates (may be empty initially)
     */
    std::vector<ImVec2> AddPoint(const ImVec2& normalized_pos, double timestamp, bool is_down);

    /**
     * Finalize the stroke (pen up). Returns any remaining modeled points.
     *
     * @param final_pos Final pen position in normalized coordinates
     * @param timestamp Final timestamp in seconds
     * @return Remaining modeled points to complete the stroke
     */
    std::vector<ImVec2> EndStroke(const ImVec2& final_pos, double timestamp);

    /**
     * Get predicted points for live preview (optional, for lower latency feedback).
     * Only valid when enable_prediction is true in Config.
     *
     * @return Predicted points extending beyond current modeled position
     */
    std::vector<ImVec2> GetPrediction() const;

    /**
     * Check if modeler is currently processing a stroke.
     */
    bool IsActive() const { return is_active_; }

private:
    std::unique_ptr<ink::stroke_model::StrokeModeler> modeler_;
    bool is_active_ = false;

    // Coordinate scaling: normalized (0-1) to modeler space
    // We scale up for better numerical precision in the physics simulation
    static constexpr float kModelScale = 1000.0f;

    // Convert between coordinate systems
    ImVec2 ToModelSpace(const ImVec2& normalized) const;
    ImVec2 FromModelSpace(float x, float y) const;
};

// Defined after class is complete to satisfy GCC C++20 aggregate rules
inline void InkStrokeModelerWrapper::BeginStroke() { BeginStroke(Config{}); }

} // namespace Annotations
} // namespace qcview
