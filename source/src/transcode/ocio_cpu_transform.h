#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <string>
#include <vector>
#include <memory>

#include "../player/image_loader_interface.h"

namespace OCIO = OCIO_NAMESPACE;

namespace ump {

/**
 * OCIOCPUTransform
 *
 * CPU-based OCIO color transformation for transcode operations.
 *
 * Reuses the same transform building logic as OCIOPipeline but uses
 * OCIO's CPU processor instead of generating GPU shaders.
 *
 * This allows color-accurate transcodes without needing OpenGL context
 * per worker thread.
 */
class OCIOCPUTransform {
public:
    OCIOCPUTransform();
    ~OCIOCPUTransform();

    /**
     * Initialize transform from OCIO configuration
     *
     * Mirrors OCIOPipeline::BuildFromDescription() but for CPU processing.
     *
     * @param src_colorspace Source colorspace (e.g., "ACES2065-1", "Linear Rec.709")
     * @param display Display name (e.g., "sRGB", "Rec.1886")
     * @param view View name (e.g., "Standard", "Raw")
     * @param looks Optional looks (comma-separated)
     * @param scene_lut_files LUT files applied BEFORE display transform
     * @param display_lut_files LUT files applied AFTER display transform
     * @return true if successful
     */
    bool Initialize(
        const std::string& src_colorspace,
        const std::string& display,
        const std::string& view,
        const std::string& looks = "",
        const std::vector<std::string>& scene_lut_files = {},
        const std::vector<std::string>& display_lut_files = {}
    );

    /**
     * Apply transform to RGBA float pixels (in-place)
     *
     * @param rgba_data Pointer to RGBA float data (4 components per pixel)
     * @param width Image width
     * @param height Image height
     */
    void Apply(float* rgba_data, int width, int height);

    /**
     * Apply transform and convert to 8-bit RGBA
     *
     * Convenience method that handles float → uint8 conversion.
     * Allocates new buffer.
     *
     * @param input Input pixel data (any format - will be converted to float)
     * @param output_width Output width (-1 = same as input)
     * @param output_height Output height (-1 = same as input)
     * @return RGBA8 pixel buffer ready for FFMPEG encoder
     */
    std::vector<uint8_t> ApplyToUInt8(
        const PixelData& input,
        int output_width = -1,
        int output_height = -1
    );

    /**
     * Check if transform is valid and ready to use
     */
    bool IsValid() const { return is_valid_; }

    /**
     * Get processor info (for debugging)
     */
    std::string GetProcessorInfo() const;

private:
    // Create passthrough transform (fallback)
    bool CreatePassthroughTransform();

    // Convert PixelData to RGBA float (internal helper)
    std::vector<float> ConvertToRGBAFloat(const PixelData& input) const;

    // Resize image (bilinear interpolation)
    std::vector<float> ResizeImage(
        const float* src, int src_width, int src_height,
        int dst_width, int dst_height
    ) const;

    OCIO::ConstConfigRcPtr config_;
    OCIO::ConstProcessorRcPtr processor_;
    OCIO::ConstCPUProcessorRcPtr cpu_processor_;

    bool is_valid_ = false;
};

} // namespace ump
