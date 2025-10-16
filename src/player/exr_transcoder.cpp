#include "exr_transcoder.h"
#include "../utils/debug_utils.h"
#include "image_loaders.h"  // For TIFFLoader and PNGLoader

#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfChannelList.h>

#include "direct_exr_cache.h"  // For MemoryMappedIStream

#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <regex>

// stb_image_resize2 for high-quality float resizing (single-header, public domain)
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../external/stb/stb_image_resize2.h"

#ifdef _WIN32
    #include <windows.h>
#endif

namespace ump {

//=============================================================================
// Constructor / Destructor
//=============================================================================

EXRTranscoder::EXRTranscoder() {
    Initialize();
}

EXRTranscoder::~EXRTranscoder() {
    CancelTranscode();

    // Wait for transcode thread to finish
    if (transcode_thread_.joinable()) {
        transcode_thread_.join();
    }

    // Cleanup cache on shutdown if requested (blocking operation)
    if (clear_cache_on_exit_) {
        try {
            std::filesystem::remove_all(cache_dir_);
            Debug::Log("EXRTranscoder: Cleaned up all transcodes on exit");
        } catch (const std::exception& e) {
            Debug::Log("EXRTranscoder: Failed to cleanup on exit - " + std::string(e.what()));
        }
    }
}

//=============================================================================
// Initialization
//=============================================================================

void EXRTranscoder::Initialize() {
    if (initialized_) return;

    SetupDefaultCacheDirectory();

    // Create cache directory if it doesn't exist
    std::filesystem::create_directories(cache_dir_);

    // Lazy cleanup: Remove old transcode directories (older than retention days)
    try {
        auto now = std::filesystem::file_time_type::clock::now();
        auto retention_cutoff = now - std::chrono::hours(24 * cache_retention_days_);

        size_t removed_dirs = 0;
        size_t total_size = 0;

        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
            if (entry.is_directory()) {
                // Check directory age (use newest file in directory)
                auto dir_time = std::filesystem::last_write_time(entry.path());

                // Calculate directory size
                size_t dir_size = 0;
                for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path())) {
                    if (file.is_regular_file()) {
                        dir_size += file.file_size();
                        auto file_time = std::filesystem::last_write_time(file.path());
                        if (file_time > dir_time) dir_time = file_time;
                    }
                }

                total_size += dir_size;

                // Remove if older than retention days
                if (dir_time < retention_cutoff) {
                    std::filesystem::remove_all(entry.path());
                    removed_dirs++;
                }
            }
        }

        if (removed_dirs > 0) {
            Debug::Log("EXRTranscoder: Cleaned up " + std::to_string(removed_dirs) +
                      " old transcode directories (older than " + std::to_string(cache_retention_days_) + " days)");
        }

        // Additional cleanup if cache exceeds size limit
        size_t max_bytes = static_cast<size_t>(cache_max_gb_) * 1024ULL * 1024ULL * 1024ULL;
        if (total_size > max_bytes) {
            Debug::Log("EXRTranscoder: Cache size exceeds " + std::to_string(cache_max_gb_) + " GB (" +
                      std::to_string(total_size / (1024 * 1024 * 1024)) + " GB), removing oldest directories");

            // Collect all directories with their ages
            std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> dirs;
            for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
                if (entry.is_directory()) {
                    auto dir_time = std::filesystem::last_write_time(entry.path());
                    for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path())) {
                        if (file.is_regular_file()) {
                            auto file_time = std::filesystem::last_write_time(file.path());
                            if (file_time > dir_time) dir_time = file_time;
                        }
                    }
                    dirs.push_back({entry.path(), dir_time});
                }
            }

            // Sort by age (oldest first)
            std::sort(dirs.begin(), dirs.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });

            // Remove oldest 50%
            size_t remove_count = dirs.size() / 2;
            for (size_t i = 0; i < remove_count && i < dirs.size(); i++) {
                std::filesystem::remove_all(dirs[i].first);
            }

            Debug::Log("EXRTranscoder: Removed " + std::to_string(remove_count) + " oldest directories to free space");
        }
    } catch (const std::exception& e) {
        Debug::Log("EXRTranscoder: Cleanup warning - " + std::string(e.what()));
    }

    initialized_ = true;
    Debug::Log("EXRTranscoder: Initialized with cache dir: " + cache_dir_);
}

