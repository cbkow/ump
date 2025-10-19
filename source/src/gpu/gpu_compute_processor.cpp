#include "gpu_compute_processor.h"
#include "../utils/debug_utils.h"
// OLD: #include "../player/exr_frame_cache.h" - Removed (using DirectEXRCache now)
#include <chrono>

namespace ump {

    GPUComputeProcessor::GPUComputeProcessor() {
        Debug::Log("GPUComputeProcessor: Constructor called");
    }

    GPUComputeProcessor::~GPUComputeProcessor() {
        Shutdown();
    }

    bool GPUComputeProcessor::Initialize(const GPUComputeConfig& config) {
        if (is_initialized_) {
            Debug::Log("GPUComputeProcessor: Already initialized");
            return true;
        }

        if (!config.IsValid()) {
            Debug::Log("GPUComputeProcessor: Invalid configuration");
            return false;
        }

        config_ = config;

        if (!config_.enable_compute_acceleration) {
            Debug::Log("GPUComputeProcessor: GPU compute acceleration disabled in config");
            return false;
        }

        // For now, this is a CPU fallback implementation
        // TODO: Add OpenCL/CUDA support when libraries are available
        Debug::Log("GPUComputeProcessor: Initialized with CPU fallback (GPU compute not yet implemented)");
        is_initialized_ = true;
        return true;
    }

    void GPUComputeProcessor::Shutdown() {
        if (!is_initialized_) {
            return;
        }

        is_initialized_ = false;
        Debug::Log("GPUComputeProcessor: Shutdown complete");
    }

    bool GPUComputeProcessor::ProcessFrame(GLuint& gpu_texture) {
        if (!is_initialized_) {
            return false;
        }

        // Stub - not implemented yet
        return false;
    }

    void GPUComputeProcessor::SetConfig(const GPUComputeConfig& config) {
        if (config.IsValid()) {
            config_ = config;
        }
    }

    GPUComputeStats GPUComputeProcessor::GetStats() const {
        return stats_;
    }

    void GPUComputeProcessor::ResetStats() {
        stats_ = GPUComputeStats{};
    }

    bool GPUComputeProcessor::IsGPUComputeAvailable() {
        // TODO: Check for OpenCL/CUDA availability
        return false;
    }

    std::string GPUComputeProcessor::GetSystemInfo() {
        std::string info = "GPU Compute: Not yet implemented (CPU fallback active)";

        // TODO: Add actual GPU detection
        // Example: "OpenCL: Available, CUDA: Available on RTX 4080"

        return info;
    }

    void GPUComputeProcessor::UpdateStats(size_t processing_time_ms) {
        stats_.frames_processed++;
        stats_.total_processing_time_ms += processing_time_ms;
        stats_.average_processing_time_ms = stats_.total_processing_time_ms / stats_.frames_processed;
        stats_.gpu_active = false; // CPU fallback for now
        stats_.opencl_available = false; // Not implemented yet
        stats_.cuda_available = false; // Not implemented yet
        stats_.active_device_name = "CPU Fallback";
    }

} // namespace ump