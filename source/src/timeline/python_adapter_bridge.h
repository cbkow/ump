#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace ump {

/**
 * PythonAdapterBridge - Singleton class for interfacing with Python OTIO adapters
 *
 * This class provides the ability to import AAF and XML timeline files using
 * OTIO's Python adapters, which are more feature-complete than the C++ API.
 *
 * The Python interpreter is embedded and initialized lazily on first use.
 * All calls are thread-safe via internal mutex.
 *
 * Usage:
 *   auto& bridge = PythonAdapterBridge::Instance();
 *   if (bridge.Initialize(python_home_path)) {
 *       std::string error;
 *       std::string json = bridge.ImportTimeline("file.aaf", error);
 *       if (!json.empty()) {
 *           // Parse JSON with OTIO C++ API
 *       }
 *   }
 */
class PythonAdapterBridge {
public:
    // Get singleton instance
    static PythonAdapterBridge& Instance();

    // Initialize Python interpreter with embedded distribution path
    // python_home: Path to the python311 directory containing python.exe
    // Returns true if initialization succeeded
    bool Initialize(const std::string& python_home);

    // Shutdown Python interpreter (called automatically on destruction)
    void Shutdown();

    // Check if Python interpreter is ready
    bool IsInitialized() const { return initialized_; }

    // Import a timeline file using Python OTIO adapters
    // Supports: .aaf, .xml (FCP 7, FCP X, Premiere)
    // Returns: JSON string of the timeline, empty string on error
    // error_message: Populated with error details on failure
    std::string ImportTimeline(const std::string& file_path,
                               std::string& error_message);

    // Get the last error message
    std::string GetLastError() const { return last_error_; }

    // Check if a file extension is supported by Python adapters
    static bool IsSupported(const std::string& file_path);

    // Resolve AAF MobIDs to actual file paths via hex pattern matching
    // For "linked media" AAF exports where file locators are not embedded,
    // this matches SourceMob MobID hex segments against Avid MXF filenames.
    // aaf_path: Path to the AAF file
    // mob_ids: List of MobID strings (urn:smpte:umid:... format from OTIO metadata)
    // search_directory: Directory to search for MXF files (e.g., Avid MediaFiles/MXF)
    // error_message: Populated with error details on failure
    // Returns: Map of mob_id -> file_path (empty string if not found for that mob)
    std::map<std::string, std::string> ResolveMobPaths(
        const std::string& aaf_path,
        const std::vector<std::string>& mob_ids,
        const std::string& search_directory,
        std::string& error_message);

    // Result struct for ResolveAllMobs
    struct AllMobsResult {
        std::map<std::string, std::string> by_mob_id;  // mob_id -> file_path
        std::map<std::string, std::string> by_name;    // clip_name (lowercase) -> file_path
    };

    // Resolve ALL MasterMobs in the AAF to their MXF file paths
    // This is the preferred method - it handles edge cases where:
    // - OTIO adapter stopped at MasterMob instead of following to FileSourceMob
    // - Clips reference external volume paths (original source media)
    // - File locators are not embedded in the AAF
    // Returns both mob_id and name-based mappings for flexible matching
    AllMobsResult ResolveAllMobs(
        const std::string& aaf_path,
        const std::string& search_directory,
        std::string& error_message);

private:
    PythonAdapterBridge() = default;
    ~PythonAdapterBridge();

    // Non-copyable, non-movable
    PythonAdapterBridge(const PythonAdapterBridge&) = delete;
    PythonAdapterBridge& operator=(const PythonAdapterBridge&) = delete;

    bool initialized_ = false;
    std::string last_error_;
    std::string python_home_;
    mutable std::mutex mutex_;

    // Python module references (stored as void* to avoid Python.h in header)
    void* adapters_module_ = nullptr;
    void* sys_module_ = nullptr;
};

} // namespace ump
