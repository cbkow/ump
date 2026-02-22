#include "ocio_lut_baker.h"
#include "../color/ocio_config_manager.h"
#include "../utils/debug_utils.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

extern std::unique_ptr<OCIOConfigManager> ocio_manager;

namespace qcview {

std::string OCIOLutBaker::GetLutCacheDir() {
#ifdef _WIN32
    const char* localappdata = std::getenv("LOCALAPPDATA");
    if (localappdata) {
        fs::path lut_dir = fs::path(localappdata) / "qcview" / "luts";
        try {
            fs::create_directories(lut_dir);
            return lut_dir.string();
        } catch (const std::exception& e) {
            Debug::Log("ERROR: OCIOLutBaker - Failed to create LUT cache dir: " + std::string(e.what()));
        }
    }
#endif
    // Fallback to temp directory
    fs::path fallback = fs::temp_directory_path() / "qcview_lut_cache";
    try {
        fs::create_directories(fallback);
        return fallback.string();
    } catch (const std::exception& e) {
        Debug::Log("ERROR: OCIOLutBaker - Failed to create fallback LUT dir: " + std::string(e.what()));
        return "";
    }
}

std::string OCIOLutBaker::SanitizeForFilename(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        // Replace special characters with underscore
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == ' ' ||
            c == ',' || c == ';') {
            result += '_';
        } else {
            result += c;
        }
    }
    // Limit length to avoid path issues
    if (result.size() > 64) {
        result = result.substr(0, 64);
    }
    return result;
}

std::string OCIOLutBaker::JoinLutNames(const std::vector<std::string>& luts) {
    if (luts.empty()) {
        return "none";
    }

    std::string result;
    for (size_t i = 0; i < luts.size(); ++i) {
        // Extract just filename from path
        fs::path p(luts[i]);
        std::string name = p.stem().string();  // filename without extension
        if (i > 0) result += "+";
        result += SanitizeForFilename(name);
    }

    // Limit total length
    if (result.size() > 64) {
        result = result.substr(0, 64);
    }
    return result;
}

std::string OCIOLutBaker::GenerateLutFilename(const BakeConfig& config) {
    std::string name;
    name += SanitizeForFilename(config.src_colorspace) + "_";
    name += SanitizeForFilename(config.display) + "_";
    name += SanitizeForFilename(config.view) + "_";
    name += config.looks.empty() ? "none" : SanitizeForFilename(config.looks);
    name += "_";
    name += JoinLutNames(config.scene_luts);
    name += "_";
    name += JoinLutNames(config.display_luts);
    name += ".cube";
    return name;
}

std::string OCIOLutBaker::GetCachedLutPath(const BakeConfig& config) {
    std::string cache_dir = GetLutCacheDir();
    if (cache_dir.empty()) {
        return "";
    }
    return (fs::path(cache_dir) / GenerateLutFilename(config)).string();
}

bool OCIOLutBaker::HasCachedLut(const BakeConfig& config) {
    std::string path = GetCachedLutPath(config);
    if (path.empty()) {
        return false;
    }
    return fs::exists(path);
}

bool OCIOLutBaker::HasTransform(const BakeConfig& config) {
    // No transform if basic parameters are empty
    if (config.src_colorspace.empty() || config.display.empty() || config.view.empty()) {
        return false;
    }
    return true;
}

