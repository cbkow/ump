#include "ocio_cpu_transform.h"
#include "../color/ocio_config_manager.h"
#include "../utils/debug_utils.h"

#undef min
#undef max

#include <cstring>
#include <algorithm>
#include <cstdint>

// Simple half-to-float conversion (avoids Imath linking issues)
inline float HalfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exponent = (h & 0x7C00) >> 10;
    uint32_t mantissa = (h & 0x03FF);

    if (exponent == 0) {
        if (mantissa == 0) {
            // Zero
            uint32_t result = sign;
            return *reinterpret_cast<float*>(&result);
        } else {
            // Denormalized
            exponent = 1;
            while ((mantissa & 0x0400) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x03FF;
        }
    } else if (exponent == 31) {
        // Inf or NaN
        uint32_t result = sign | 0x7F800000 | (mantissa << 13);
        return *reinterpret_cast<float*>(&result);
    }

    // Normalized
    uint32_t result = sign | ((exponent + 112) << 23) | (mantissa << 13);
    return *reinterpret_cast<float*>(&result);
}

extern std::unique_ptr<OCIOConfigManager> ocio_manager;

namespace ump {

OCIOCPUTransform::OCIOCPUTransform() {
}

OCIOCPUTransform::~OCIOCPUTransform() {
}

bool OCIOCPUTransform::Initialize(
    const std::string& src_colorspace,
    const std::string& display,
    const std::string& view,
    const std::string& looks,
    const std::vector<std::string>& scene_lut_files,
    const std::vector<std::string>& display_lut_files
) {
    // Check if OCIO manager has a config
    if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
        Debug::Log("ERROR: OCIOCPUTransform - No OCIO config loaded");
        return CreatePassthroughTransform();
    }