void EXRTranscoder::SetupDefaultCacheDirectory() {
    // Use custom cache path if set, otherwise use %localappdata%
    if (!custom_cache_path_.empty()) {
        std::filesystem::path custom_path = std::filesystem::path(custom_cache_path_) / "EXRtranscodes";
        cache_dir_ = custom_path.string();

        Debug::Log("EXRTranscoder: Using custom cache path for transcode cache: " + cache_dir_);
        return;
    }

    // Get %localappdata% environment variable (Windows)
    const char* localappdata = std::getenv("LOCALAPPDATA");

    if (localappdata) {
        std::filesystem::path app_data_path = std::filesystem::path(localappdata) / "ump" / "EXRtranscodes";
        cache_dir_ = app_data_path.string();

        Debug::Log("EXRTranscoder: Using %LOCALAPPDATA% for transcode cache: " + cache_dir_);
    } else {
        // Fallback to relative directory if %localappdata% not available
        cache_dir_ = "temp/exr_transcodes/";
        Debug::Log("EXRTranscoder: Warning - %LOCALAPPDATA% not found, using fallback: " + cache_dir_);
    }
}

void EXRTranscoder::SetCacheDirectory(const std::string& path) {
    cache_dir_ = path;
    std::filesystem::create_directories(cache_dir_);
    Debug::Log("EXRTranscoder: Cache directory set to: " + cache_dir_);
}

void EXRTranscoder::SetCacheConfig(const std::string& custom_path, int retention_days, int max_gb, bool clear_on_exit) {
    custom_cache_path_ = custom_path;
    cache_retention_days_ = retention_days;
    cache_max_gb_ = max_gb;
    clear_cache_on_exit_ = clear_on_exit;

    // Reinitialize if path changed
    if (initialized_) {
        initialized_ = false;
        Initialize();
    }
}

size_t EXRTranscoder::ClearAllTranscodes() {
    Debug::Log("=== Clearing ALL EXR transcodes from all cache locations ===");

    size_t total_bytes = 0;

    // Helper lambda to clear a directory and return bytes deleted
    auto clear_directory = [&total_bytes](const std::string& dir_path) -> size_t {
        if (!std::filesystem::exists(dir_path)) {
            Debug::Log("Directory does not exist: " + dir_path);
            return 0;
        }

        try {
            size_t dir_bytes = 0;
            int file_count = 0;

            // Calculate total size before deleting
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
                if (entry.is_regular_file()) {
                    dir_bytes += entry.file_size();
                    file_count++;
                }
            }

            std::filesystem::remove_all(dir_path);
            std::filesystem::create_directories(dir_path); // Recreate empty directory

            total_bytes += dir_bytes;
            Debug::Log("Cleared directory: " + dir_path + " (" + std::to_string(file_count) + " files, " +
                      std::to_string(dir_bytes / 1024.0 / 1024.0) + " MB)");

            return dir_bytes;
        } catch (const std::exception& e) {
            Debug::Log("Failed to clear directory " + dir_path + ": " + std::string(e.what()));
            return 0;
        }
    };

    // 1. Clear default location (%LOCALAPPDATA%\ump\EXRtranscodes\)
    const char* localappdata = std::getenv("LOCALAPPDATA");
    if (localappdata) {
        std::filesystem::path default_path = std::filesystem::path(localappdata) / "ump" / "EXRtranscodes";
        clear_directory(default_path.string());
    }

    // 2. Clear custom location (if set)
    if (!custom_cache_path_.empty()) {
        std::filesystem::path custom_path = std::filesystem::path(custom_cache_path_) / "EXRtranscodes";
        clear_directory(custom_path.string());
    }

    // 3. Clear fallback location (temp/exr_transcodes/)
    clear_directory("temp/exr_transcodes");

    Debug::Log("=== Cleared " + std::to_string(total_bytes / 1024.0 / 1024.0) + " MB total from EXR transcodes ===");
    return total_bytes;
}

//=============================================================================
// Cache Key Generation
//=============================================================================

std::string EXRTranscoder::GenerateCacheKey(const std::string& base_name,
                                            const std::string& layer,
                                            int max_width,
                                            Imf::Compression compression) const {
    std::ostringstream oss;
    oss << base_name << "_" << layer << "_";

    if (max_width > 0) {
        oss << max_width;
    } else {
        oss << "native";
    }

    oss << "_" << CompressionToString(compression);

    return oss.str();
}

