#include "ocio_config_manager.h"
#include <iostream>
#include <filesystem>

OCIOConfigManager::OCIOConfigManager() {
    std::cout << "=== OCIOConfigManager Constructor START ===" << std::endl;
    std::cout.flush();

    ScanForConfigs();

    // TEMP: Test loading built-in ACES 2.0 config directly
    std::cout << "TESTING: Attempting to load built-in ACES 2.0 config..." << std::endl;
    std::cout.flush();
    try {
        auto test_config = OCIO::Config::CreateFromFile("ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5");
        std::cout << "SUCCESS: Built-in ACES 2.0 config loaded!" << std::endl;
        std::cout << "  Config name: " << test_config->getName() << std::endl;
        std::cout << "  Num displays: " << test_config->getNumDisplays() << std::endl;
        std::cout.flush();
    } catch (const OCIO::Exception& e) {
        std::cout << "FAILED: Could not load built-in config: " << e.what() << std::endl;
        std::cout.flush();
    }

    // Load Blender config as default for Standard workflows
    std::cout << "OCIOConfigManager: Loading default Blender config..." << std::endl;
    std::cout.flush();
    if (!LoadConfiguration(OCIOConfigType::BLENDER)) {
        std::cout << "Failed to load Blender config - check assets/OCIO/Blender/config.ocio" << std::endl;
    } else {
        std::cout << "Blender config loaded. Standard workflows available." << std::endl;
    }
    std::cout << "=== OCIOConfigManager Constructor END ===" << std::endl;
    std::cout.flush();
}

void OCIOConfigManager::ScanForConfigs() {
    available_configs.clear();

    // Built-in fake sRGB config removed - now using Blender config for Standard workflows

    // Add built-in ACES 2.0 config (embedded in OCIO 2.5 library)
    available_configs.push_back({
        "ACES 2.0",
        "Built-in ACES 2.0 Studio Config (OCIO 2.5)",
        "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5",  // URI, not file path
        OCIOConfigType::ACES_20
    });

    // Scan assets/OCIO/ directory for config files
    std::string assets_ocio_path = "assets/OCIO";

    if (std::filesystem::exists(assets_ocio_path)) {
        std::cout << "Scanning for OCIO configs in: " << assets_ocio_path << std::endl;

        // Look for common config structures
        // Format: {folder_name, display_name, config_type}
        // Note: ACES_2.0 is NOT scanned here - we use the built-in config instead
        std::vector<std::tuple<std::string, std::string, OCIOConfigType>> config_folders = {
            {"ACES_1.3", "ACES_1.3", OCIOConfigType::ACES_13},
            // {"ACES_2.0", "ACES 2.0", OCIOConfigType::ACES_20},  // Removed - using built-in
            {"Blender", "Blender 4.5", OCIOConfigType::BLENDER},
            {"Blender5", "Blender 5.0", OCIOConfigType::BLENDER5}
        };

        for (const auto& [folder, display_name, type] : config_folders) {
            std::string config_path = assets_ocio_path + "/" + folder + "/config.ocio";
            std::cout << "Checking for config: " << config_path << std::endl;
            if (std::filesystem::exists(config_path)) {
                available_configs.push_back({
                    display_name,
                    "Custom OCIO configuration",
                    config_path,
                    type
                    });
                std::cout << "Found config: " << config_path << std::endl;
            } else {
                std::cout << "Config not found: " << config_path << std::endl;
            }
        }
    }
    else {
        std::cout << "Assets/OCIO directory not found - using built-in configs only" << std::endl;
    }
}

std::string OCIOConfigManager::GetConfigPath(OCIOConfigType type) const {
    for (const auto& config_info : available_configs) {
        if (config_info.type == type) {
            return config_info.path;
        }
    }
    return "";
}

bool OCIOConfigManager::LoadConfiguration(OCIOConfigType type) {
    try {
        std::string config_path = GetConfigPath(type);
        std::cout << "Trying to load config type " << static_cast<int>(type) << " from path: " << config_path << std::endl;

        if (config_path.empty()) {
            std::cout << "Config path empty" << std::endl;
            return false;
        }

        // Check if this is a URI (starts with "ocio://")
        if (config_path.substr(0, 7) == "ocio://") {
            std::cout << "Loading built-in config via URI: " << config_path << std::endl;
            config = OCIO::Config::CreateFromFile(config_path.c_str());
            active_config_type = type;
            BuildAliasMappings();
            std::cout << "Loaded built-in config successfully" << std::endl;
            return true;
        }

        // Otherwise, load from file system
        if (std::filesystem::exists(config_path)) {
            std::cout << "Config file exists, loading..." << std::endl;
            config = OCIO::Config::CreateFromFile(config_path.c_str());
            active_config_type = type;
            BuildAliasMappings();  // Build alias mappings after loading
            std::cout << "Loaded OCIO config: " << config_path << std::endl;
            return true;
        } else {
            std::cout << "Config path empty or file doesn't exist: " << config_path << std::endl;
        }

    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Failed to load OCIO config: " << e.what() << std::endl;
    }

    return false;
}

