#include "ink_stroke_modeler_wrapper.h"

#include "ink_stroke_modeler/stroke_modeler.h"
#include "ink_stroke_modeler/params.h"
#include "ink_stroke_modeler/types.h"

namespace ump {
namespace Annotations {

InkStrokeModelerWrapper::InkStrokeModelerWrapper()
    : modeler_(std::make_unique<ink::stroke_model::StrokeModeler>()) {
}

InkStrokeModelerWrapper::~InkStrokeModelerWrapper() = default;

InkStrokeModelerWrapper::InkStrokeModelerWrapper(InkStrokeModelerWrapper&&) noexcept = default;
InkStrokeModelerWrapper& InkStrokeModelerWrapper::operator=(InkStrokeModelerWrapper&&) noexcept = default;

void InkStrokeModelerWrapper::BeginStroke(const Config& config) {
    ink::stroke_model::StrokeModelParams params;

    // Configure wobble smoother - reduces high-frequency noise at slow speeds
    params.wobble_smoother_params.is_enabled = true;
    params.wobble_smoother_params.timeout = ink::stroke_model::Duration(config.wobble_timeout_seconds);
    params.wobble_smoother_params.speed_floor = config.wobble_speed_floor;
    params.wobble_smoother_params.speed_ceiling = config.wobble_speed_ceiling;

    // Configure position modeler (spring-mass simulation)
    params.position_modeler_params.spring_mass_constant = config.spring_mass_constant;
    params.position_modeler_params.drag_constant = config.drag_constant;

    // Configure sampling parameters
    params.sampling_params.min_output_rate = 180.0;  // Output rate in points per second
    params.sampling_params.end_of_stroke_stopping_distance = 0.001f * kModelScale;
    params.sampling_params.end_of_stroke_max_iterations = 20;

    // Configure prediction (disabled by default for annotation accuracy)
    if (!config.enable_prediction) {
        params.prediction_params = ink::stroke_model::DisabledPredictorParams{};
    } else {
        // Use Kalman predictor for lower latency when enabled
        ink::stroke_model::KalmanPredictorParams kalman_params;
        kalman_params.prediction_interval = ink::stroke_model::Duration(0.02);  // 20ms lookahead
        params.prediction_params = kalman_params;
    }

    auto status = modeler_->Reset(params);
    is_active_ = status.ok();

    if (!status.ok()) {
        // Log error but don't throw - fall back to raw input
        is_active_ = false;
    }
}

std::vector<ImVec2> InkStrokeModelerWrapper::AddPoint(
    const ImVec2& normalized_pos,
    double timestamp,
    bool is_down
) {
    std::vector<ImVec2> result;

    if (!is_active_ || !modeler_) {
        // Fallback: return the input point directly if modeler isn't active
        result.push_back(normalized_pos);
        return result;
    }

    // Create input event
    ink::stroke_model::Input input;
    input.event_type = is_down
        ? ink::stroke_model::Input::EventType::kDown
        : ink::stroke_model::Input::EventType::kMove;

    ImVec2 model_pos = ToModelSpace(normalized_pos);
    input.position = {model_pos.x, model_pos.y};
    input.time = ink::stroke_model::Time(timestamp);
    input.pressure = -1;       // Not reported (use -1 for "not available")
    input.tilt = -1;           // Not reported
    input.orientation = -1;    // Not reported

    // Process input through the modeler
    std::vector<ink::stroke_model::Result> modeler_results;
    auto status = modeler_->Update(input, modeler_results);

    if (status.ok()) {
        result.reserve(modeler_results.size());
        for (const auto& r : modeler_results) {
            result.push_back(FromModelSpace(r.position.x, r.position.y));
        }
    } else {
        // On error, fall back to raw input
        result.push_back(normalized_pos);
    }

    return result;
}

std::vector<ImVec2> InkStrokeModelerWrapper::EndStroke(
    const ImVec2& final_pos,
    double timestamp
) {
    std::vector<ImVec2> result;

    if (!is_active_ || !modeler_) {
        result.push_back(final_pos);
        is_active_ = false;
        return result;
    }

    // Create pen-up event
    ink::stroke_model::Input input;
    input.event_type = ink::stroke_model::Input::EventType::kUp;

    ImVec2 model_pos = ToModelSpace(final_pos);
    input.position = {model_pos.x, model_pos.y};
    input.time = ink::stroke_model::Time(timestamp);
    input.pressure = -1;
    input.tilt = -1;
    input.orientation = -1;

    // Process the up event
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

std::vector<ImVec2> InkStrokeModelerWrapper::GetPrediction() const {
    std::vector<ImVec2> result;

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

ImVec2 InkStrokeModelerWrapper::ToModelSpace(const ImVec2& normalized) const {
    return ImVec2(normalized.x * kModelScale, normalized.y * kModelScale);
}

ImVec2 InkStrokeModelerWrapper::FromModelSpace(float x, float y) const {
    return ImVec2(x / kModelScale, y / kModelScale);
}

} // namespace Annotations
} // namespace ump
