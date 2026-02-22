#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace OCIO = OCIO_NAMESPACE;

namespace qcview {

/**
 * OCIOLutBaker
 *
 * Bakes OCIO color transforms into .cube LUT files for FFmpeg's lut3d filter.
 * This allows video file transcoding to use FFmpeg's native LUT application
 * instead of per-frame OCIO CPU transforms.
 *
 * LUTs are cached in %LOCALAPPDATA%\qcview\LUT\ with readable filenames based
 * on the transform parameters.
 */
class OCIOLutBaker {
public:
    /**
     * Configuration for LUT baking.
     * Mirrors the transform parameters used by OCIOCPUTransform.
     */
    struct BakeConfig {
        std::string src_colorspace;    // Source colorspace (e.g., "ACES2065-1")
        std::string display;           // Display device (e.g., "sRGB")
        std::string view;              // View transform (e.g., "Standard")
        std::string looks;             // Optional looks (comma-separated)
        std::vector<std::string> scene_luts;    // LUT files BEFORE display transform
        std::vector<std::string> display_luts;  // LUT files AFTER display transform
        int cube_size = 65;            // 65x65x65 3D LUT (good quality/size tradeoff)
    };

    /**
     * Get LUT cache directory (%LOCALAPPDATA%\qcview\LUT\)
     * Creates directory if it doesn't exist.
     *
     * @return Path to LUT cache directory, or empty string on failure
     */
    static std::string GetLutCacheDir();

    /**
     * Generate readable filename from config.
     *
     * Format: {src}_{display}_{view}_{looks}_{sceneluts}_{displayluts}.cube
     * Example: "ACES2065-1_sRGB_Standard_none_none_none.cube"
     *
     * @param config Transform configuration
     * @return Generated filename
     */
    static std::string GenerateLutFilename(const BakeConfig& config);

    /**
     * Get full path for cached LUT.
     * Combines GetLutCacheDir() + GenerateLutFilename().
     *
     * @param config Transform configuration
     * @return Full path to LUT file
     */
    static std::string GetCachedLutPath(const BakeConfig& config);

    /**
     * Check if cached LUT already exists.
     *
     * @param config Transform configuration
     * @return true if LUT file exists
     */
    static bool HasCachedLut(const BakeConfig& config);

    /**
     * Check if any transform is applied (not identity).
     * Returns false if no colorspace/display/view are set.
     *
     * @param config Transform configuration
     * @return true if transform would change pixels
     */
    static bool HasTransform(const BakeConfig& config);

    /**
     * Bake transform to .cube file, or return cached path if exists.
     *
     * This is the main entry point. It will:
     * 1. Check if cached LUT exists -> return path
     * 2. Build OCIO GroupTransform from config
     * 3. Sample processor at cube_size^3 grid points
     * 4. Write .cube file to cache directory
     *
     * @param config Transform configuration
     * @param progress_callback Optional callback for progress (0.0 to 1.0)
     * @param cancel_flag Optional atomic flag to cancel baking
     * @return Full path to LUT file, or empty string on failure
     */
    static std::string BakeOrGetCached(
        const BakeConfig& config,
        std::function<void(float)> progress_callback = nullptr,
        std::atomic<bool>* cancel_flag = nullptr
    );

    /**
     * Bake transform to a user-specified .cube file path.
     *
     * Unlike BakeOrGetCached, this always writes to the specified path
     * (no caching). Used for "Export LUT" functionality.
     *
     * @param config Transform configuration
     * @param output_path Full path for output .cube file
     * @param progress_callback Optional callback for progress (0.0 to 1.0)
     * @param cancel_flag Optional atomic flag to cancel baking
     * @return output_path on success, or empty string on failure
     */
    static std::string BakeTo(
        const BakeConfig& config,
        const std::string& output_path,
        std::function<void(float)> progress_callback = nullptr,
        std::atomic<bool>* cancel_flag = nullptr
    );

private:
    /**
     * Sanitize string for use in filename.
     * Replaces special characters with underscores.
     */
    static std::string SanitizeForFilename(const std::string& str);

    /**
     * Join LUT filenames for use in cache key.
     * Extracts just the filename (not full path) from each LUT.
     */
    static std::string JoinLutNames(const std::vector<std::string>& luts);

    /**
     * Build OCIO GroupTransform from config.
     * Same logic as OCIOCPUTransform::Initialize().
     *
     * @param config Transform configuration
     * @param ocio_config OCIO config to use
     * @return GroupTransform, or nullptr on failure
     */
    static OCIO::GroupTransformRcPtr BuildTransform(
        const BakeConfig& config,
        OCIO::ConstConfigRcPtr ocio_config
    );

    /**
     * Write 3D LUT to .cube file.
     *
     * @param processor CPU processor to sample
     * @param output_path Output .cube file path
     * @param cube_size Size of 3D LUT (e.g., 65 = 65x65x65)
     * @param progress_callback Optional progress callback
     * @param cancel_flag Optional cancel flag
     * @return true if successful
     */
    static bool WriteCubeFile(
        OCIO::ConstCPUProcessorRcPtr processor,
        const std::string& output_path,
        int cube_size,
        std::function<void(float)> progress_callback,
        std::atomic<bool>* cancel_flag
    );
};

} // namespace qcview