bool OCIOConfigManager::SwitchToConfig(OCIOConfigType type) {
    std::cout << "SwitchToConfig called: requested type=" << static_cast<int>(type)
              << ", current type=" << static_cast<int>(active_config_type)
              << ", config=" << (config ? "loaded" : "null") << std::endl;

    // Check if we're already using this config
    if (active_config_type == type && config != nullptr) {
        std::cout << "Already using config type " << static_cast<int>(type) << std::endl;
        return true;
    }

    std::cout << "Switching to config type " << static_cast<int>(type) << std::endl;
    return LoadConfiguration(type);
}

bool OCIOConfigManager::LoadCustomConfiguration(const std::string& path) {
    try {
        if (std::filesystem::exists(path)) {
            config = OCIO::Config::CreateFromFile(path.c_str());
            active_config_type = OCIOConfigType::CUSTOM;
            BuildAliasMappings();  // Build alias mappings after loading
            std::cout << "Loaded custom OCIO config: " << path << std::endl;
            return true;
        }
    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Failed to load custom OCIO config: " << e.what() << std::endl;
    }

    return false;
}

std::string OCIOConfigManager::GetActiveConfigName() const {
    for (const auto& config_info : available_configs) {
        if (config_info.type == active_config_type) {
            return config_info.name;
        }
    }
    return "Unknown Config";
}

std::vector<std::string> OCIOConfigManager::GetInputColorSpaces() const {
    // Default behavior: include all colorspaces
    return GetInputColorSpaces(false);
}

std::vector<std::string> OCIOConfigManager::GetInputColorSpaces(bool exclude_display_colorspaces) const {
    std::vector<std::string> colorspaces;

    if (!config) {
        std::cerr << "No OCIO config loaded" << std::endl;
        return colorspaces;
    }

    try {
        for (int i = 0; i < config->getNumColorSpaces(); ++i) {
            const char* name = config->getColorSpaceNameByIndex(i);
            if (!name) continue;

            // If filtering is enabled (for ACES 2.0), check encoding
            if (exclude_display_colorspaces) {
                auto cs = config->getColorSpace(name);
                if (cs) {
                    const char* encoding = cs->getEncoding();
                    if (encoding) {
                        std::string enc(encoding);
                        // Skip display-referred encodings
                        if (enc == "sdr-video" || enc == "hdr-video" ||
                            enc == "edr-video" || enc == "display-linear") {
                            continue;  // Skip this colorspace
                        }
                    }
                }
            }

            colorspaces.push_back(name);
        }
    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Error getting colorspaces: " << e.what() << std::endl;
    }

    return colorspaces;
}

std::vector<std::string> OCIOConfigManager::GetLooks() const {
    if (!config) {
        return {};
    }

    std::vector<std::string> looks;
    try {
        for (int i = 0; i < config->getNumLooks(); ++i) {
            const char* name = config->getLookNameByIndex(i);
            if (name) looks.push_back(name);
        }
    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Error getting looks: " << e.what() << std::endl;
    }

    return looks;
}

std::vector<std::string> OCIOConfigManager::GetDisplays() const {
    if (!config) {
        return {};
    }

    std::vector<std::string> displays;
    try {
        for (int i = 0; i < config->getNumDisplays(); ++i) {
            const char* name = config->getDisplay(i);
            if (name) displays.push_back(name);
        }
    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Error getting displays: " << e.what() << std::endl;
    }

    return displays;
}

std::vector<std::string> OCIOConfigManager::GetViews(const std::string& display) const {
    if (!config) {
        return {};
    }

    std::vector<std::string> views;
    try {
        int num_views = config->getNumViews(display.c_str());
        for (int i = 0; i < num_views; ++i) {
            const char* view_name = config->getView(display.c_str(), i);
            if (view_name) {
                views.push_back(view_name);
            }
        }
    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Error getting views for display '" << display << "': " << e.what() << std::endl;
    }

    return views;
}