OCIO::GroupTransformRcPtr OCIOLutBaker::BuildTransform(
    const BakeConfig& config,
    OCIO::ConstConfigRcPtr ocio_config
) {
    try {
        OCIO::GroupTransformRcPtr groupTransform = OCIO::GroupTransform::Create();

        if (!config.looks.empty() || !config.scene_luts.empty() || !config.display_luts.empty()) {
            // Complex transform with looks/LUTs
            if (!config.looks.empty()) {
                OCIO::LookTransformRcPtr lookTransform = OCIO::LookTransform::Create();
                lookTransform->setSrc(config.src_colorspace.c_str());
                lookTransform->setLooks(config.looks.c_str());

                const char* lookResultColorSpace = OCIO::LookTransform::GetLooksResultColorSpace(
                    ocio_config, ocio_config->getCurrentContext(), config.looks.c_str());

                if (!lookResultColorSpace) {
                    Debug::Log("ERROR: OCIOLutBaker - GetLooksResultColorSpace returned NULL");
                    return nullptr;
                }

                lookTransform->setDst(lookResultColorSpace);
                groupTransform->appendTransform(lookTransform);

                // Scene-Referred LUTs (after looks)
                for (const auto& lut_path : config.scene_luts) {
                    try {
                        OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                        fileTransform->setSrc(lut_path.c_str());
                        fileTransform->setInterpolation(OCIO::INTERP_BEST);
                        fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                        groupTransform->appendTransform(fileTransform);
                    } catch (OCIO::Exception& e) {
                        Debug::Log("ERROR: OCIOLutBaker - Failed to load scene LUT: " + std::string(e.what()));
                        return nullptr;
                    }
                }

                // Display transform from look result colorspace
                OCIO::DisplayViewTransformRcPtr displayTransform = OCIO::DisplayViewTransform::Create();
                displayTransform->setSrc(lookResultColorSpace);
                displayTransform->setDisplay(config.display.c_str());
                displayTransform->setView(config.view.c_str());
                displayTransform->setLooksBypass(false);
                groupTransform->appendTransform(displayTransform);

            } else {
                // No looks, but have LUT files
                OCIO::ColorSpaceTransformRcPtr csTransform = OCIO::ColorSpaceTransform::Create();
                csTransform->setSrc(config.src_colorspace.c_str());
                csTransform->setDst("scene_linear");
                groupTransform->appendTransform(csTransform);

                // Scene-Referred LUTs
                for (const auto& lut_path : config.scene_luts) {
                    try {
                        OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                        fileTransform->setSrc(lut_path.c_str());
                        fileTransform->setInterpolation(OCIO::INTERP_BEST);
                        fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                        groupTransform->appendTransform(fileTransform);
                    } catch (OCIO::Exception& e) {
                        Debug::Log("ERROR: OCIOLutBaker - Failed to load scene LUT: " + std::string(e.what()));
                        return nullptr;
                    }
                }

                // Display transform
                OCIO::DisplayViewTransformRcPtr displayTransform = OCIO::DisplayViewTransform::Create();
                displayTransform->setSrc("scene_linear");
                displayTransform->setDisplay(config.display.c_str());
                displayTransform->setView(config.view.c_str());
                groupTransform->appendTransform(displayTransform);
            }

            // Display-Referred LUTs (after display transform)
            for (const auto& lut_path : config.display_luts) {
                try {
                    OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                    fileTransform->setSrc(lut_path.c_str());
                    fileTransform->setInterpolation(OCIO::INTERP_BEST);
                    fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                    groupTransform->appendTransform(fileTransform);
                } catch (OCIO::Exception& e) {
                    Debug::Log("ERROR: OCIOLutBaker - Failed to load display LUT: " + std::string(e.what()));
                    return nullptr;
                }
            }

        } else {
            // Simple display-view transform (no looks/LUTs)
            OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
            transform->setSrc(config.src_colorspace.c_str());
            transform->setDisplay(config.display.c_str());
            transform->setView(config.view.c_str());
            groupTransform->appendTransform(transform);
        }

        return groupTransform;

    } catch (OCIO::Exception& e) {
        Debug::Log("ERROR: OCIOLutBaker::BuildTransform exception: " + std::string(e.what()));
        return nullptr;
    }
}