const char* EXRTranscoder::CompressionToString(Imf::Compression comp) const {
    switch (comp) {
        case Imf::NO_COMPRESSION: return "NONE";
        case Imf::RLE_COMPRESSION: return "RLE";
        case Imf::ZIPS_COMPRESSION: return "ZIPS";
        case Imf::ZIP_COMPRESSION: return "ZIP";
        case Imf::PIZ_COMPRESSION: return "PIZ";
        case Imf::PXR24_COMPRESSION: return "PXR24";
        case Imf::B44_COMPRESSION: return "B44";
        case Imf::B44A_COMPRESSION: return "B44A";
        case Imf::DWAA_COMPRESSION: return "DWAA";
        case Imf::DWAB_COMPRESSION: return "DWAB";
        default: return "UNKNOWN";
    }
}

//=============================================================================
// Transcode Path Management
//=============================================================================

std::string EXRTranscoder::GetTranscodePath(const std::string& source_first_file,
                                            const std::string& layer,
                                            int max_width,
                                            Imf::Compression compression) const {
    std::filesystem::path source_path(source_first_file);
    std::string base_name = source_path.stem().string();

    // Remove frame number from base name (e.g., "sequence_0001" -> "sequence")
    std::regex pattern(R"(^(.+)[_\.\-](\d+)$)");
    std::smatch match;
    if (std::regex_match(base_name, match, pattern)) {
        base_name = match[1].str();
    }

    std::string cache_key = GenerateCacheKey(base_name, layer, max_width, compression);
    std::filesystem::path transcode_dir = std::filesystem::path(cache_dir_) / cache_key;

    return transcode_dir.string();
}

bool EXRTranscoder::HasTranscodedSequence(const std::vector<std::string>& source_files,
                                          const std::string& layer,
                                          int max_width,
                                          Imf::Compression compression) const {
    if (source_files.empty()) return false;

    std::string transcode_dir = GetTranscodePath(source_files[0], layer, max_width, compression);

    if (!std::filesystem::exists(transcode_dir)) return false;

    // Count transcoded files
    int transcoded_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(transcode_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".exr") {
            transcoded_count++;
        }
    }

    // Check if we have all frames transcoded
    return transcoded_count >= source_files.size();
}

std::vector<std::string> EXRTranscoder::GetTranscodedFiles(const std::vector<std::string>& source_files,
                                                           const std::string& layer,
                                                           int max_width,
                                                           Imf::Compression compression) const {
    std::vector<std::string> transcoded_files;

    if (source_files.empty()) return transcoded_files;

    std::string transcode_dir = GetTranscodePath(source_files[0], layer, max_width, compression);

    if (!std::filesystem::exists(transcode_dir)) return transcoded_files;

    // Collect all .exr files in transcode directory
    for (const auto& entry : std::filesystem::directory_iterator(transcode_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".exr") {
            transcoded_files.push_back(entry.path().string());
        }
    }

    // Sort by filename
    std::sort(transcoded_files.begin(), transcoded_files.end());

    return transcoded_files;
}

//=============================================================================
// Async Transcode
//=============================================================================

void EXRTranscoder::TranscodeSequenceAsync(
    const std::vector<std::string>& source_files,
    const std::string& layer,
    const EXRTranscodeConfig& config,
    std::function<void(int, int, const std::string&)> progress_callback,
    std::function<void(bool, const std::string&)> completion_callback) {

    if (is_transcoding_.load()) {
        Debug::Log("EXRTranscoder: ERROR - Transcode already in progress");
        if (completion_callback) {
            completion_callback(false, "Transcode already in progress");
        }
        return;
    }

    if (source_files.empty()) {
        Debug::Log("EXRTranscoder: ERROR - No source files provided");
        if (completion_callback) {
            completion_callback(false, "No source files provided");
        }
        return;
    }

    // Wait for previous thread to finish
    if (transcode_thread_.joinable()) {
        transcode_thread_.join();
    }

    // Start transcode worker thread
    cancel_requested_ = false;
    is_transcoding_ = true;

    transcode_thread_ = std::thread(&EXRTranscoder::TranscodeWorker, this,
                                    source_files, layer, config,
                                    progress_callback, completion_callback);
}

