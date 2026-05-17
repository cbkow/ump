// InkStrokeModelerWrapper — see header for adaptation notes.

#include "ink_stroke_modeler_wrapper.h"

#include "ink_stroke_modeler/params.h"
#include "ink_stroke_modeler/stroke_modeler.h"
#include "ink_stroke_modeler/types.h"

namespace qcv {

InkStrokeModelerWrapper::InkStrokeModelerWrapper()
    : modeler_(std::make_unique<ink::stroke_model::StrokeModeler>())
{
}

InkStrokeModelerWrapper::~InkStrokeModelerWrapper() = default;

InkStrokeModelerWrapper::InkStrokeModelerWrapper(InkStrokeModelerWrapper&&) noexcept            = default;
InkStrokeModelerWrapper& InkStrokeModelerWrapper::operator=(InkStrokeModelerWrapper&&) noexcept = default;

void InkStrokeModelerWrapper::BeginStroke(const Config& config)
{
    ink::stroke_model::StrokeModelParams params;

    params.wobble_smoother_params.is_enabled    = true;
    params.wobble_smoother_params.timeout       = ink::stroke_model::Duration(config.wobble_timeout_seconds);
    params.wobble_smoother_params.speed_floor   = config.wobble_speed_floor;
    params.wobble_smoother_params.speed_ceiling = config.wobble_speed_ceiling;

    params.position_modeler_params.spring_mass_constant = config.spring_mass_constant;
    params.position_modeler_params.drag_constant        = config.drag_constant;

    // 300 Hz output rate. The old app used 180 Hz; bumping this
    // packs the round caps tighter so the connecting quads are
    // visually shorter at fast drag speeds (less perceptible
    // straight-segment look between caps).
    params.sampling_params.min_output_rate                = 300.0;
    params.sampling_params.end_of_stroke_stopping_distance = 0.001f * kModelScale;
    params.sampling_params.end_of_stroke_max_iterations    = 20;

    if (!config.enable_prediction) {
        params.prediction_params = ink::stroke_model::DisabledPredictorParams{};
    } else {
        ink::stroke_model::KalmanPredictorParams kalman_params;
        kalman_params.prediction_interval = ink::stroke_model::Duration(0.02);  // 20ms lookahead
        params.prediction_params          = kalman_params;
    }

    auto status = modeler_->Reset(params);
    is_active_  = status.ok();
}

std::vector<QPointF> InkStrokeModelerWrapper::AddPoint(
    const QPointF& normalized_pos,
    double timestamp,
    bool is_down)
{
    std::vector<QPointF> result;

    if (!is_active_ || !modeler_) {
        // Fallback: pass raw input through if the modeler isn't healthy.
        result.push_back(normalized_pos);
        return result;
    }

    ink::stroke_model::Input input;
    input.event_type = is_down
        ? ink::stroke_model::Input::EventType::kDown
        : ink::stroke_model::Input::EventType::kMove;

    const QPointF model_pos = ToModelSpace(normalized_pos);
    input.position    = {static_cast<float>(model_pos.x()),
                         static_cast<float>(model_pos.y())};
    input.time        = ink::stroke_model::Time(timestamp);
    input.pressure    = -1;
    input.tilt        = -1;
    input.orientation = -1;

    std::vector<ink::stroke_model::Result> modeler_results;
    auto status = modeler_->Update(input, modeler_results);

    if (status.ok()) {
        result.reserve(modeler_results.size());
        for (const auto& r : modeler_results) {
            result.push_back(FromModelSpace(r.position.x, r.position.y));
        }
    } else {
        result.push_back(normalized_pos);
    }

    return result;
}

std::vector<QPointF> InkStrokeModelerWrapper::EndStroke(
    const QPointF& final_pos,
    double timestamp)
{
    std::vector<QPointF> result;

    if (!is_active_ || !modeler_) {
        result.push_back(final_pos);
        is_active_ = false;
        return result;
    }

    ink::stroke_model::Input input;
    input.event_type = ink::stroke_model::Input::EventType::kUp;

    const QPointF model_pos = ToModelSpace(final_pos);
    input.position    = {static_cast<float>(model_pos.x()),
                         static_cast<float>(model_pos.y())};
    input.time        = ink::stroke_model::Time(timestamp);
    input.pressure    = -1;
    input.tilt        = -1;
    input.orientation = -1;

    std::vector<ink::stroke_model::Result> modeler_results;
    auto status = modeler_->Update(input, modeler_results);

    if (status.ok()) {
        result.reserve(modeler_results.size());
        for (const auto& r : modeler_results) {
            result.push_back(FromModelSpace(r.position.x, r.position.y));
        }
    } else {
        result.push_back(final_pos);
    }

    is_active_ = false;
    return result;
}

std::vector<QPointF> InkStrokeModelerWrapper::GetPrediction() const
{
    std::vector<QPointF> result;

    if (!is_active_ || !modeler_) {
        return result;
    }

    std::vector<ink::stroke_model::Result> predicted;
    auto status = modeler_->Predict(predicted);

    if (status.ok()) {
        result.reserve(predicted.size());
        for (const auto& r : predicted) {
            result.push_back(FromModelSpace(r.position.x, r.position.y));
        }
    }

    return result;
}

QPointF InkStrokeModelerWrapper::ToModelSpace(const QPointF& normalized) const
{
    return QPointF(normalized.x() * kModelScale,
                   normalized.y() * kModelScale);
}

QPointF InkStrokeModelerWrapper::FromModelSpace(float x, float y) const
{
    return QPointF(static_cast<qreal>(x) / kModelScale,
                   static_cast<qreal>(y) / kModelScale);
}

} // namespace qcv
