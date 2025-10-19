#include "gpu_scheduler.h"
#include "debug_utils.h"

void CooperativeGPUScheduler::BeginFrame() {
    frame_start = std::chrono::steady_clock::now();
    if (!initialized) {
        last_yield = frame_start;
        initialized = true;
    }
}

bool CooperativeGPUScheduler::ShouldYield() {
    if (!initialized) return false;

    float current_frame_time = GetCurrentFrameTime();
    return current_frame_time > adaptive_yield_threshold;
}

void CooperativeGPUScheduler::CooperativeYield() {
    if (!initialized) return;

    float frame_time = GetCurrentFrameTime();

    // Only yield if we're actually taking too long
    if (frame_time > adaptive_yield_threshold) {
        glFinish(); // Complete pending GPU work
        std::this_thread::sleep_for(std::chrono::microseconds(25)); // Much shorter yield
        consecutive_heavy_frames++;

        // Log when we're being cooperative (occasionally)
        if (consecutive_heavy_frames % 60 == 1) { // Log every ~1 second at 60fps
            //Debug::Log("GPU cooperation active - frame time: " + std::to_string(frame_time) + "ms");
        }

        if (consecutive_heavy_frames > 30) { // The wait
            adaptive_yield_threshold *= 0.99f; // The threshold
        }
    }
    else {
        consecutive_heavy_frames = (std::max)(0, consecutive_heavy_frames - 2);
        if (consecutive_heavy_frames == 0) {
            adaptive_yield_threshold = 25.0f; // Baseline
        }
    }

    last_yield = std::chrono::steady_clock::now();
}

bool CooperativeGPUScheduler::IsGPUContended() {
    return consecutive_heavy_frames > 15; // More conservative threshold
}

void CooperativeGPUScheduler::Reset() {
    consecutive_heavy_frames = 0;
    adaptive_yield_threshold = 25.0f; // Higher baseline
    initialized = false;
}

float CooperativeGPUScheduler::GetCurrentFrameTime() const {
    if (!initialized) return 0.0f;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float, std::milli>(now - frame_start).count();
}