bool OCIOLutBaker::WriteCubeFile(
    OCIO::ConstCPUProcessorRcPtr processor,
    const std::string& output_path,
    int cube_size,
    std::function<void(float)> progress_callback,
    std::atomic<bool>* cancel_flag
) {
    std::ofstream file(output_path);
    if (!file.is_open()) {
        Debug::Log("ERROR: OCIOLutBaker - Failed to open output file: " + output_path);
        return false;
    }

    // Write .cube header
    file << "# Generated by UnionPlayer OCIOLutBaker\n";
    file << "TITLE \"OCIO Transform LUT\"\n";
    file << "\n";
    file << "LUT_3D_SIZE " << cube_size << "\n";
    file << "\n";
    file << "DOMAIN_MIN 0.0 0.0 0.0\n";
    file << "DOMAIN_MAX 1.0 1.0 1.0\n";
    file << "\n";

    // Set precision for output
    file << std::fixed << std::setprecision(6);

    // Sample the processor at each grid point
    // Order: R varies fastest, then G, then B (standard .cube order)
    const int total_samples = cube_size * cube_size * cube_size;
    int sample_count = 0;

    float rgb[3];
    const float step = 1.0f / (cube_size - 1);

    for (int b = 0; b < cube_size; ++b) {
        for (int g = 0; g < cube_size; ++g) {
            for (int r = 0; r < cube_size; ++r) {
                // Check for cancellation
                if (cancel_flag && cancel_flag->load()) {
                    file.close();
                    // Delete partial file
                    try {
                        fs::remove(output_path);
                    } catch (...) {}
                    Debug::Log("OCIOLutBaker: Baking cancelled");
                    return false;
                }

                // Input RGB values (0.0 to 1.0)
                rgb[0] = r * step;
                rgb[1] = g * step;
                rgb[2] = b * step;

                // Apply OCIO transform
                processor->applyRGB(rgb);

                // Write output (clamp to valid range)
                file << std::max(0.0f, rgb[0]) << " "
                     << std::max(0.0f, rgb[1]) << " "
                     << std::max(0.0f, rgb[2]) << "\n";

                ++sample_count;
            }
        }

        // Report progress after each B slice
        if (progress_callback) {
            progress_callback(static_cast<float>(sample_count) / total_samples);
        }
    }

    file.close();

    if (!file.good()) {
        Debug::Log("ERROR: OCIOLutBaker - Error writing to file: " + output_path);
        try {
            fs::remove(output_path);
        } catch (...) {}
        return false;
    }

    Debug::Log("OCIOLutBaker: Successfully wrote LUT to " + output_path +
               " (" + std::to_string(cube_size) + "^3 = " + std::to_string(total_samples) + " samples)");
    return true;
}

std::string OCIOLutBaker::BakeOrGetCached(
    const BakeConfig& config,
    std::function<void(float)> progress_callback,
    std::atomic<bool>* cancel_flag
) {
    // Check if transform is needed
    if (!HasTransform(config)) {
        Debug::Log("OCIOLutBaker: No transform specified, skipping LUT generation");
        return "";
    }

    // Check for cached LUT
    std::string lut_path = GetCachedLutPath(config);
    if (lut_path.empty()) {
        Debug::Log("ERROR: OCIOLutBaker - Could not determine LUT cache path");
        return "";
    }

    if (fs::exists(lut_path)) {
        Debug::Log("OCIOLutBaker: Using cached LUT: " + lut_path);
        if (progress_callback) {
            progress_callback(1.0f);
        }
        return lut_path;
    }

    // Need to generate LUT
    Debug::Log("OCIOLutBaker: Generating LUT: " + lut_path);

    // Check if OCIO manager has a config
    if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
        Debug::Log("ERROR: OCIOLutBaker - No OCIO config loaded");
        return "";
    }

    try {
        OCIO::ConstConfigRcPtr ocio_config = ocio_manager->GetConfig();
        if (!ocio_config) {
            Debug::Log("ERROR: OCIOLutBaker - Could not get OCIO config from manager");
            return "";
        }

        // Verify colorspace exists
        try {
            ocio_config->getColorSpace(config.src_colorspace.c_str());
        } catch (OCIO::Exception& e) {
            Debug::Log("ERROR: OCIOLutBaker - Colorspace '" + config.src_colorspace +
                       "' not found in config: " + std::string(e.what()));
            return "";
        }

        // Verify display exists
        bool display_found = false;
        int num_displays = ocio_config->getNumDisplays();
        for (int i = 0; i < num_displays; ++i) {
            if (std::string(ocio_config->getDisplay(i)) == config.display) {
                display_found = true;
                break;
            }
        }
        if (!display_found) {
            Debug::Log("ERROR: OCIOLutBaker - Display '" + config.display + "' not found in config");
            return "";
        }

        // Verify view exists
        bool view_found = false;
        try {
            int num_views = ocio_config->getNumViews(config.display.c_str());
            for (int i = 0; i < num_views; ++i) {
                const char* view_name = ocio_config->getView(config.display.c_str(), i);
                if (view_name && std::string(view_name) == config.view) {
                    view_found = true;
                    break;
                }
            }
        } catch (OCIO::Exception& e) {
            Debug::Log("ERROR: OCIOLutBaker - Exception checking views: " + std::string(e.what()));
            return "";
        }
        if (!view_found) {
            Debug::Log("ERROR: OCIOLutBaker - View '" + config.view + "' not found for display '" + config.display + "'");
            return "";
        }

        // Build transform
        OCIO::GroupTransformRcPtr transform = BuildTransform(config, ocio_config);
        if (!transform) {
            Debug::Log("ERROR: OCIOLutBaker - Failed to build transform");
            return "";
        }

        // Get processor
        OCIO::ConstProcessorRcPtr processor = ocio_config->getProcessor(transform);
        if (!processor) {
            Debug::Log("ERROR: OCIOLutBaker - Failed to create OCIO processor");
            return "";
        }

        // Get CPU processor
        OCIO::ConstCPUProcessorRcPtr cpu_processor = processor->getDefaultCPUProcessor();
        if (!cpu_processor) {
            Debug::Log("ERROR: OCIOLutBaker - Failed to create CPU processor");
            return "";
        }

        // Write .cube file
        if (!WriteCubeFile(cpu_processor, lut_path, config.cube_size, progress_callback, cancel_flag)) {
            return "";
        }

        return lut_path;

    } catch (OCIO::Exception& e) {
        Debug::Log("ERROR: OCIOLutBaker - OCIO exception: " + std::string(e.what()));
        return "";
    } catch (const std::exception& e) {
        Debug::Log("ERROR: OCIOLutBaker - Exception: " + std::string(e.what()));
        return "";
    }
}

