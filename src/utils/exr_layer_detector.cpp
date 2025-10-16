#include "exr_layer_detector.h"

// Fix Windows min/max macro conflicts with OpenEXR
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>  // For memory-mapped file APIs
#undef min
#undef max
#endif

// OpenEXR includes - using direct API as per lessons learned
#include <ImfInputFile.h>
#include <ImfChannelList.h>
#include <ImfHeader.h>
#include <ImfPixelType.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace ump {

    EXRLayerDetector::EXRLayerDetector() = default;
    EXRLayerDetector::~EXRLayerDetector() = default;

    bool EXRLayerDetector::DetectLayers(const std::string& file_path, std::vector<EXRLayer>& layers) {
        int cryptomatte_count = 0;
        return DetectLayers(file_path, layers, cryptomatte_count);
    }

    bool EXRLayerDetector::DetectLayers(const std::string& file_path, std::vector<EXRLayer>& layers, int& cryptomatte_count) {
        layers.clear();
        cryptomatte_count = 0;
        last_error_.clear();

        try {
            // Use memory-mapped file for fast header reading (same as DirectEXRCache)
            if (!std::filesystem::exists(file_path)) {
                last_error_ = "EXR file does not exist: " + file_path;
                return false;
            }

            // Memory-mapped stream for fast I/O
            std::unique_ptr<Imf::IStream> stream;

#ifdef _WIN32
            // Windows memory-mapped file
            int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, nullptr, 0);
            std::vector<wchar_t> wpath(wlen);
            MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, wpath.data(), wlen);

            HANDLE fileHandle = CreateFileW(wpath.data(), GENERIC_READ, FILE_SHARE_READ,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (fileHandle == INVALID_HANDLE_VALUE) {
                last_error_ = "Cannot open file: " + file_path;
                return false;
            }
            CloseHandle(fileHandle); // Just checking existence, InputFile will open it
#endif

            Imf::InputFile input_file(file_path.c_str());
            const Imf::Header& header = input_file.header();
            const Imf::ChannelList& channel_list = header.channels();

            // Collect all channels
            std::vector<EXRChannel> channels;

            for (Imf::ChannelList::ConstIterator it = channel_list.begin(); it != channel_list.end(); ++it) {
                try {
                    const std::string& channel_name = it.name();
                    const Imf::Channel& channel = it.channel();

                    EXRChannel exr_channel;
                    exr_channel.name = channel_name;

                    // Get pixel type
                    switch (channel.type) {
                        case Imf::HALF:
                            exr_channel.pixel_type = "half";
                            break;
                        case Imf::FLOAT:
                            exr_channel.pixel_type = "float";
                            break;
                        case Imf::UINT:
                            exr_channel.pixel_type = "uint";
                            break;
                        default:
                            exr_channel.pixel_type = "unknown";
                            break;
                    }

                    exr_channel.x_sampling = channel.xSampling;
                    exr_channel.y_sampling = channel.ySampling;
                    exr_channel.linear = channel.pLinear;

                    channels.push_back(exr_channel);
                } catch (const std::exception& e) {
                    // Silently skip problematic channels
                    continue;
                }
            }

            if (channels.empty()) {
                // No standard channels found - create fallback layer
                CreateFallbackLayer(channels, layers);
                return true;
            }

            // Group channels into logical layers
            GroupChannelsIntoLayers(channels, layers);

            // Count Cryptomattes before filtering them out
            for (const EXRLayer& layer : layers) {
                if (IsCryptomatteLayer(layer.name)) {
                    cryptomatte_count++;
                }
            }

            // Validate and sort layers
            ValidateAndSortLayers(layers);

            return !layers.empty();

        } catch (const std::exception& e) {
            last_error_ = std::string("EXR reading error: ") + e.what();
            // Create fallback single layer on error
            EXRLayer fallback_layer;
            fallback_layer.name = "RGBA";
            fallback_layer.display_name = "Main RGBA";
            fallback_layer.has_rgba = true;
            fallback_layer.has_alpha = true;
            fallback_layer.is_default = true;
            fallback_layer.priority = 0;
            layers.push_back(fallback_layer);
            return true; // Return true with fallback to allow processing
        }
    }

    bool EXRLayerDetector::HasMultipleLayers(const std::string& file_path) {
        std::vector<EXRLayer> layers;
        if (!DetectLayers(file_path, layers)) {
            return false;
        }
        return layers.size() > 1;
    }

    void EXRLayerDetector::GroupChannelsIntoLayers(const std::vector<EXRChannel>& channels, std::vector<EXRLayer>& layers) {
        std::unordered_map<std::string, std::vector<EXRChannel>> layer_map;

        // Group channels by layer name
        for (const EXRChannel& channel : channels) {
            std::string layer_name = ExtractLayerName(channel.name);
            layer_map[layer_name].push_back(channel);
        }

        // Convert groups to EXRLayer objects
        for (const auto& pair : layer_map) {
            EXRLayer layer;
            layer.name = pair.first;
            layer.display_name = GetLayerDisplayName(pair.first);
            layer.channels = pair.second;
            layer.priority = GetLayerPriority(pair.first);

            // Check for RGBA completeness
            bool has_r = false, has_g = false, has_b = false, has_a = false;
            for (const EXRChannel& ch : layer.channels) {
                std::string component = ExtractChannelComponent(ch.name);
                if (component == "R") has_r = true;
                else if (component == "G") has_g = true;
                else if (component == "B") has_b = true;
                else if (component == "A") has_a = true;
            }

            layer.has_rgba = has_r && has_g && has_b;
            layer.has_alpha = has_a;

            layers.push_back(layer);
        }
    }

    std::string EXRLayerDetector::ExtractLayerName(const std::string& channel_name) {
        printf("EXRLayerDetector: Extracting layer from channel '%s'\n", channel_name.c_str());

        // Find the last dot separator
        size_t dot_pos = channel_name.find_last_of('.');
        if (dot_pos == std::string::npos) {
            // No dot found - this is a root channel (R, G, B, A, etc.)
            printf("EXRLayerDetector: No dot found, using RGBA layer\n");
            return "RGBA"; // Default layer name
        }

        std::string before_last_dot = channel_name.substr(0, dot_pos);
        std::string last_component = channel_name.substr(dot_pos + 1);

        // Check if last component is a standard channel (R, G, B, A, r, g, b, a)
        std::string last_upper = last_component;
        std::transform(last_upper.begin(), last_upper.end(), last_upper.begin(), ::toupper);
        bool is_channel_component = (last_upper == "R" || last_upper == "G" || last_upper == "B" || last_upper == "A");

        if (!is_channel_component) {
            // Last component is not RGBA, so this might be a pass name like "ViewLayer.Diffuse"
            // without a channel component - treat the whole thing as layer name
            printf("EXRLayerDetector: Last component '%s' is not RGBA, using full name as layer\n", last_component.c_str());
            return channel_name;
        }

        // Return FULL layer prefix including ViewLayer/RenderLayer
        // DirectEXRCache needs the full name to find channels
        // For "ViewLayer.Combined.R" -> return "ViewLayer.Combined" (NOT just "Combined")
        // For "beauty.R" -> return "beauty"
        printf("EXRLayerDetector: Extracted FULL layer name '%s' from '%s'\n", before_last_dot.c_str(), channel_name.c_str());
        return before_last_dot;
    }

    std::string EXRLayerDetector::ExtractChannelComponent(const std::string& channel_name) {
        // Find the last dot separator
        size_t dot_pos = channel_name.find_last_of('.');
        if (dot_pos == std::string::npos) {
            // No dot found - entire name is the component
            return channel_name;
        }

        return channel_name.substr(dot_pos + 1);
    }

    bool EXRLayerDetector::ChannelBelongsToLayer(const std::string& channel_name, const std::string& layer_name) {
        if (layer_name == "RGBA") {
            // Root RGBA layer accepts channels without dots
            return channel_name.find('.') == std::string::npos;
        }

        // Named layer - channel should start with layer name + dot
        return channel_name.find(layer_name + ".") == 0;
    }

    void EXRLayerDetector::ValidateAndSortLayers(std::vector<EXRLayer>& layers) {
        // Remove empty layers and Cryptomatte layers (non-RGB data)
        layers.erase(std::remove_if(layers.begin(), layers.end(),
            [](const EXRLayer& layer) {
                return layer.channels.empty() || IsCryptomatteLayer(layer.name);
            }), layers.end());

        // Sort by priority (lower numbers first)
        std::sort(layers.begin(), layers.end(),
            [](const EXRLayer& a, const EXRLayer& b) { return a.priority < b.priority; });

        // Mark default layer
        MarkDefaultLayer(layers);
    }

    void EXRLayerDetector::MarkDefaultLayer(std::vector<EXRLayer>& layers) {
        if (layers.empty()) return;

        // Priority order: beauty > RGBA > first layer with RGBA > first layer
        for (EXRLayer& layer : layers) {
            if (layer.name == "beauty" && layer.has_rgba) {
                layer.is_default = true;
                return;
            }
        }

        for (EXRLayer& layer : layers) {
            if (layer.name == "RGBA" && layer.has_rgba) {
                layer.is_default = true;
                return;
            }
        }

        for (EXRLayer& layer : layers) {
            if (layer.has_rgba) {
                layer.is_default = true;
                return;
            }
        }

        // Fallback to first layer
        layers[0].is_default = true;
    }

    void EXRLayerDetector::CreateFallbackLayer(const std::vector<EXRChannel>& channels, std::vector<EXRLayer>& layers) {
        EXRLayer fallback_layer;
        fallback_layer.name = "Unknown";
        fallback_layer.display_name = "Unknown Layer";
        fallback_layer.channels = channels;
        fallback_layer.has_rgba = false;
        fallback_layer.has_alpha = false;
        fallback_layer.is_default = true;
        fallback_layer.priority = 1000; // Low priority

        layers.push_back(fallback_layer);
    }

    std::string EXRLayerDetector::GetLayerDisplayName(const std::string& layer_name) {
        // Standard render pass names with descriptive text
        if (layer_name == "RGBA") return "Main RGBA";
        if (layer_name == "beauty") return "Beauty Pass";
        if (layer_name == "diffuse") return "Diffuse Pass";
        if (layer_name == "specular") return "Specular Pass";
        if (layer_name == "reflection") return "Reflection Pass";
        if (layer_name == "emission") return "Emission Pass";
        if (layer_name == "shadow") return "Shadow Pass";
        if (layer_name == "normal") return "Normal Map";
        if (layer_name == "depth") return "Depth Buffer";
        if (layer_name == "alpha") return "Alpha Matte";

        // Blender/Cycles render pass patterns
        if (layer_name.find("Combined") != std::string::npos) return "Beauty/Combined";
        if (layer_name.find("Emit") != std::string::npos) return "Emission";
        if (layer_name.find("DiffDir") != std::string::npos) return "Diffuse Direct";
        if (layer_name.find("DiffCol") != std::string::npos) return "Diffuse Color";
        if (layer_name.find("GlossDir") != std::string::npos) return "Specular Direct";
        if (layer_name.find("GlossCol") != std::string::npos) return "Specular Color";
        if (layer_name.find("Normal") != std::string::npos) return "Normal Pass";
        if (layer_name.find("Z") != std::string::npos) return "Depth/Z";

        // Maya/Arnold patterns
        if (layer_name.find("direct_diffuse") != std::string::npos) return "Direct Diffuse";
        if (layer_name.find("indirect_diffuse") != std::string::npos) return "Indirect Diffuse";
        if (layer_name.find("direct_specular") != std::string::npos) return "Direct Specular";
        if (layer_name.find("indirect_specular") != std::string::npos) return "Indirect Specular";

        // Cryptomatte patterns (should be filtered out, but if they show up)
        if (layer_name.find("CryptoMaterial") != std::string::npos) return "Cryptomatte Material";
        if (layer_name.find("CryptoObject") != std::string::npos) return "Cryptomatte Object";

        // Generic layer with cleaned up naming
        if (!layer_name.empty()) {
            std::string display = layer_name;

            // Replace common separators with spaces
            std::replace(display.begin(), display.end(), '_', ' ');
            std::replace(display.begin(), display.end(), '.', ' ');

            // Capitalize first letter
            if (!display.empty()) {
                display[0] = std::toupper(display[0]);
            }

            return display;
        }

        return "Unknown Layer";
    }

    int EXRLayerDetector::GetLayerPriority(const std::string& layer_name) {
        // Standard pass priorities (lower = higher priority = appears first)
        if (layer_name == "beauty") return 0;
        if (layer_name == "RGBA") return 1;

        // Blender/Cycles specific priorities
        if (layer_name.find("Combined") != std::string::npos) return 0; // Main beauty pass
        if (layer_name.find("DiffCol") != std::string::npos) return 10;
        if (layer_name.find("DiffDir") != std::string::npos) return 11;
        if (layer_name.find("GlossCol") != std::string::npos) return 20;
        if (layer_name.find("GlossDir") != std::string::npos) return 21;
        if (layer_name.find("Emit") != std::string::npos) return 30;

        // Standard pass priorities
        if (layer_name == "diffuse") return 10;
        if (layer_name == "specular") return 20;
        if (layer_name == "reflection") return 30;
        if (layer_name == "emission") return 40;
        if (layer_name == "shadow") return 50;
        if (layer_name == "normal") return 60;
        if (layer_name == "depth") return 70;
        if (layer_name == "alpha") return 80;

        // Utility/technical passes have lower priority
        if (layer_name.find("Normal") != std::string::npos) return 60;
        if (layer_name.find("Z") != std::string::npos) return 70;
        if (layer_name.find("CryptoMaterial") != std::string::npos) return 90;
        if (layer_name.find("CryptoObject") != std::string::npos) return 91;

        return 100; // Default priority for unknown layers
    }

    bool EXRLayerDetector::IsStandardChannel(const std::string& channel_name) {
        static const std::unordered_set<std::string> standard_channels = {
            "R", "G", "B", "A", "Y", "Z"
        };
        return standard_channels.find(channel_name) != standard_channels.end();
    }

    bool EXRLayerDetector::IsCryptomatteLayer(const std::string& layer_name) {
        // Cryptomatte layers typically contain "crypto" in their name
        // and use lowercase rgba channels instead of uppercase RGBA
        std::string lower_name = layer_name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        return (lower_name.find("crypto") != std::string::npos);
    }

} // namespace ump