void EXRTranscoder::CancelTranscode() {
    if (is_transcoding_.load()) {
        Debug::Log("EXRTranscoder: Cancel requested");
        cancel_requested_ = true;
    }
}

//=============================================================================
// Transcode Worker Thread
//=============================================================================

void EXRTranscoder::TranscodeWorker(
    std::vector<std::string> source_files,
    std::string layer,
    EXRTranscodeConfig config,
    std::function<void(int, int, const std::string&)> progress_callback,
    std::function<void(bool, const std::string&)> completion_callback) {

    Debug::Log("EXRTranscoder: Starting transcode worker - " + std::to_string(source_files.size()) + " frames");

    std::string transcode_dir = GetTranscodePath(source_files[0], layer, config.max_width, config.compression);

    // Create transcode directory
    try {
        std::filesystem::create_directories(transcode_dir);
    } catch (const std::exception& e) {
        std::string error = "Failed to create transcode directory: " + std::string(e.what());
        Debug::Log("EXRTranscoder: ERROR - " + error);
        is_transcoding_ = false;
        if (completion_callback) completion_callback(false, error);
        return;
    }

    // Detect source format from first file
    std::string ext = source_files[0].substr(source_files[0].find_last_of('.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    bool is_exr = (ext == ".exr");
    bool is_tiff = (ext == ".tif" || ext == ".tiff");
    bool is_png = (ext == ".png");

    if (!is_exr && !is_tiff && !is_png) {
        std::string error = "Unsupported format: " + ext;
        Debug::Log("EXRTranscoder: ERROR - " + error);
        is_transcoding_ = false;
        if (completion_callback) completion_callback(false, error);
        return;
    }

    std::string format_name = is_exr ? "EXR" : (is_tiff ? "TIFF" : "PNG");
    Debug::Log("EXRTranscoder: Detected format: " + format_name);

    // Determine target resolution from first frame
    int target_width = 0;
    int target_height = 0;
    int source_width = 0;
    int source_height = 0;

    try {
        // Get dimensions based on format
        if (is_exr) {
            // Read EXR dimensions
            auto stream = std::make_unique<MemoryMappedIStream>(source_files[0]);
            Imf::MultiPartInputFile file(*stream);
            const Imf::Header& header = file.header(0);
            const Imath::Box2i displayWindow = header.displayWindow();
            source_width = displayWindow.max.x - displayWindow.min.x + 1;
            source_height = displayWindow.max.y - displayWindow.min.y + 1;
        } else if (is_tiff) {
            // Get TIFF dimensions
            ImageInfo info;
            if (!TIFFLoader::GetInfo(source_files[0], info)) {
                throw std::runtime_error("Failed to get TIFF dimensions");
            }
            source_width = info.width;
            source_height = info.height;
        } else if (is_png) {
            // Get PNG dimensions
            ImageInfo info;
            if (!PNGLoader::GetInfo(source_files[0], info)) {
                throw std::runtime_error("Failed to get PNG dimensions");
            }
            source_width = info.width;
            source_height = info.height;
        }

        if (config.max_width > 0 && source_width > config.max_width) {
            // Calculate target dimensions preserving aspect ratio
            target_width = config.max_width;
            target_height = static_cast<int>((static_cast<double>(source_height) / source_width) * target_width);

            Debug::Log("EXRTranscoder: Resizing from " + std::to_string(source_width) + "x" + std::to_string(source_height) +
                      " to " + std::to_string(target_width) + "x" + std::to_string(target_height));
        } else {
            // Native resolution
            target_width = source_width;
            target_height = source_height;
            Debug::Log("EXRTranscoder: Using native resolution " + std::to_string(target_width) + "x" + std::to_string(target_height));
        }
    } catch (const std::exception& e) {
        std::string error = "Failed to read source dimensions: " + std::string(e.what());
        Debug::Log("EXRTranscoder: ERROR - " + error);
        is_transcoding_ = false;
        if (completion_callback) completion_callback(false, error);
        return;
    }

    // Parallel transcoding using async (similar to DirectEXRCache pattern)
    Debug::Log("EXRTranscoder: Using " + std::to_string(config.threadCount) + " parallel threads");

    completed_count_ = 0;
    failed_count_ = 0;

    // Task queue and active futures
    size_t next_frame_index = 0;
    std::vector<std::future<bool>> active_tasks;
    std::vector<size_t> active_indices;

    auto launch_task = [&](size_t frame_idx) -> bool {
        if (frame_idx >= source_files.size()) return false;

        // Create copies of strings BEFORE async lambda (critical for parallel execution)
        std::string source_file = source_files[frame_idx];  // COPY, not reference
        std::filesystem::path source_path(source_file);

        // For TIFF/PNG, change extension to .exr
        std::string output_filename;
        if (is_exr) {
            output_filename = source_path.filename().string();
        } else {
            // Replace extension with .exr
            output_filename = source_path.stem().string() + ".exr";
        }

        std::string dest_file = (std::filesystem::path(transcode_dir) / output_filename).string();

        // Launch async task - capture by VALUE to ensure each thread gets unique work
        auto future = std::async(std::launch::async, [this, source_file, dest_file, layer,
                                                       target_width, target_height,
                                                       compression = config.compression, is_exr]() -> bool {
            std::string error_message;
            bool success = false;

            // Call appropriate transcode method based on format
            if (is_exr) {
                success = TranscodeFrame(source_file, dest_file, layer,
                                        target_width, target_height,
                                        compression, error_message);
            } else {
                // TIFF/PNG → EXR transcode (layer parameter ignored for non-EXR)
                success = TranscodeImageToEXR(source_file, dest_file,
                                             target_width, target_height,
                                             compression, error_message);
            }

            if (success) {
                completed_count_.fetch_add(1);
            } else {
                failed_count_.fetch_add(1);
                Debug::Log("EXRTranscoder: Failed to transcode " + source_file + " - " + error_message);
            }

            return success;
        });

        active_tasks.push_back(std::move(future));
        active_indices.push_back(frame_idx);
        return true;
    };

    // Launch initial batch (up to threadCount tasks)
    for (size_t i = 0; i < config.threadCount && next_frame_index < source_files.size(); i++) {
        launch_task(next_frame_index++);
    }

    // Main loop: wait for task completion and launch new ones
    while (!active_tasks.empty()) {
        // Check for cancellation
        if (cancel_requested_.load()) {
            Debug::Log("EXRTranscoder: Cancelled by user - waiting for " +
                      std::to_string(active_tasks.size()) + " active tasks to finish");
            // Wait for all active tasks to complete before exiting
            for (auto& task : active_tasks) {
                task.wait();
            }
            is_transcoding_ = false;
            if (completion_callback) completion_callback(false, "Cancelled by user");
            return;
        }

        // Check for too many failures
        if (failed_count_.load() > 10) {
            std::string error = "Too many failures (" + std::to_string(failed_count_.load()) + "), aborting transcode";
            Debug::Log("EXRTranscoder: ERROR - " + error);
            // Wait for active tasks
            for (auto& task : active_tasks) {
                task.wait();
            }
            is_transcoding_ = false;
            if (completion_callback) completion_callback(false, error);
            return;
        }

        // Poll for completed tasks (check with 10ms timeout)
        bool any_completed = false;
        for (size_t i = 0; i < active_tasks.size(); ) {
            auto status = active_tasks[i].wait_for(std::chrono::milliseconds(0));
            if (status == std::future_status::ready) {
                // Task completed - get result (this also handles exceptions)
                try {
                    active_tasks[i].get();
                } catch (const std::exception& e) {
                    Debug::Log("EXRTranscoder: Exception in transcode task: " + std::string(e.what()));
                    failed_count_.fetch_add(1);
                }

                // Remove completed task
                active_tasks.erase(active_tasks.begin() + i);
                active_indices.erase(active_indices.begin() + i);
                any_completed = true;

                // Launch next frame if available
                if (next_frame_index < source_files.size()) {
                    launch_task(next_frame_index++);
                }
            } else {
                i++;
            }
        }

        // Update progress (thread-safe)
        int completed = completed_count_.load();
        int total = static_cast<int>(source_files.size());
        if (progress_callback && (any_completed || completed % 10 == 0)) {
            std::string message = "Transcoding frame " + std::to_string(completed) + "/" + std::to_string(total) +
                                " (" + std::to_string(active_tasks.size()) + " active threads)";
            progress_callback(completed, total, message);
        }

        // Small sleep to avoid busy-waiting
        if (!any_completed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Completion
    is_transcoding_ = false;
    int completed = completed_count_.load();
    int failed = failed_count_.load();

    if (failed == 0) {
        Debug::Log("EXRTranscoder: Transcode complete - " + std::to_string(completed) + " frames");
        if (completion_callback) completion_callback(true, "");
    } else {
        std::string error = "Transcode completed with " + std::to_string(failed) + " failures";
        Debug::Log("EXRTranscoder: WARNING - " + error);
        if (completion_callback) completion_callback(true, error);  // Still success, but with warnings
    }
}

//=============================================================================
// Single Frame Transcode
//=============================================================================

bool EXRTranscoder::TranscodeFrame(const std::string& source_path,
                                   const std::string& dest_path,
                                   const std::string& layer,
                                   int target_width,
                                   int target_height,
                                   Imf::Compression compression,
                                   std::string& error_message) {
    try {
        // Read source EXR
        auto stream = std::make_unique<MemoryMappedIStream>(source_path);
        Imf::MultiPartInputFile file(*stream);

        const Imf::Header& header = file.header(0);
        const Imath::Box2i displayWindow = header.displayWindow();

        int source_width = displayWindow.max.x - displayWindow.min.x + 1;
        int source_height = displayWindow.max.y - displayWindow.min.y + 1;

        // Allocate source pixel buffer
        std::vector<half> source_pixels(source_width * source_height * 4);  // RGBA

        // Setup framebuffer to read RGBA from layer
        const Imf::ChannelList& channels = header.channels();
        Imf::FrameBuffer frameBuffer;

        std::string layerPrefix = layer.empty() ? "" : (layer + ".");
        std::string channelR = layerPrefix + "R";
        std::string channelG = layerPrefix + "G";
        std::string channelB = layerPrefix + "B";
        std::string channelA = layerPrefix + "A";

        const Imf::Channel* chR = channels.findChannel(channelR.c_str());
        const Imf::Channel* chG = channels.findChannel(channelG.c_str());
        const Imf::Channel* chB = channels.findChannel(channelB.c_str());
        const Imf::Channel* chA = channels.findChannel(channelA.c_str());

        // Fallback to root-level channels
        if (!chR && !layer.empty()) {
            channelR = "R";
            channelG = "G";
            channelB = "B";
            channelA = "A";
            chR = channels.findChannel("R");
            chG = channels.findChannel("G");
            chB = channels.findChannel("B");
            chA = channels.findChannel("A");
        }

        if (!chR || !chG || !chB) {
            error_message = "Missing RGB channels for layer '" + layer + "'";
            return false;
        }

        // Setup frame buffer for interleaved RGBA
        size_t xStride = 4 * sizeof(half);
        size_t yStride = source_width * xStride;

        char* base = reinterpret_cast<char*>(source_pixels.data()) - displayWindow.min.x * xStride - displayWindow.min.y * yStride;

        frameBuffer.insert(channelR.c_str(), Imf::Slice(Imf::HALF, base + 0 * sizeof(half), xStride, yStride));
        frameBuffer.insert(channelG.c_str(), Imf::Slice(Imf::HALF, base + 1 * sizeof(half), xStride, yStride));
        frameBuffer.insert(channelB.c_str(), Imf::Slice(Imf::HALF, base + 2 * sizeof(half), xStride, yStride));

        if (chA) {
            frameBuffer.insert(channelA.c_str(), Imf::Slice(Imf::HALF, base + 3 * sizeof(half), xStride, yStride));
        } else {
            // Fill alpha with 1.0 if no alpha channel
            for (size_t i = 0; i < source_pixels.size(); i += 4) {
                source_pixels[i + 3] = half(1.0f);
            }
        }

        // Read pixels
        Imf::InputPart part(file, 0);
        part.setFrameBuffer(frameBuffer);
        part.readPixels(displayWindow.min.y, displayWindow.max.y);

        // Resize if needed
        std::vector<half> target_pixels;
        bool needs_resize = (target_width != source_width || target_height != source_height);

        if (needs_resize) {
            target_pixels.resize(target_width * target_height * 4);
            if (!ResizePixels(source_pixels, source_width, source_height,
                            target_pixels, target_width, target_height)) {
                error_message = "Failed to resize pixels";
                return false;
            }
        } else {
            target_pixels = std::move(source_pixels);
        }

        // Write single-layer EXR
        Imf::Header out_header(target_width, target_height);
        out_header.compression() = compression;
        out_header.channels().insert("R", Imf::Channel(Imf::HALF));
        out_header.channels().insert("G", Imf::Channel(Imf::HALF));
        out_header.channels().insert("B", Imf::Channel(Imf::HALF));
        out_header.channels().insert("A", Imf::Channel(Imf::HALF));

        Imf::OutputFile out_file(dest_path.c_str(), out_header);

        // Setup output framebuffer
        Imf::FrameBuffer out_frameBuffer;
        size_t out_xStride = 4 * sizeof(half);
        size_t out_yStride = target_width * out_xStride;
        char* out_base = reinterpret_cast<char*>(target_pixels.data());

        out_frameBuffer.insert("R", Imf::Slice(Imf::HALF, out_base + 0 * sizeof(half), out_xStride, out_yStride));
        out_frameBuffer.insert("G", Imf::Slice(Imf::HALF, out_base + 1 * sizeof(half), out_xStride, out_yStride));
        out_frameBuffer.insert("B", Imf::Slice(Imf::HALF, out_base + 2 * sizeof(half), out_xStride, out_yStride));
        out_frameBuffer.insert("A", Imf::Slice(Imf::HALF, out_base + 3 * sizeof(half), out_xStride, out_yStride));

        out_file.setFrameBuffer(out_frameBuffer);
        out_file.writePixels(target_height);

        return true;

    } catch (const std::exception& e) {
        error_message = std::string(e.what());
        return false;
    }
}

//=============================================================================
// Pixel Resize (swscale)
//=============================================================================

//=============================================================================
// TIFF/PNG → EXR Transcode
//=============================================================================

bool EXRTranscoder::TranscodeImageToEXR(const std::string& source_path,
                                        const std::string& dest_path,
                                        int target_width,
                                        int target_height,
                                        Imf::Compression compression,
                                        std::string& error_message) {
    try {
        // Detect source format from extension
        std::string ext = source_path.substr(source_path.find_last_of('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool is_tiff = (ext == ".tif" || ext == ".tiff");
        bool is_png = (ext == ".png");

        if (!is_tiff && !is_png) {
            error_message = "Unsupported format (expected TIFF or PNG): " + ext;
            return false;
        }

        // Load source image using appropriate loader
        std::vector<uint8_t> pixel_data;
        int source_width = 0;
        int source_height = 0;
        PipelineMode mode = PipelineMode::NORMAL;

        bool loaded = false;
        if (is_tiff) {
            loaded = TIFFLoader::Load(source_path, pixel_data, source_width, source_height, mode);
        } else if (is_png) {
            loaded = PNGLoader::Load(source_path, pixel_data, source_width, source_height, mode);
        }

        if (!loaded) {
            error_message = "Failed to load source image";
            return false;
        }

        // Convert pixels to half-float RGBA
        std::vector<half> source_pixels_half;
        source_pixels_half.resize(source_width * source_height * 4);

        if (mode == PipelineMode::NORMAL) {
            // 8-bit RGBA → half-float RGBA
            for (size_t i = 0; i < source_pixels_half.size(); i++) {
                source_pixels_half[i] = half(pixel_data[i] / 255.0f);
            }
        } else if (mode == PipelineMode::HIGH_RES) {
            // 16-bit RGBA (uint16) → half-float RGBA
            const uint16_t* pixels16 = reinterpret_cast<const uint16_t*>(pixel_data.data());
            for (size_t i = 0; i < source_pixels_half.size(); i++) {
                source_pixels_half[i] = half(pixels16[i] / 65535.0f);
            }
        } else if (mode == PipelineMode::ULTRA_HIGH_RES) {
            // 16-bit float → half-float (direct cast from float)
            const uint16_t* pixels16 = reinterpret_cast<const uint16_t*>(pixel_data.data());
            for (size_t i = 0; i < source_pixels_half.size(); i++) {
                // 16-bit float data is already in half format
                source_pixels_half[i] = half(half::FromBits, pixels16[i]);
            }
        }

        // Resize if needed
        std::vector<half> target_pixels;
        bool needs_resize = (target_width > 0 && target_height > 0 &&
                            (target_width != source_width || target_height != source_height));

        if (needs_resize) {
            target_pixels.resize(target_width * target_height * 4);
            if (!ResizePixels(source_pixels_half, source_width, source_height,
                            target_pixels, target_width, target_height)) {
                error_message = "Failed to resize pixels";
                return false;
            }
        } else {
            // Use native resolution
            target_width = source_width;
            target_height = source_height;
            target_pixels = std::move(source_pixels_half);
        }

        // Write single-layer EXR with specified compression
        Imf::Header out_header(target_width, target_height);
        out_header.compression() = compression;
        out_header.channels().insert("R", Imf::Channel(Imf::HALF));
        out_header.channels().insert("G", Imf::Channel(Imf::HALF));
        out_header.channels().insert("B", Imf::Channel(Imf::HALF));
        out_header.channels().insert("A", Imf::Channel(Imf::HALF));

        Imf::OutputFile out_file(dest_path.c_str(), out_header);

        // Setup output framebuffer
        Imf::FrameBuffer out_frameBuffer;
        size_t out_xStride = 4 * sizeof(half);
        size_t out_yStride = target_width * out_xStride;
        char* out_base = reinterpret_cast<char*>(target_pixels.data());

        out_frameBuffer.insert("R", Imf::Slice(Imf::HALF, out_base + 0 * sizeof(half), out_xStride, out_yStride));
        out_frameBuffer.insert("G", Imf::Slice(Imf::HALF, out_base + 1 * sizeof(half), out_xStride, out_yStride));
        out_frameBuffer.insert("B", Imf::Slice(Imf::HALF, out_base + 2 * sizeof(half), out_xStride, out_yStride));
        out_frameBuffer.insert("A", Imf::Slice(Imf::HALF, out_base + 3 * sizeof(half), out_xStride, out_yStride));

        out_file.setFrameBuffer(out_frameBuffer);
        out_file.writePixels(target_height);

        return true;

    } catch (const std::exception& e) {
        error_message = std::string(e.what());
        return false;
    }
}

//=============================================================================
// Pixel Resize (swscale)
//=============================================================================

bool EXRTranscoder::ResizePixels(const std::vector<half>& src_pixels,
                                 int src_width, int src_height,
                                 std::vector<half>& dst_pixels,
                                 int dst_width, int dst_height) {
    // Convert half-float to float for stb_image_resize2
    std::vector<float> src_float(src_pixels.size());
    for (size_t i = 0; i < src_pixels.size(); i++) {
        // Direct bit manipulation to avoid lookup table
        uint16_t h = src_pixels[i].bits();
        uint32_t sign = (h & 0x8000) << 16;
        uint32_t exponent = (h & 0x7C00) >> 10;
        uint32_t mantissa = (h & 0x03FF);

        if (exponent == 0) {
            src_float[i] = 0.0f;
        } else if (exponent == 31) {
            uint32_t f_bits = sign | 0x7F800000 | (mantissa << 13);
            memcpy(&src_float[i], &f_bits, sizeof(float));
        } else {
            uint32_t f_exp = (exponent - 15 + 127) << 23;
            uint32_t f_mant = mantissa << 13;
            uint32_t f_bits = sign | f_exp | f_mant;
            memcpy(&src_float[i], &f_bits, sizeof(float));
        }
    }

    // Allocate destination buffer
    std::vector<float> dst_float(dst_width * dst_height * 4);

    // Use stb_image_resize2 - NO MUTEX NEEDED! Fully thread-safe!
    // STBIR_RGBA = 4 channels, high quality cubic filter
    stbir_resize_float_linear(
        src_float.data(), src_width, src_height, 0,  // src
        dst_float.data(), dst_width, dst_height, 0,  // dst
        STBIR_RGBA  // 4 channels (RGBA)
    );

    // Convert float back to half-float
    dst_pixels.resize(dst_float.size());
    for (size_t i = 0; i < dst_float.size(); i++) {
        uint32_t f_bits;
        memcpy(&f_bits, &dst_float[i], sizeof(float));

        uint32_t sign = (f_bits & 0x80000000) >> 16;
        int32_t exponent = ((f_bits & 0x7F800000) >> 23) - 127 + 15;
        uint32_t mantissa = (f_bits & 0x007FFFFF) >> 13;

        uint16_t h;
        if (exponent <= 0) {
            h = sign;
        } else if (exponent >= 31) {
            h = sign | 0x7C00;
        } else {
            h = sign | ((exponent & 0x1F) << 10) | (mantissa & 0x3FF);
        }

        dst_pixels[i] = half(half::FromBits, h);
    }

    return true;
}

} // namespace ump
