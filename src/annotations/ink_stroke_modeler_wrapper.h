// InkStrokeModelerWrapper — direct lift from old QCView's
// src/annotations/ink_stroke_modeler_wrapper.{h,cpp} per Guide 19 §2.3.
//
// Wraps Google's ink::stroke_model::StrokeModeler (real-time stroke
// smoothing). The modeler combines:
//   - Wobble smoother (low-pass on slow input)
//   - Spring-mass position model (drag toward raw input)
//   - Resampler (consistent output rate independent of input cadence)
// See https://github.com/google/ink-stroke-modeler.
//
// Coordinates are scaled into 0..kModelScale (1000.0) before feeding the
// modeler — its internal constants assume "screen-ish" units, not
// normalized 0..1, so 0..1 inputs lose precision. Output is scaled back.
//
// Adaptation notes vs old app:
//   - Namespace qcview::Annotations → qcv
//   - ImVec2 → QPointF
//   - Same Config defaults as old app (tuned for annotation accuracy,
//     not low-latency stylus prediction; prediction off by default)

#pragma once

#include <QPointF>

#include <memory>
#include <vector>

namespace ink::stroke_model { class StrokeModeler; }

namespace qcv {

class InkStrokeModelerWrapper {
public:
    struct Config {
        // Wobble smoother — low-pass at slow speeds. Defaults are the
        // library's recommended starting point for finger / mouse input.
        float wobble_timeout_seconds = 0.04f;
        float wobble_speed_floor     = 31.0f;
        float wobble_speed_ceiling   = 620.0f;

        // Position modeler (spring-mass simulation).
        float spring_mass_constant = 11.0f / 32400.0f;
        float drag_constant        = 72.0f;

        // Predictor (off by default — annotation accuracy > input lag).
        bool  enable_prediction = false;
    };

    InkStrokeModelerWrapper();
    ~InkStrokeModelerWrapper();

    InkStrokeModelerWrapper(const InkStrokeModelerWrapper&)            = delete;
    InkStrokeModelerWrapper& operator=(const InkStrokeModelerWrapper&) = delete;
    InkStrokeModelerWrapper(InkStrokeModelerWrapper&&) noexcept;
    InkStrokeModelerWrapper& operator=(InkStrokeModelerWrapper&&) noexcept;

    void BeginStroke(const Config& config);

    // Inputs are in normalized 0..1 viewport coordinates. Outputs are
    // resampled, smoothed points (zero or more per call).
    std::vector<QPointF> AddPoint(const QPointF& normalized_pos,
                                  double timestamp,
                                  bool is_down);

    std::vector<QPointF> EndStroke(const QPointF& final_pos,
                                   double timestamp);

    std::vector<QPointF> GetPrediction() const;

    bool IsActive() const { return is_active_; }

private:
    QPointF ToModelSpace(const QPointF& normalized) const;
    QPointF FromModelSpace(float x, float y) const;

    static constexpr float kModelScale = 1000.0f;

    std::unique_ptr<ink::stroke_model::StrokeModeler> modeler_;
    bool is_active_ = false;
};

} // namespace qcv