    try {
        // Get the actual config from the manager
        config_ = ocio_manager->GetConfig();

        if (!config_) {
            Debug::Log("ERROR: OCIOCPUTransform - Could not get OCIO config from manager");
            return CreatePassthroughTransform();
        }

        // Verify the colorspace exists
        try {
            config_->getColorSpace(src_colorspace.c_str());
        } catch (OCIO::Exception& e) {
            Debug::Log("WARNING: OCIOCPUTransform - Colorspace '" + src_colorspace +
                       "' not found in config");
            return CreatePassthroughTransform();
        }

        // Verify display exists
        bool display_found = false;
        int num_displays = config_->getNumDisplays();
        for (int i = 0; i < num_displays; ++i) {
            if (std::string(config_->getDisplay(i)) == display) {
                display_found = true;
                break;
            }
        }

        if (!display_found) {
            Debug::Log("WARNING: OCIOCPUTransform - Display '" + display + "' not found in config");
            Debug::Log("Available displays in OCIO config:");
            for (int i = 0; i < num_displays; ++i) {
                Debug::Log("  [" + std::to_string(i) + "] " + std::string(config_->getDisplay(i)));
            }
            return CreatePassthroughTransform();
        }

        // Verify view exists for this display
        bool view_found = false;
        try {
            int num_views = config_->getNumViews(display.c_str());
            for (int i = 0; i < num_views; ++i) {
                const char* view_name = config_->getView(display.c_str(), i);
                if (view_name && std::string(view_name) == view) {
                    view_found = true;
                    break;
                }
            }
        } catch (OCIO::Exception& e) {
            Debug::Log("ERROR: OCIOCPUTransform - Exception checking views: " + std::string(e.what()));
        }

        if (!view_found) {
            Debug::Log("WARNING: OCIOCPUTransform - View '" + view + "' not found for display '" + display + "'");
            return CreatePassthroughTransform();
        }

        // Create transform (same logic as OCIOPipeline::BuildFromDescription)
        if (!looks.empty() || !scene_lut_files.empty() || !display_lut_files.empty()) {
            // Complex transform with looks/LUTs
            OCIO::GroupTransformRcPtr groupTransform = OCIO::GroupTransform::Create();

            // Add look transform if specified
            if (!looks.empty()) {
                OCIO::LookTransformRcPtr lookTransform = OCIO::LookTransform::Create();
                lookTransform->setSrc(src_colorspace.c_str());
                lookTransform->setLooks(looks.c_str());

                const char* lookResultColorSpace = OCIO::LookTransform::GetLooksResultColorSpace(
                    config_, config_->getCurrentContext(), looks.c_str());

                if (!lookResultColorSpace) {
                    Debug::Log("ERROR: OCIOCPUTransform - GetLooksResultColorSpace returned NULL");
                    return CreatePassthroughTransform();
                }

                lookTransform->setDst(lookResultColorSpace);
                groupTransform->appendTransform(lookTransform);

                // Scene-Referred LUTs (after looks)
                for (const auto& lut_path : scene_lut_files) {
                    try {
                        OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                        fileTransform->setSrc(lut_path.c_str());
                        fileTransform->setInterpolation(OCIO::INTERP_BEST);
                        fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                        groupTransform->appendTransform(fileTransform);
                        Debug::Log("OCIOCPUTransform: Added Scene-Referred LUT: " + lut_path);
                    } catch (OCIO::Exception& e) {
                        Debug::Log("ERROR: OCIOCPUTransform - Failed to load scene LUT '" + lut_path +
                                   "': " + std::string(e.what()));
                    }
                }

                // Display transform from look result colorspace
                OCIO::DisplayViewTransformRcPtr displayTransform = OCIO::DisplayViewTransform::Create();
                displayTransform->setSrc(lookResultColorSpace);
                displayTransform->setDisplay(display.c_str());
                displayTransform->setView(view.c_str());
                displayTransform->setLooksBypass(false);
                groupTransform->appendTransform(displayTransform);

            } else {
                // No looks, but have LUT files
                OCIO::ColorSpaceTransformRcPtr csTransform = OCIO::ColorSpaceTransform::Create();
                csTransform->setSrc(src_colorspace.c_str());
                csTransform->setDst("scene_linear");
                groupTransform->appendTransform(csTransform);

                // Scene-Referred LUTs
                for (const auto& lut_path : scene_lut_files) {
                    try {
                        OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                        fileTransform->setSrc(lut_path.c_str());
                        fileTransform->setInterpolation(OCIO::INTERP_BEST);
                        fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                        groupTransform->appendTransform(fileTransform);
                        Debug::Log("OCIOCPUTransform: Added Scene-Referred LUT: " + lut_path);
                    } catch (OCIO::Exception& e) {
                        Debug::Log("ERROR: OCIOCPUTransform - Failed to load scene LUT: " + std::string(e.what()));
                    }
                }

                // Display transform
                OCIO::DisplayViewTransformRcPtr displayTransform = OCIO::DisplayViewTransform::Create();
                displayTransform->setSrc("scene_linear");
                displayTransform->setDisplay(display.c_str());
                displayTransform->setView(view.c_str());
                groupTransform->appendTransform(displayTransform);
            }

            // Display-Referred LUTs (after display transform)
            for (const auto& lut_path : display_lut_files) {
                try {
                    OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                    fileTransform->setSrc(lut_path.c_str());
                    fileTransform->setInterpolation(OCIO::INTERP_BEST);
                    fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                    groupTransform->appendTransform(fileTransform);
                    Debug::Log("OCIOCPUTransform: Added Display-Referred LUT: " + lut_path);
                } catch (OCIO::Exception& e) {
                    Debug::Log("ERROR: OCIOCPUTransform - Failed to load display LUT: " + std::string(e.what()));
                }
            }

            processor_ = config_->getProcessor(groupTransform);

        } else {
            // Simple display-view transform (no looks/LUTs)
            OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
            transform->setSrc(src_colorspace.c_str());
            transform->setDisplay(display.c_str());
            transform->setView(view.c_str());
            processor_ = config_->getProcessor(transform);
        }

        if (!processor_) {
            Debug::Log("ERROR: OCIOCPUTransform - Failed to create OCIO processor");
            return CreatePassthroughTransform();
        }

        // Get CPU processor
        cpu_processor_ = processor_->getDefaultCPUProcessor();

        if (!cpu_processor_) {
            Debug::Log("ERROR: OCIOCPUTransform - Failed to create CPU processor");
            return CreatePassthroughTransform();
        }

        is_valid_ = true;
        Debug::Log("OCIOCPUTransform: Initialized successfully");
        Debug::Log("  Source: " + src_colorspace);
        Debug::Log("  Display: " + display + " - " + view);
        if (!looks.empty()) {
            Debug::Log("  Looks: " + looks);
        }

        return true;

    } catch (OCIO::Exception& e) {
        Debug::Log("OCIO Exception in OCIOCPUTransform::Initialize: " + std::string(e.what()));
        return CreatePassthroughTransform();
    }
}

bool OCIOCPUTransform::CreatePassthroughTransform() {
    Debug::Log("OCIOCPUTransform: Creating passthrough (no transform)");
    is_valid_ = false;  // Passthrough = no actual transform
    return false;
}

void OCIOCPUTransform::Apply(float* rgba_data, int width, int height) {
    if (!is_valid_ || !cpu_processor_) {
        // Passthrough - no transform
        return;
    }

    try {
        // Create packed image descriptor
        OCIO::PackedImageDesc img(
            rgba_data,
            width, height,
            OCIO::CHANNEL_ORDERING_RGBA
        );

        // Apply transform in-place
        cpu_processor_->apply(img);

    } catch (const std::exception& e) {
        Debug::Log("ERROR: OCIOCPUTransform::Apply exception: " + std::string(e.what()));
    }
}

