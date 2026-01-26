#ifdef WITH_GSTREAMER

#include "gstreamer_manager.h"
#include "../utils/debug_utils.h"

#include <gst/gst.h>
#include <iostream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ump {

//=============================================================================
// Singleton Implementation
//=============================================================================

GStreamerManager& GStreamerManager::Instance() {
    static GStreamerManager instance;
    return instance;
}

GStreamerManager::GStreamerManager() = default;

GStreamerManager::~GStreamerManager() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool GStreamerManager::Initialize() {
    if (initialized_.load()) {
        return true;
    }

    init_error_.clear();

    // Set up plugin path BEFORE gst_init
    SetupPluginPath();

    // Enable full debug output if requested (overrides the nvcodec-focused debug set in SetupPluginPath)
    if (config_.verbose_logging) {
        #ifdef _WIN32
        _putenv_s("GST_DEBUG", "3");
        #else
        setenv("GST_DEBUG", "3", 1);
        #endif
    }

    // Initialize GStreamer
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        if (error) {
            init_error_ = std::string("GStreamer initialization failed: ") + error->message;
            g_error_free(error);
        } else {
            init_error_ = "GStreamer initialization failed (unknown error)";
        }
        Debug::Log("[GStreamerManager] " + init_error_);
        return false;
    }

    // Probe available features
    ProbeFeatures();

    initialized_.store(true);

    Debug::Log("[GStreamerManager] Initialized: " + GetVersionString());
    Debug::Log("[GStreamerManager] Plugin path: " + plugin_path_);
    Debug::Log("[GStreamerManager] Decoders: "
              "libav=" + std::string(has_libav_ ? "yes" : "no") + ", "
              "H264=" + std::string(has_h264_ ? "yes" : "no") + ", "
              "H265=" + std::string(has_h265_ ? "yes" : "no") + ", "
              "ProRes=" + std::string(has_prores_ ? "yes" : "no") + ", "
              "DNxHD=" + std::string(has_dnxhd_ ? "yes" : "no"));

    Debug::Log("[GStreamerManager] Scaling: Software=" + std::string(has_videoconvertscale_ ? "yes" : "no"));

    // Check demuxers (critical for container format support)
    bool has_qtdemux = HasElement("qtdemux");
    bool has_matroska = HasElement("matroskademux");
    Debug::Log("[GStreamerManager] Demuxers: "
              "qtdemux=" + std::string(has_qtdemux ? "yes" : "no") + ", "
              "matroskademux=" + std::string(has_matroska ? "yes" : "no") + ", "
              "avidemux=" + std::string(HasElement("avidemux") ? "yes" : "no") + ", "
              "mpegpsdemux=" + std::string(HasElement("mpegpsdemux") ? "yes" : "no"));

    // If qtdemux is missing, try to diagnose why
    if (!has_qtdemux) {
        Debug::Log("[GStreamerManager] WARNING: qtdemux not found - MP4/MOV files won't work!");

        // Try to find the isomp4 plugin
        GstPlugin* plugin = gst_plugin_load_by_name("isomp4");
        if (plugin) {
            Debug::Log("[GStreamerManager] isomp4 plugin loaded manually: " +
                      std::string(gst_plugin_get_filename(plugin)));
            gst_object_unref(plugin);

            // Check again after manual load
            if (HasElement("qtdemux")) {
                Debug::Log("[GStreamerManager] qtdemux now available after manual plugin load");
            }
        } else {
            Debug::Log("[GStreamerManager] Failed to load isomp4 plugin - check dependencies");

            // Try direct Windows DLL load to get specific error
            #ifdef _WIN32
            std::filesystem::path plugin_file = std::filesystem::path(plugin_path_) / "gstisomp4.dll";
            if (std::filesystem::exists(plugin_file)) {
                HMODULE hModule = LoadLibraryA(plugin_file.string().c_str());
                if (!hModule) {
                    DWORD error = GetLastError();
                    char buf[256];
                    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error,
                                  0, buf, sizeof(buf), nullptr);
                    Debug::Log("[GStreamerManager] LoadLibrary failed: " + std::string(buf));
                } else {
                    Debug::Log("[GStreamerManager] LoadLibrary succeeded but GStreamer didn't register it");
                    FreeLibrary(hModule);
                }
            } else {
                Debug::Log("[GStreamerManager] gstisomp4.dll not found at: " + plugin_file.string());
            }
            #endif

            // List all registered plugins for diagnostics
            GstRegistry* registry = gst_registry_get();
            GList* plugins = gst_registry_get_plugin_list(registry);
            std::string plugin_list = "[GStreamerManager] Loaded plugins: ";
            int count = 0;
            for (GList* p = plugins; p && count < 30; p = p->next, count++) {
                GstPlugin* pl = GST_PLUGIN(p->data);
                if (count > 0) plugin_list += ", ";
                plugin_list += gst_plugin_get_name(pl);
            }
            gst_plugin_list_free(plugins);
            Debug::Log(plugin_list);
        }
    }

    // List all available decoders for diagnostics
    auto decoders = GetAvailableDecoders();
    if (!decoders.empty()) {
        std::string decoder_list = "[GStreamerManager] Available decoders: ";
        for (size_t i = 0; i < decoders.size(); i++) {
            if (i > 0) decoder_list += ", ";
            decoder_list += decoders[i];
        }
        Debug::Log(decoder_list);
    } else {
        Debug::Log("[GStreamerManager] WARNING: No video decoders found!");
    }

    return true;
}

