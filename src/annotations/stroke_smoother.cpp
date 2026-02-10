#include "stroke_smoother.h"
#include <cmath>
#include <algorithm>

namespace ump {
namespace Annotations {

std::vector<ImVec2> StrokeSmoother::SmoothStroke(
    const std::vector<ImVec2>& input_points,
    const SmoothingConfig& config
) {
    // Edge case: Empty or single point
    if (input_points.size() <= 1) {
        return input_points;
    }

    // Remove duplicate points that are too close together
    std::vector<ImVec2> cleaned_points = RemoveDuplicates(input_points, config.min_point_distance);

    // Edge case: After cleaning, check again
    if (cleaned_points.size() <= 1) {
        return cleaned_points;
    }

    // Edge case: Two points - return linear interpolation
    if (cleaned_points.size() == 2) {
        std::vector<ImVec2> result;
        result.reserve(config.segments_per_curve + 1);

        for (int i = 0; i <= config.segments_per_curve; ++i) {
            float t = static_cast<float>(i) / config.segments_per_curve;
            ImVec2 point;
            point.x = cleaned_points[0].x + t * (cleaned_points[1].x - cleaned_points[0].x);
            point.y = cleaned_points[0].y + t * (cleaned_points[1].y - cleaned_points[0].y);
            result.push_back(point);
        }

        return result;
    }

    // Edge case: Three points - simple curve
    if (cleaned_points.size() == 3) {
        std::vector<ImVec2> result;
        result.reserve(config.segments_per_curve * 2);

        // Use first point as phantom P0
        InterpolateCurveSegment(
            cleaned_points[0],  // P0 (phantom - use first point)
            cleaned_points[0],  // P1
            cleaned_points[1],  // P2
            cleaned_points[2],  // P3
            config.alpha,
            config.segments_per_curve,
            result
        );

        // Use last point as phantom P3
        InterpolateCurveSegment(
            cleaned_points[0],  // P0
            cleaned_points[1],  // P1
            cleaned_points[2],  // P2
            cleaned_points[2],  // P3 (phantom - use last point)
            config.alpha,
            config.segments_per_curve,
            result
        );

        result.push_back(cleaned_points[2]); // Ensure last point is included
        return result;
    }

    // Full Catmull-Rom interpolation for 4+ points
    std::vector<ImVec2> result;
    result.reserve(cleaned_points.size() * config.segments_per_curve);

    const size_t n = cleaned_points.size();

    // First segment: use first point as phantom P0
    InterpolateCurveSegment(
        cleaned_points[0],  // P0 (phantom)
        cleaned_points[0],  // P1
        cleaned_points[1],  // P2
        cleaned_points[2],  // P3
        config.alpha,
        config.segments_per_curve,
        result
    );

    // Middle segments: full Catmull-Rom with all four control points
    for (size_t i = 0; i < n - 3; ++i) {
        InterpolateCurveSegment(
            cleaned_points[i],      // P0
            cleaned_points[i + 1],  // P1
            cleaned_points[i + 2],  // P2
            cleaned_points[i + 3],  // P3
            config.alpha,
            config.segments_per_curve,
            result
        );
    }

    // Last segment: use last point as phantom P3
    InterpolateCurveSegment(
        cleaned_points[n - 3],  // P0
        cleaned_points[n - 2],  // P1
        cleaned_points[n - 1],  // P2
        cleaned_points[n - 1],  // P3 (phantom)
        config.alpha,
        config.segments_per_curve,
        result
    );

    // Ensure the last point is included
    result.push_back(cleaned_points[n - 1]);

    return result;
}

void StrokeSmoother::InterpolateCurveSegment(
    const ImVec2& P0,
    const ImVec2& P1,
    const ImVec2& P2,
    const ImVec2& P3,
    float alpha,
    int segments,
    std::vector<ImVec2>& output
) {
    // Generate interpolated points between P1 and P2
    for (int i = 0; i < segments; ++i) {
        float t = static_cast<float>(i) / segments;
        ImVec2 point = EvaluateCatmullRom(P0, P1, P2, P3, t);
        output.push_back(point);
    }
}

ImVec2 StrokeSmoother::EvaluateCatmullRom(
    const ImVec2& P0,
    const ImVec2& P1,
    const ImVec2& P2,
    const ImVec2& P3,
    float t
) {
    // Catmull-Rom spline formula (uniform variant):
    // P(t) = 0.5 * [
    //   (2*P1) +
    //   (-P0 + P2) * t +
    //   (2*P0 - 5*P1 + 4*P2 - P3) * t^2 +
    //   (-P0 + 3*P1 - 3*P2 + P3) * t^3
    // ]

    float t2 = t * t;
    float t3 = t2 * t;

    ImVec2 result;

    // X component
    result.x = 0.5f * (
        (2.0f * P1.x) +
        (-P0.x + P2.x) * t +
        (2.0f * P0.x - 5.0f * P1.x + 4.0f * P2.x - P3.x) * t2 +
        (-P0.x + 3.0f * P1.x - 3.0f * P2.x + P3.x) * t3
    );

    // Y component
    result.y = 0.5f * (
        (2.0f * P1.y) +
        (-P0.y + P2.y) * t +
        (2.0f * P0.y - 5.0f * P1.y + 4.0f * P2.y - P3.y) * t2 +
        (-P0.y + 3.0f * P1.y - 3.0f * P2.y + P3.y) * t3
    );

    return result;
}

std::vector<ImVec2> StrokeSmoother::RemoveDuplicates(
    const std::vector<ImVec2>& points,
    float min_distance
) {
    if (points.empty()) {
        return points;
    }

    std::vector<ImVec2> result;
    result.reserve(points.size());

    result.push_back(points[0]);

    for (size_t i = 1; i < points.size(); ++i) {
        if (Distance(result.back(), points[i]) >= min_distance) {
            result.push_back(points[i]);
        }
    }

    return result;
}

float StrokeSmoother::Distance(const ImVec2& a, const ImVec2& b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace Annotations
} // namespace ump
