#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace ump {

/**
 * DifferenceSequenceGenerator
 *
 * Generates pre-computed difference PNG sequences from two transcoded video sequences.
 * This eliminates runtime compositing overhead by creating a third cached sequence.
 *
 * Algorithm (CPU-based, no OpenGL required):
 *   For each frame:
 *     1. Load primary_frame_N.png from disk
 *     2. Load comparison_frame_N.png from disk
 *     3. Compute: diff_pixel = abs(primary_pixel - comparison_pixel) * amplification
 *     4. Save difference_frame_N.png to combined cache directory
 *
 * Usage:
 *   DifferenceSequenceGenerator gen;
 *   gen.SetProgressCallback([](float p) { printf("%.1f%%\n", p * 100); });
 *   gen.Generate(primary_dir, comparison_dir, output_dir, total_frames, amplification);
 */
class DifferenceSequenceGenerator {
public:
    DifferenceSequenceGenerator();
    ~DifferenceSequenceGenerator();

    // Generate difference sequence (async)
    bool StartGeneration(
        const std::string& primary_cache_dir,
        const std::string& comparison_cache_dir,
        const std::string& output_cache_dir,
        int total_frames,
        float amplification = 5.0f
    );

    // Cancel in-progress generation
    void CancelGeneration();

    // Status
    bool IsGenerating() const { return generating_; }
    bool IsComplete() const { return complete_; }
    float GetProgress() const { return progress_.load(); }

    // Callbacks
    void SetProgressCallback(std::function<void(float)> callback) { progress_callback_ = callback; }
    void SetCompletionCallback(std::function<void()> callback) { completion_callback_ = callback; }

    // Get output directory
    std::string GetOutputDirectory() const { return output_cache_dir_; }

private:
    // Worker thread
    void GenerationWorker();

    // Process single frame pair
    bool GenerateDifferenceFrame(int frame_number);

    // Helper: Load PNG from disk
    unsigned char* LoadPNGFrame(const std::string& path, int& width, int& height, int& channels);

    // Helper: Save PNG to disk
    bool SavePNGFrame(const std::string& path, unsigned char* data, int width, int height, int channels);

    // Configuration
    std::string primary_cache_dir_;
    std::string comparison_cache_dir_;
    std::string output_cache_dir_;
    int total_frames_ = 0;
    float amplification_ = 5.0f;

    // State
    std::atomic<bool> generating_{false};
    std::atomic<bool> complete_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<float> progress_{0.0f};

    // Threading
    std::thread generation_thread_;

    // Callbacks
    std::function<void(float)> progress_callback_;
    std::function<void()> completion_callback_;
};

} // namespace ump
