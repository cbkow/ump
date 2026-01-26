#pragma once

#ifdef WITH_GSTREAMER

#include <string>
#include <atomic>
#include <vector>

namespace ump {

//=============================================================================
// GStreamer Backend Configuration
//=============================================================================

struct GStreamerConfig {
    bool enable_d3d11 = true;           // Use D3D11 hardware acceleration
    bool enable_zero_copy = true;       // Zero-copy texture sharing (D3D11)
    int decode_threads = 4;             // Decode threads hint
    bool prefer_hardware_decoders = true;
    bool verbose_logging = false;       // Enable GStreamer debug output
};

//=============================================================================
// GStreamer Backend Manager (Singleton)
//
// Handles global GStreamer initialization, plugin discovery, and cleanup.
// Must be initialized before any GStreamer decoder is created.
//=============================================================================

class GStreamerManager {
public:
    // Singleton access
    static GStreamerManager& Instance();

    // Delete copy/move
    GStreamerManager(const GStreamerManager&) = delete;
    GStreamerManager& operator=(const GStreamerManager&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize GStreamer runtime (call once at startup)
    // Sets GST_PLUGIN_PATH to bundled plugins
    // Returns true on success
    bool Initialize();

    // Shutdown GStreamer (call at application exit)
    void Shutdown();

    // Check if GStreamer is available and initialized
    bool IsInitialized() const { return initialized_.load(); }

    //=========================================================================
    // Feature Detection
    //=========================================================================

    // Check if specific features are available
    bool HasProResSupport() const { return has_prores_; }
    bool HasDNxHDSupport() const { return has_dnxhd_; }
    bool HasH264Support() const { return has_h264_; }
    bool HasH265Support() const { return has_h265_; }
    bool HasLibavSupport() const { return has_libav_; }

    // Scaling support (software only - GPU scaling has compatibility issues)
    bool HasVideoConvertScale() const { return has_videoconvertscale_; }

    // Check if a specific GStreamer element/plugin is available
    bool HasElement(const std::string& element_name) const;

    // Get list of available decoder elements
    std::vector<std::string> GetAvailableDecoders() const;

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetConfig(const GStreamerConfig& config) { config_ = config; }
    const GStreamerConfig& GetConfig() const { return config_; }

    //=========================================================================
    // Diagnostics
    //=========================================================================

    // Get version string for logging
    std::string GetVersionString() const;

    // Get plugin search path
    std::string GetPluginPath() const { return plugin_path_; }

    // Get initialization error (if Initialize() failed)
    const std::string& GetInitError() const { return init_error_; }

private:
    GStreamerManager();
    ~GStreamerManager();

    // Setup plugin path before gst_init
    void SetupPluginPath();

    // Probe available features after initialization
    void ProbeFeatures();

    // State
    std::atomic<bool> initialized_{false};
    GStreamerConfig config_;
    std::string plugin_path_;
    std::string init_error_;

    // Feature detection results
    bool has_prores_ = false;
    bool has_dnxhd_ = false;
    bool has_h264_ = false;
    bool has_h265_ = false;
    bool has_libav_ = false;

    // Scaling support
    bool has_videoconvertscale_ = false;  // Software scaling element
};

} // namespace ump

#endif // WITH_GSTREAMER
