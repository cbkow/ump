#pragma once

#include <string>
#include <vector>
#include <memory>

namespace ump {

    struct EXRChannel {
        std::string name;           // e.g., "R", "beauty.R", "diffuse.G"
        std::string pixel_type;     // "half", "float", "uint"
        int x_sampling = 1;
        int y_sampling = 1;
        bool linear = false;
    };

    struct EXRLayer {
        std::string name;           // e.g., "beauty", "diffuse", "specular", "RGBA"
        std::string display_name;   // Human-readable name for UI
        std::vector<EXRChannel> channels;
        bool has_rgba = false;      // True if layer has R, G, B (and optionally A)
        bool has_alpha = false;     // True if layer has alpha channel
        bool is_default = false;    // True if this is the main/default layer

        // For UI sorting
        int priority = 100;         // Lower numbers appear first (beauty=0, diffuse=10, etc.)
    };

    class EXRLayerDetector {
    public:
        EXRLayerDetector();
        ~EXRLayerDetector();

        // Main interface
        bool DetectLayers(const std::string& file_path, std::vector<EXRLayer>& layers);
        bool DetectLayers(const std::string& file_path, std::vector<EXRLayer>& layers, int& cryptomatte_count);
        bool HasMultipleLayers(const std::string& file_path);

        // Utility methods
        static std::string GetLayerDisplayName(const std::string& layer_name);
        static int GetLayerPriority(const std::string& layer_name);
        static bool IsStandardChannel(const std::string& channel_name);
        static bool IsCryptomatteLayer(const std::string& layer_name);

    private:
        // Layer detection helpers
        void GroupChannelsIntoLayers(const std::vector<EXRChannel>& channels, std::vector<EXRLayer>& layers);
        void ProcessStandardLayers(const std::vector<EXRChannel>& channels, std::vector<EXRLayer>& layers);
        void ProcessNamedLayers(const std::vector<EXRChannel>& channels, std::vector<EXRLayer>& layers);
        void CreateFallbackLayer(const std::vector<EXRChannel>& channels, std::vector<EXRLayer>& layers);

        // Channel analysis
        std::string ExtractLayerName(const std::string& channel_name);
        std::string ExtractChannelComponent(const std::string& channel_name);
        bool ChannelBelongsToLayer(const std::string& channel_name, const std::string& layer_name);

        // Layer validation
        void ValidateAndSortLayers(std::vector<EXRLayer>& layers);
        void MarkDefaultLayer(std::vector<EXRLayer>& layers);

        // Error handling
        std::string last_error_;
    };

} // namespace ump