std::string OCIOLutBaker::BakeTo(
    const BakeConfig& config,
    const std::string& output_path,
    std::function<void(float)> progress_callback,
    std::atomic<bool>* cancel_flag
) {
    // Check if transform is needed
    if (!HasTransform(config)) {
        Debug::Log("ERROR: OCIOLutBaker::BakeTo - No transform specified");
        return "";
    }

    if (output_path.empty()) {
        Debug::Log("ERROR: OCIOLutBaker::BakeTo - No output path specified");
        return "";
    }

    Debug::Log("OCIOLutBaker: Exporting LUT to: " + output_path);

    // Check if OCIO manager has a config
    if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
        Debug::Log("ERROR: OCIOLutBaker::BakeTo - No OCIO config loaded");
        return "";
    }

    try {
        OCIO::ConstConfigRcPtr ocio_config = ocio_manager->GetConfig();
        if (!ocio_config) {
            Debug::Log("ERROR: OCIOLutBaker::BakeTo - Could not get OCIO config from manager");
            return "";
        }

        // Build transform
        OCIO::GroupTransformRcPtr transform = BuildTransform(config, ocio_config);
        if (!transform) {
            Debug::Log("ERROR: OCIOLutBaker::BakeTo - Failed to build transform");
            return "";
        }

        // Get processor
        OCIO::ConstProcessorRcPtr processor = ocio_config->getProcessor(transform);
        if (!processor) {
            Debug::Log("ERROR: OCIOLutBaker::BakeTo - Failed to create OCIO processor");
            return "";
        }

        // Get CPU processor
        OCIO::ConstCPUProcessorRcPtr cpu_processor = processor->getDefaultCPUProcessor();
        if (!cpu_processor) {
            Debug::Log("ERROR: OCIOLutBaker::BakeTo - Failed to create CPU processor");
            return "";
        }

        // Write .cube file
        if (!WriteCubeFile(cpu_processor, output_path, config.cube_size, progress_callback, cancel_flag)) {
            return "";
        }

        return output_path;

    } catch (OCIO::Exception& e) {
        Debug::Log("ERROR: OCIOLutBaker::BakeTo - OCIO exception: " + std::string(e.what()));
        return "";
    } catch (const std::exception& e) {
        Debug::Log("ERROR: OCIOLutBaker::BakeTo - Exception: " + std::string(e.what()));
        return "";
    }
}

} // namespace qcview
