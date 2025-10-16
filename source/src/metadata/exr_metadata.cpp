#include "exr_metadata.h"
#include "../utils/exr_layer_detector.h"
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfCompression.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfChromaticitiesAttribute.h>
#include <Imath/ImathBox.h>
#include <algorithm>
#include <cstdio>
#include <sstream>

void EXRMetadata::DetectAndCacheExtendedProperties() {
    if (extended_properties_detected || file_path.empty()) {
        return;
    }

    try {
        // Open EXR file
        Imf::MultiPartInputFile file(file_path.c_str());
        const Imf::Header& header = file.header(0);

        // Get compression type
        const char* compressionNames[] = {
            "None", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24",
            "B44", "B44A", "DWAA", "DWAB"
        };
        int compressionType = static_cast<int>(header.compression());
        if (compressionType >= 0 && compressionType < 10) {
            compression = compressionNames[compressionType];
        }

        // Check if tiled
        is_tiled = header.hasTileDescription();

        // Get multi-part info
        is_multi_part = (file.parts() > 1);
        part_count = file.parts();

        // Get window dimensions
        const Imath::Box2i displayWindow = header.displayWindow();
        const Imath::Box2i dataWindow = header.dataWindow();

        display_width = displayWindow.max.x - displayWindow.min.x + 1;
        display_height = displayWindow.max.y - displayWindow.min.y + 1;
        data_width = dataWindow.max.x - dataWindow.min.x + 1;
        data_height = dataWindow.max.y - dataWindow.min.y + 1;

        // Get channel list and detect layers
        ump::EXRLayerDetector detector;
        std::vector<ump::EXRLayer> detected_layers;

        if (detector.DetectLayers(file_path, detected_layers)) {
            // Convert detected layers to our simplified layer info
            layers.clear();
            total_layers = 0;
            total_channels = 0;

            bool has_half = false;
            bool has_float = false;

            for (const auto& detected_layer : detected_layers) {
                EXRLayerInfo layer_info;
                layer_info.name = detected_layer.name;
                layer_info.display_name = detected_layer.display_name;
                layer_info.channel_count = static_cast<int>(detected_layer.channels.size());
                layer_info.has_alpha = detected_layer.has_alpha;
                layer_info.is_main_layer = detected_layer.is_default;

                // Build channel types string (RGB, RGBA, etc.)
                std::string channel_types_str;
                for (const auto& ch : detected_layer.channels) {
                    // Extract channel component (R, G, B, A, Z, etc.)
                    std::string ch_name = ch.name;
                    size_t dot_pos = ch_name.find_last_of('.');
                    if (dot_pos != std::string::npos) {
                        ch_name = ch_name.substr(dot_pos + 1);
                    }
                    channel_types_str += ch_name;

                    // Track pixel types for bit depth
                    if (ch.pixel_type == "half") has_half = true;
                    if (ch.pixel_type == "float") has_float = true;

                    // Use first channel's pixel type as representative
                    if (layer_info.pixel_type.empty()) {
                        layer_info.pixel_type = ch.pixel_type;
                    }
                }
                layer_info.channel_types = channel_types_str;

                layers.push_back(layer_info);
                total_layers++;
                total_channels += layer_info.channel_count;
            }

            // Build layer summary
            if (total_layers > 0) {
                if (total_layers == 1 && layers[0].is_main_layer) {
                    // Single main layer (RGBA or RGB)
                    layer_summary = layers[0].channel_types + " (" +
                                   std::to_string(total_channels) + " channels, " +
                                   layers[0].pixel_type + ")";
                } else {
                    // Multiple layers
                    std::ostringstream oss;
                    oss << total_layers << " layers (";
                    for (size_t i = 0; i < std::min(size_t(3), layers.size()); i++) {
                        if (i > 0) oss << ", ";
                        oss << layers[i].display_name;
                    }
                    if (layers.size() > 3) {
                        oss << ", +" << (layers.size() - 3) << " more";
                    }
                    oss << ")";
                    layer_summary = oss.str();
                }

                // Determine bit depth
                if (has_float) {
                    bit_depth = 32;
                } else if (has_half) {
                    bit_depth = 16;
                } else {
                    bit_depth = 32; // uint
                }

                // Set pixel format
                pixel_format = has_float ? "float" : (has_half ? "half" : "uint");
            }
        }

        // Try to get color space attributes (may not be present)
        if (header.findTypedAttribute<Imf::StringAttribute>("colorspace")) {
            colorspace = header.typedAttribute<Imf::StringAttribute>("colorspace").value();
        }

        if (header.findTypedAttribute<Imf::ChromaticitiesAttribute>("chromaticities")) {
            const Imf::Chromaticities& chroma = header.typedAttribute<Imf::ChromaticitiesAttribute>("chromaticities").value();
            // Format as string for display
            char buf[256];
            snprintf(buf, sizeof(buf), "R(%.3f,%.3f) G(%.3f,%.3f) B(%.3f,%.3f) W(%.3f,%.3f)",
                chroma.red.x, chroma.red.y,
                chroma.green.x, chroma.green.y,
                chroma.blue.x, chroma.blue.y,
                chroma.white.x, chroma.white.y);
            chromaticities = buf;
        }

        extended_properties_detected = true;

    } catch (const std::exception& e) {
        // Failed to extract extended properties - leave as defaults
        extended_properties_detected = false;
    }
}
