#pragma once
#include <OpenColorIO/OpenColorIO.h>
#include <string>
#include <vector>
#include <map>

namespace OCIO = OCIO_NAMESPACE;

enum class OCIOConfigType {
    ACES_12,
    ACES_13,
    ACES_20,     // ACES 2.0 config with OCIO v2.5 features (shared_views, viewing_rules)
    BLENDER,
    BLENDER5,    // Blender 5 config with OCIO v2.4 features
    CUSTOM
};

struct OCIOConfigInfo {
    std::string name;
    std::string description;
    std::string path;
    OCIOConfigType type;
};

class OCIOConfigManager {
public:
    OCIOConfigManager();
    ~OCIOConfigManager() = default;

    // Configuration management
    bool LoadConfiguration(OCIOConfigType type);
    bool LoadCustomConfiguration(const std::string& path);
    bool SwitchToConfig(OCIOConfigType type);  // Switch to a different config on-demand

    // Component queries for UI
    std::vector<std::string> GetInputColorSpaces() const;
    std::vector<std::string> GetInputColorSpaces(bool exclude_display_colorspaces) const;  // For ACES 2.0
    std::vector<std::string> GetLooks() const;
    std::vector<std::string> GetDisplays() const;
    std::vector<std::string> GetViews(const std::string& display) const;
    std::vector<std::string> GetViewsForSource(const std::string& display, const std::string& src_colorspace) const;  // Encoding-aware for ACES 2.0

    // Config info
    OCIO::ConstConfigRcPtr GetConfig() const { return config; }
    std::string GetActiveConfigName() const;
    bool IsConfigLoaded() const { return config != nullptr; }
    OCIOConfigType GetActiveConfigType() const { return active_config_type; }
    const std::vector<OCIOConfigInfo>& GetAvailableConfigs() const { return available_configs; }

    // Alias management - comprehensive name mapping
    std::string GetBestAlias(const std::string& full_name) const;
    std::string ResolveAlias(const std::string& alias) const;
    std::string GetUIName(const std::string& full_name) const;  // Apply UI truncation
    std::string GetFullName(const std::string& ui_name) const;  // Reverse UI truncation

private:
    OCIO::ConstConfigRcPtr config;
    std::vector<OCIOConfigInfo> available_configs;
    OCIOConfigType active_config_type = OCIOConfigType::BLENDER5;

    // Alias mapping tables (built when config loads)
    std::map<std::string, std::string> alias_to_full;     // alias -> full name
    std::map<std::string, std::string> full_to_alias;     // full name -> best alias
    std::map<std::string, std::string> ui_to_full;        // UI truncated -> full name
    std::map<std::string, std::string> full_to_ui;        // full name -> UI truncated

    void ScanForConfigs();
    std::string GetConfigPath(OCIOConfigType type) const;
    void BuildAliasMappings();  // Build mapping tables when config loads
    std::string ApplyUITruncation(const std::string& full_name) const;
};