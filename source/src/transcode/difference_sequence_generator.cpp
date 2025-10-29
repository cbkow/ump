#include "difference_sequence_generator.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

// STB image for PNG I/O (implementations in main.cpp and video_player.cpp)
#include "../../external/glfw/deps/stb_image.h"
#include "../../external/glfw/deps/stb_image_write.h"

namespace fs = std::filesystem;

namespace ump {

// ============================================================================
// Constructor / Destructor
// ============================================================================

DifferenceSequenceGenerator::DifferenceSequenceGenerator() {
    Debug::Log("DifferenceSequenceGenerator: Constructor");
}

DifferenceSequenceGenerator::~DifferenceSequenceGenerator() {
    Debug::Log("DifferenceSequenceGenerator: Destructor");
    CancelGeneration();
}

// ============================================================================
// Generation Control
// ============================================================================

bool DifferenceSequenceGenerator::StartGeneration(
    const std::string& primary_cache_dir,
    const std::string& comparison_cache_dir,
    const std::string& output_cache_dir,
    int total_frames,
    float amplification)
{
    Debug::Log("DifferenceSequenceGenerator: Starting generation");
    Debug::Log("  Primary: " + primary_cache_dir);
    Debug::Log("  Comparison: " + comparison_cache_dir);
    Debug::Log("  Output: " + output_cache_dir);
    Debug::Log("  Frames: " + std::to_string(total_frames));
    Debug::Log("  Amplification: " + std::to_string(amplification));

    if (generating_) {
        Debug::Log("ERROR: Already generating");
        return false;
    }

    // Store config
    primary_cache_dir_ = primary_cache_dir;
    comparison_cache_dir_ = comparison_cache_dir;
    output_cache_dir_ = output_cache_dir;
    total_frames_ = total_frames;
    amplification_ = amplification;

    // Create output directory
    try {
        fs::create_directories(output_cache_dir_);
    } catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to create output directory: " + std::string(e.what()));
        return false;
    }

    // Start generation thread
    generating_ = true;
    complete_ = false;
    cancel_requested_ = false;
    progress_ = 0.0f;

    generation_thread_ = std::thread(&DifferenceSequenceGenerator::GenerationWorker, this);

    return true;
}

void DifferenceSequenceGenerator::CancelGeneration() {
    Debug::Log("DifferenceSequenceGenerator: Cancel requested");
    cancel_requested_ = true;

    if (generation_thread_.joinable()) {
        generation_thread_.join();
    }

    generating_ = false;
}

// ============================================================================
// Worker Thread
// ============================================================================

void DifferenceSequenceGenerator::GenerationWorker() {
    Debug::Log("DifferenceSequenceGenerator: Worker thread started");

    int frames_processed = 0;

    for (int frame = 0; frame < total_frames_; ++frame) {
        if (cancel_requested_) {
            Debug::Log("DifferenceSequenceGenerator: Cancelled by user");
            generating_ = false;
            return;
        }

        // Generate difference for this frame
        if (!GenerateDifferenceFrame(frame)) {
            Debug::Log("ERROR: Failed to generate frame " + std::to_string(frame));
            generating_ = false;
            return;
        }

        frames_processed++;
        progress_ = (float)frames_processed / (float)total_frames_;

        // Call progress callback
        if (progress_callback_) {
            progress_callback_(progress_);
        }

        // Log progress periodically
        if (frames_processed % 100 == 0) {
            Debug::Log("DifferenceSequenceGenerator: Progress " + std::to_string((int)(progress_ * 100)) + "%");
        }
    }

    Debug::Log("DifferenceSequenceGenerator: Generation complete!");
    generating_ = false;
    complete_ = true;
    progress_ = 1.0f;

    // Call completion callback
    if (completion_callback_) {
        completion_callback_();
    }
}

bool DifferenceSequenceGenerator::GenerateDifferenceFrame(int frame_number) {
    // Build frame paths
    std::stringstream ss_primary, ss_comparison, ss_output;
    ss_primary << primary_cache_dir_ << "/frame_" << std::setfill('0') << std::setw(5) << frame_number << ".png";
    ss_comparison << comparison_cache_dir_ << "/frame_" << std::setfill('0') << std::setw(5) << frame_number << ".png";
    ss_output << output_cache_dir_ << "/frame_" << std::setfill('0') << std::setw(5) << frame_number << ".png";

    std::string primary_path = ss_primary.str();
    std::string comparison_path = ss_comparison.str();
    std::string output_path = ss_output.str();

    // Load primary frame
    int primary_width, primary_height, primary_channels;
    unsigned char* primary_data = LoadPNGFrame(primary_path, primary_width, primary_height, primary_channels);
    if (!primary_data) {
        Debug::Log("ERROR: Failed to load primary frame: " + primary_path);
        return false;
    }

    // Load comparison frame
    int comparison_width, comparison_height, comparison_channels;
    unsigned char* comparison_data = LoadPNGFrame(comparison_path, comparison_width, comparison_height, comparison_channels);
    if (!comparison_data) {
        Debug::Log("ERROR: Failed to load comparison frame: " + comparison_path);
        stbi_image_free(primary_data);
        return false;
    }

    // Verify dimensions match
    if (primary_width != comparison_width || primary_height != comparison_height) {
        Debug::Log("ERROR: Frame dimensions mismatch at frame " + std::to_string(frame_number));
        stbi_image_free(primary_data);
        stbi_image_free(comparison_data);
        return false;
    }

    // Allocate output buffer (RGB)
    int width = primary_width;
    int height = primary_height;
    int output_channels = 3;  // RGB
    unsigned char* output_data = new unsigned char[width * height * output_channels];

    // Compute difference (CPU-based)
    // diff = abs(primary - comparison) * amplification
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int primary_idx = (y * width + x) * primary_channels;
            int comparison_idx = (y * width + x) * comparison_channels;
            int output_idx = (y * width + x) * output_channels;

            for (int c = 0; c < 3; ++c) {  // RGB only
                float primary_val = (float)primary_data[primary_idx + c] / 255.0f;
                float comparison_val = (float)comparison_data[comparison_idx + c] / 255.0f;
                float diff = std::abs(primary_val - comparison_val) * amplification_;

                // Clamp to [0, 1]
                diff = (std::min)(1.0f, (std::max)(0.0f, diff));

                output_data[output_idx + c] = (unsigned char)(diff * 255.0f);
            }
        }
    }

    // Save output frame
    bool success = SavePNGFrame(output_path, output_data, width, height, output_channels);

    // Cleanup
    stbi_image_free(primary_data);
    stbi_image_free(comparison_data);
    delete[] output_data;

    return success;
}

// ============================================================================
// Helper Functions
// ============================================================================

unsigned char* DifferenceSequenceGenerator::LoadPNGFrame(const std::string& path, int& width, int& height, int& channels) {
    // Load PNG using stb_image
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 0);
    if (!data) {
        Debug::Log("ERROR: stbi_load failed: " + path);
        return nullptr;
    }
    return data;
}

bool DifferenceSequenceGenerator::SavePNGFrame(const std::string& path, unsigned char* data, int width, int height, int channels) {
    // Save PNG using stb_image_write
    int stride_in_bytes = width * channels;
    int result = stbi_write_png(path.c_str(), width, height, channels, data, stride_in_bytes);
    if (result == 0) {
        Debug::Log("ERROR: stbi_write_png failed: " + path);
        return false;
    }
    return true;
}

} // namespace ump
