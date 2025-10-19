#pragma once
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <glad/gl.h>
#else
#include <GL/gl.h>
#endif

class CooperativeGPUScheduler {
private:
    std::chrono::steady_clock::time_point last_yield;
    std::chrono::steady_clock::time_point frame_start;
    float adaptive_yield_threshold = 25.0f; // Threshold
    int consecutive_heavy_frames = 0;
    bool initialized = false;

public:
    void BeginFrame();
    bool ShouldYield();
    void CooperativeYield();
    bool IsGPUContended();
    void Reset();
    float GetCurrentFrameTime() const;
};