void GStreamerManager::Shutdown() {
    if (!initialized_.load()) {
        return;
    }

    // Note: gst_deinit() should only be called once and makes GStreamer
    // unusable for the rest of the application lifetime.
    // For most applications, it's better to just let the OS clean up.
    // gst_deinit();

    initialized_.store(false);
    Debug::Log("[GStreamerManager] Shutdown complete");
}

//=============================================================================
// Plugin Path Setup
//=============================================================================

void GStreamerManager::SetupPluginPath() {
    #ifdef _WIN32
    // Get executable directory
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();

    // Set GST_PLUGIN_PATH to bundled plugins
    std::filesystem::path plugin_dir = exe_dir / "gst-plugins";
    plugin_path_ = plugin_dir.string();

    // Check if plugin directory exists
    if (std::filesystem::exists(plugin_dir)) {
        // Combine with any existing GST_PLUGIN_PATH
        const char* existing = std::getenv("GST_PLUGIN_PATH");
        std::string full_path = plugin_path_;
        if (existing && strlen(existing) > 0) {
            full_path = plugin_path_ + ";" + existing;
        }

        _putenv_s("GST_PLUGIN_PATH", full_path.c_str());

        // Disable system plugin scanning (we only want bundled plugins)
        _putenv_s("GST_PLUGIN_SYSTEM_PATH", "");

        // Set registry location for faster subsequent startups
        std::filesystem::path registry_path = exe_dir / "gst-registry.bin";
        _putenv_s("GST_REGISTRY", registry_path.string().c_str());
    } else {
        // Plugin directory doesn't exist - check for system GStreamer
        Debug::Log("[GStreamerManager] Warning: Plugin directory not found: " + plugin_path_);
        Debug::Log("[GStreamerManager] Falling back to system GStreamer installation");
        plugin_path_ = "(system)";
    }
    #else
    // Linux/macOS: Use system GStreamer by default
    plugin_path_ = "(system)";
    #endif
}

//=============================================================================
// Feature Detection
//=============================================================================

void GStreamerManager::ProbeFeatures() {
    // Check for software scaling element
    // Note: D3D11/D3D12 GPU scaling have compatibility issues, using software only
    has_videoconvertscale_ = HasElement("videoconvertscale");

    // Check for libav (FFmpeg) support
    has_libav_ = HasElement("avdec_h264") || HasElement("avdec_prores");

    // Check for H.264 decoder (any variant)
    has_h264_ = HasElement("d3d11h264dec") ||
                HasElement("avdec_h264") ||
                HasElement("mfh264dec") ||      // Windows Media Foundation (HW accel)
                HasElement("openh264dec") ||
                HasElement("vaapih264dec");

    // Check for H.265/HEVC decoder
    has_h265_ = HasElement("d3d11h265dec") ||
                HasElement("avdec_hevc") ||
                HasElement("mfh265dec") ||      // Windows Media Foundation
                HasElement("vaapih265dec");

    // Check for ProRes decoder (via libav)
    has_prores_ = HasElement("avdec_prores");

    // Check for DNxHD decoder (via libav)
    has_dnxhd_ = HasElement("avdec_dnxhd");
}

bool GStreamerManager::HasElement(const std::string& element_name) const {
    GstElementFactory* factory = gst_element_factory_find(element_name.c_str());
    if (factory) {
        gst_object_unref(factory);
        return true;
    }
    return false;
}

std::vector<std::string> GStreamerManager::GetAvailableDecoders() const {
    std::vector<std::string> decoders;

    // List of decoders we care about
    const char* decoder_names[] = {
        // D3D11 hardware decoders
        "d3d11h264dec", "d3d11h265dec", "d3d11vp9dec", "d3d11av1dec",
        // VAAPI hardware decoders (Linux)
        "vaapih264dec", "vaapih265dec", "vaapivp9dec",
        // NVDEC hardware decoders
        "nvh264dec", "nvh265dec",
        // Windows Media Foundation decoders
        "mfh264dec", "mfh265dec", "mfvp9dec",
        // libav (FFmpeg) software decoders
        "avdec_h264", "avdec_hevc", "avdec_prores", "avdec_dnxhd",
        "avdec_vp9", "avdec_av1", "avdec_mjpeg",
        // Other decoders
        "openh264dec",
        nullptr
    };

    for (int i = 0; decoder_names[i] != nullptr; i++) {
        if (HasElement(decoder_names[i])) {
            decoders.push_back(decoder_names[i]);
        }
    }

    return decoders;
}

//=============================================================================
// Diagnostics
//=============================================================================

std::string GStreamerManager::GetVersionString() const {
    guint major, minor, micro, nano;
    gst_version(&major, &minor, &micro, &nano);

    char version[64];
    snprintf(version, sizeof(version), "GStreamer %u.%u.%u", major, minor, micro);
    if (nano > 0) {
        char nano_str[16];
        snprintf(nano_str, sizeof(nano_str), ".%u", nano);
        strcat(version, nano_str);
    }

    return std::string(version);
}

} // namespace ump

#endif // WITH_GSTREAMER
