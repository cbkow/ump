#pragma once

#include <vector>
#include <imgui.h>

namespace ump {
namespace Annotations {

/**
 * Stroke smoothing engine using Catmull-Rom spline interpolation.
 * Provides professional-quality smoothing for freehand drawing strokes.
 */
class StrokeSmoother {
public:
    /**
     * Configuration for stroke smoothing behavior.
     */
    struct SmoothingConfig {
        float alpha = 0.5f;           // Centripetal parameter (0.0 = uniform, 0.5 = centripetal, 1.0 = chordal)
        int segments_per_curve = 20;  // Number of interpolated points between control points
        float min_point_distance = 0.001f;  // Minimum distance between points (normalized coords)

        SmoothingConfig() = default;
    };

    /**
     * Smooth a stroke using Catmull-Rom spline interpolation.
     *
     * @param input_points Raw input points from mouse/touch input
     * @param config Smoothing configuration parameters
     * @return Smoothed stroke as interpolated points
     *
     * Edge cases:
     * - 0-1 points: Returns input unchanged
     * - 2-3 points: Returns linear interpolation
     * - 4+ points: Full Catmull-Rom interpolation
     */
    static std::vector<ImVec2> SmoothStroke(
        const std::vector<ImVec2>& input_points,
        const SmoothingConfig& config = SmoothingConfig()
    );

private:
    /**
     * Interpolate a single curve segment between P1 and P2 using Catmull-Rom spline.
     * Uses four control points: P0, P1, P2, P3
     * The curve passes through P1 and P2.
     */
    static void InterpolateCurveSegment(
        const ImVec2& P0,
        const ImVec2& P1,
        const ImVec2& P2,
        const ImVec2& P3,
        float alpha,
        int segments,
        std::vector<ImVec2>& output
    );

    /**
     * Evaluate Catmull-Rom spline at parameter t ∈ [0, 1].
     * Uses the centripetal variant for better behavior on sharp curves.
     */
    static ImVec2 EvaluateCatmullRom(
        const ImVec2& P0,
        const ImVec2& P1,
        const ImVec2& P2,
        const ImVec2& P3,
        float t
    );

    /**
     * Remove duplicate points that are too close together.
     */
    static std::vector<ImVec2> RemoveDuplicates(
        const std::vector<ImVec2>& points,
        float min_distance
    );

    /**
     * Calculate distance between two points.
     */
    static float Distance(const ImVec2& a, const ImVec2& b);
};

} // namespace Annotations
} // namespace ump