void OCIOConfigManager::BuildAliasMappings() {
    // Clear existing mappings
    alias_to_full.clear();
    full_to_alias.clear();
    ui_to_full.clear();
    full_to_ui.clear();

    if (!config) return;

    try {
        // Build mappings for colorspaces
        for (int i = 0; i < config->getNumColorSpaces(); ++i) {
            const char* cs_name = config->getColorSpaceNameByIndex(i);
            if (!cs_name) continue;

            std::string full_name(cs_name);
            std::string ui_name = ApplyUITruncation(full_name);

            // Store UI mapping
            ui_to_full[ui_name] = full_name;
            full_to_ui[full_name] = ui_name;

            // Get colorspace to check for aliases
            auto cs = config->getColorSpace(cs_name);
            int num_aliases = cs->getNumAliases();

            if (num_aliases > 0) {
                // Use first alias as the "best" alias
                const char* best_alias = cs->getAlias(0);
                if (best_alias) {
                    std::string alias(best_alias);
                    full_to_alias[full_name] = alias;
                    alias_to_full[alias] = full_name;

                    // Map all aliases to full name
                    for (int j = 0; j < num_aliases; ++j) {
                        const char* alias_name = cs->getAlias(j);
                        if (alias_name) {
                            alias_to_full[std::string(alias_name)] = full_name;
                        }
                    }
                }
            } else {
                // No aliases, use full name as alias
                full_to_alias[full_name] = full_name;
                alias_to_full[full_name] = full_name;
            }
        }

        // Build mappings for display colorspaces
        for (int i = 0; i < config->getNumDisplays(); ++i) {
            const char* display_name = config->getDisplay(i);
            if (!display_name) continue;

            std::string full_name(display_name);
            std::string ui_name = ApplyUITruncation(full_name);

            // Store UI mapping
            ui_to_full[ui_name] = full_name;
            full_to_ui[full_name] = ui_name;

            // For displays, check if there's a corresponding display colorspace with aliases
            try {
                auto display_cs = config->getColorSpace(display_name);
                if (display_cs) {
                    int num_aliases = display_cs->getNumAliases();
                    if (num_aliases > 0) {
                        const char* best_alias = display_cs->getAlias(0);
                        if (best_alias) {
                            std::string alias(best_alias);
                            full_to_alias[full_name] = alias;
                            alias_to_full[alias] = full_name;

                            // Map all aliases
                            for (int j = 0; j < num_aliases; ++j) {
                                const char* alias_name = display_cs->getAlias(j);
                                if (alias_name) {
                                    alias_to_full[std::string(alias_name)] = full_name;
                                }
                            }
                        }
                    } else {
                        full_to_alias[full_name] = full_name;
                        alias_to_full[full_name] = full_name;
                    }
                }
            } catch (OCIO::Exception&) {
                // Display might not have a colorspace, use name as alias
                full_to_alias[full_name] = full_name;
                alias_to_full[full_name] = full_name;
            }
        }
    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Error building alias mappings: " << e.what() << std::endl;
    }
}

std::string OCIOConfigManager::ApplyUITruncation(const std::string& full_name) const {
    // Apply the same truncation logic that the UI uses
    size_t dash_pos = full_name.find(" - ");
    if (dash_pos != std::string::npos) {
        return full_name.substr(0, dash_pos);
    }
    return full_name;
}

std::string OCIOConfigManager::GetBestAlias(const std::string& full_name) const {
    auto it = full_to_alias.find(full_name);
    return (it != full_to_alias.end()) ? it->second : full_name;
}

std::string OCIOConfigManager::ResolveAlias(const std::string& alias) const {
    auto it = alias_to_full.find(alias);
    return (it != alias_to_full.end()) ? it->second : alias;
}

std::string OCIOConfigManager::GetUIName(const std::string& full_name) const {
    auto it = full_to_ui.find(full_name);
    return (it != full_to_ui.end()) ? it->second : ApplyUITruncation(full_name);
}

std::string OCIOConfigManager::GetFullName(const std::string& ui_name) const {
    auto it = ui_to_full.find(ui_name);
    return (it != ui_to_full.end()) ? it->second : ui_name;
}

std::vector<std::string> OCIOConfigManager::GetViewsForSource(
    const std::string& display,
    const std::string& src_colorspace) const {

    std::vector<std::string> compatible_views;
    if (!config) return compatible_views;

    try {
        // Get all views for this display
        int num_views = config->getNumViews(display.c_str());

        for (int i = 0; i < num_views; ++i) {
            const char* view_name = config->getView(display.c_str(), i);
            if (!view_name) continue;

            // Test if this view is compatible with the source colorspace
            // by attempting to create a DisplayViewTransform
            try {
                OCIO::DisplayViewTransformRcPtr test_transform = OCIO::DisplayViewTransform::Create();
                test_transform->setSrc(src_colorspace.c_str());
                test_transform->setDisplay(display.c_str());
                test_transform->setView(view_name);

                // Try to get a processor - if this fails, the view is incompatible
                auto test_processor = config->getProcessor(test_transform);

                // If we got here, the view is compatible
                compatible_views.push_back(view_name);
            }
            catch (const OCIO::Exception&) {
                // View not compatible with this source encoding - skip it
            }
        }
    }
    catch (const OCIO::Exception& e) {
        std::cerr << "Error getting views for source '" << src_colorspace
                  << "': " << e.what() << std::endl;
    }

    return compatible_views;
}