std::vector<uint8_t> OCIOCPUTransform::ApplyToUInt8(
    const PixelData& input,
    int output_width,
    int output_height
) {
    // Determine output dimensions
    if (output_width <= 0) output_width = input.width;
    if (output_height <= 0) output_height = input.height;

    // Convert input to RGBA float
    std::vector<float> rgba_float = ConvertToRGBAFloat(input);

    // Resize if needed
    if (output_width != input.width || output_height != input.height) {
        rgba_float = ResizeImage(rgba_float.data(), input.width, input.height,
                                 output_width, output_height);
    }

    // Apply OCIO transform
    Apply(rgba_float.data(), output_width, output_height);

    // Convert to uint8
    std::vector<uint8_t> rgba_uint8(output_width * output_height * 4);
    for (size_t i = 0; i < rgba_float.size(); ++i) {
        float val = rgba_float[i];
        val = std::max(0.0f, std::min(1.0f, val));  // Clamp
        rgba_uint8[i] = static_cast<uint8_t>(val * 255.0f + 0.5f);
    }

    return rgba_uint8;
}

std::string OCIOCPUTransform::GetProcessorInfo() const {
    if (!processor_) {
        return "No processor";
    }

    try {
        return processor_->getCacheID();
    } catch (const std::exception& e) {
        return "Error getting processor info";
    }
}

std::vector<float> OCIOCPUTransform::ConvertToRGBAFloat(const PixelData& input) const {
    std::vector<float> rgba_float(input.width * input.height * 4);

    if (input.gl_format == GL_RGBA && input.gl_type == GL_HALF_FLOAT) {
        // Half-float to float (using custom conversion to avoid Imath linking issues)
        const uint16_t* src = reinterpret_cast<const uint16_t*>(input.pixels.data());
        for (size_t i = 0; i < rgba_float.size(); ++i) {
            rgba_float[i] = HalfToFloat(src[i]);
        }
    } else if (input.gl_format == GL_RGBA && input.gl_type == GL_UNSIGNED_BYTE) {
        // Uint8 to float
        const uint8_t* src = input.pixels.data();
        for (size_t i = 0; i < rgba_float.size(); ++i) {
            rgba_float[i] = src[i] / 255.0f;
        }
    } else if (input.gl_format == GL_RGBA && input.gl_type == GL_UNSIGNED_SHORT) {
        // Uint16 to float
        const uint16_t* src = reinterpret_cast<const uint16_t*>(input.pixels.data());
        for (size_t i = 0; i < rgba_float.size(); ++i) {
            rgba_float[i] = src[i] / 65535.0f;
        }
    } else if (input.gl_format == GL_RGBA && input.gl_type == GL_FLOAT) {
        // Already float
        std::memcpy(rgba_float.data(), input.pixels.data(), rgba_float.size() * sizeof(float));
    } else {
        Debug::Log("WARNING: OCIOCPUTransform::ConvertToRGBAFloat - Unsupported format/type");
        // Fill with zeros
        std::fill(rgba_float.begin(), rgba_float.end(), 0.0f);
    }

    return rgba_float;
}

std::vector<float> OCIOCPUTransform::ResizeImage(
    const float* src, int src_width, int src_height,
    int dst_width, int dst_height
) const {
    std::vector<float> dst(dst_width * dst_height * 4);

    // Simple bilinear interpolation
    float x_ratio = static_cast<float>(src_width - 1) / dst_width;
    float y_ratio = static_cast<float>(src_height - 1) / dst_height;

    for (int y = 0; y < dst_height; ++y) {
        for (int x = 0; x < dst_width; ++x) {
            float sx = x * x_ratio;
            float sy = y * y_ratio;

            int x1 = static_cast<int>(sx);
            int y1 = static_cast<int>(sy);
            int x2 = std::min(x1 + 1, src_width - 1);
            int y2 = std::min(y1 + 1, src_height - 1);

            float fx = sx - x1;
            float fy = sy - y1;

            for (int c = 0; c < 4; ++c) {
                float v1 = src[(y1 * src_width + x1) * 4 + c];
                float v2 = src[(y1 * src_width + x2) * 4 + c];
                float v3 = src[(y2 * src_width + x1) * 4 + c];
                float v4 = src[(y2 * src_width + x2) * 4 + c];

                float val = v1 * (1 - fx) * (1 - fy) +
                           v2 * fx * (1 - fy) +
                           v3 * (1 - fx) * fy +
                           v4 * fx * fy;

                dst[(y * dst_width + x) * 4 + c] = val;
            }
        }
    }

    return dst;
}

} // namespace ump
