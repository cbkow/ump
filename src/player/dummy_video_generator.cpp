#include "dummy_video_generator.h"
#include "../utils/debug_utils.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <iomanip>
#include <algorithm>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define popen _popen
    #define pclose _pclose
    #define mkdir(path, mode) _mkdir(path)
#else
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace ump {

DummyVideoGenerator::DummyVideoGenerator()
    : temp_dir("temp/dummies/")
    , initialized(false) {
    Initialize();
}

DummyVideoGenerator::~DummyVideoGenerator() {
    // Cleanup cache on shutdown if requested (blocking operation)
    if (clear_cache_on_exit_) {
        CleanupCache();
    }
}

void DummyVideoGenerator::Initialize() {
    if (initialized) return;

    // Initialize FFmpeg library
    av_log_set_level(AV_LOG_QUIET); // Suppress FFmpeg logs

    Debug::Log("FFmpeg library initialized");

    // Use %localappdata%\ump\dummies\ for persistent cache
    SetupLocalAppDataDirectory();

    // Create temp directory if it doesn't exist
    std::filesystem::create_directories(temp_dir);

    // Lazy cleanup: Remove old cache files (older than retention days)
    try {
        auto now = std::filesystem::file_time_type::clock::now();
        auto retention_cutoff = now - std::chrono::hours(24 * cache_retention_days_);

        size_t removed_count = 0;
        size_t total_size = 0;

        for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
            if (entry.is_regular_file()) {
                auto last_write = std::filesystem::last_write_time(entry.path());
                total_size += entry.file_size();

                if (last_write < retention_cutoff) {
                    std::filesystem::remove(entry.path());
                    removed_count++;
                }
            }
        }

        if (removed_count > 0) {
            Debug::Log("DummyVideoGenerator: Cleaned up " + std::to_string(removed_count) +
                      " old cache files (older than " + std::to_string(cache_retention_days_) + " days)");
        }

        // Additional cleanup if cache exceeds size limit
        size_t max_bytes = static_cast<size_t>(cache_max_gb_) * 1024ULL * 1024ULL * 1024ULL;
        if (total_size > max_bytes) {
            Debug::Log("DummyVideoGenerator: Cache size exceeds " + std::to_string(cache_max_gb_) + " GB (" +
                      std::to_string(total_size / (1024 * 1024)) + " MB), cleaning all files");
            CleanupCache();
        }
    } catch (const std::exception& e) {
        Debug::Log("DummyVideoGenerator: Cleanup warning - " + std::string(e.what()));
    }

    initialized = true;
    Debug::Log("DummyVideoGenerator initialized with temp dir: " + temp_dir);
}

std::string DummyVideoGenerator::GetDummyFor(int width, int height, double fps) {
    // Delegate to the 4-parameter version with 1 second duration (for backward compatibility)
    return GetDummyFor(width, height, fps, 1.0);
}

std::string DummyVideoGenerator::CreateFastDummy(int width, int height, double fps) {
    // Generate output filename
    std::ostringstream filename_stream;
    filename_stream << "dummy_" << width << "x" << height << "_"
                   << std::fixed << std::setprecision(1) << fps << "fps.mp4";

    std::string output_path = temp_dir + filename_stream.str();

    Debug::Log("Creating dummy video using FFmpeg API: " + output_path);

    // Create the dummy video using FFmpeg API
    if (CreateDummyVideoWithAPI(output_path, width, height, fps)) {
        Debug::Log("Successfully created dummy video: " + output_path);
        return output_path;
    } else {
        Debug::Log("ERROR: Failed to create dummy video with FFmpeg API");
        return "";
    }
}

std::string DummyVideoGenerator::GetDummyFor(int width, int height, double fps, double duration) {
    if (!initialized) Initialize();

    // Generate cache key including duration
    std::ostringstream cache_key_stream;
    cache_key_stream << width << "x" << height << "_"
                    << std::fixed << std::setprecision(1) << fps << "fps_"
                    << std::setprecision(2) << duration << "s";
    std::string cache_key = cache_key_stream.str();

    // Check if we already have this dummy cached
    auto cache_it = cache.find(cache_key);
    if (cache_it != cache.end()) {
        const std::string& cached_path = cache_it->second;
        if (FileExists(cached_path)) {
            Debug::Log("Using cached dummy: " + cached_path);
            return cached_path;
        } else {
            // Cache entry is stale, remove it
            cache.erase(cache_it);
        }
    }

    // Create new dummy video with specific duration
    std::string dummy_path = CreateFastDummy(width, height, fps, duration);

    if (!dummy_path.empty() && FileExists(dummy_path)) {
        // Cache the successful result
        cache[cache_key] = dummy_path;
        Debug::Log("Created and cached new duration-specific dummy: " + dummy_path);
        return dummy_path;
    }

    Debug::Log("ERROR: Failed to create duration-specific dummy video for " + cache_key);
    return "";
}

std::string DummyVideoGenerator::CreateFastDummy(int width, int height, double fps, double duration) {
    // Generate output filename with duration
    std::ostringstream filename_stream;
    filename_stream << "dummy_" << width << "x" << height << "_"
                   << std::fixed << std::setprecision(1) << fps << "fps_"
                   << std::setprecision(2) << duration << "s.mp4";

    std::string output_path = temp_dir + filename_stream.str();

    Debug::Log("Creating duration-specific dummy video using FFmpeg API: " + output_path + " (" + std::to_string(duration) + "s)");

    // Create the dummy video using FFmpeg API with specific duration
    if (CreateDummyVideoWithAPI(output_path, width, height, fps, duration)) {
        Debug::Log("Successfully created duration-specific dummy video: " + output_path);
        return output_path;
    } else {
        Debug::Log("ERROR: Failed to create duration-specific dummy video with FFmpeg API");
        return "";
    }
}

bool DummyVideoGenerator::CreateDummyVideoWithAPI(const std::string& output_path, int width, int height, double fps) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    const AVCodec* codec = nullptr;
    AVStream* stream = nullptr;
    int ret = 0;

    // Allocate format context
    ret = avformat_alloc_output_context2(&format_ctx, nullptr, nullptr, output_path.c_str());
    if (ret < 0) {
        Debug::Log("ERROR: Could not allocate output format context");
        return false;
    }

    // Find H.264 encoder
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        Debug::Log("ERROR: H.264 codec not found");
        avformat_free_context(format_ctx);
        return false;
    }

    // Create stream
    stream = avformat_new_stream(format_ctx, codec);
    if (!stream) {
        Debug::Log("ERROR: Could not create stream");
        avformat_free_context(format_ctx);
        return false;
    }

    // Allocate codec context
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        Debug::Log("ERROR: Could not allocate codec context");
        avformat_free_context(format_ctx);
        return false;
    }

    // Set codec parameters for fast, minimal quality encoding
    codec_ctx->bit_rate = 400000; // Low bitrate for minimal file size
    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = {1, (int)fps}; // Use natural frame timing to match FFmpeg command
    codec_ctx->framerate = {(int)fps, 1};
    codec_ctx->gop_size = 1; // I-frame only
    codec_ctx->max_b_frames = 0; // No B-frames
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    // Fast encoding preset equivalent
    av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codec_ctx->priv_data, "tune", "fastdecode", 0);
    av_opt_set_int(codec_ctx->priv_data, "crf", 51, 0); // Lowest quality

    // Some formats want stream headers to be separate
    if (format_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open codec
    ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret < 0) {
        Debug::Log("ERROR: Could not open codec");
        avcodec_free_context(&codec_ctx);
        avformat_free_context(format_ctx);
        return false;
    }

    // Copy codec parameters to stream
    ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
    if (ret < 0) {
        Debug::Log("ERROR: Could not copy codec parameters");
        avcodec_free_context(&codec_ctx);
        avformat_free_context(format_ctx);
        return false;
    }

    // Open output file
    if (!(format_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&format_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            Debug::Log("ERROR: Could not open output file");
            avcodec_free_context(&codec_ctx);
            avformat_free_context(format_ctx);
            return false;
        }
    }


    // Let FFmpeg calculate duration automatically from frame count and time_base
    // Set frame count metadata for 1-second video (equivalent to -frames:v)
    stream->nb_frames = (int)fps;

    // Write file header
    ret = avformat_write_header(format_ctx, nullptr);
    if (ret < 0) {
        Debug::Log("ERROR: Could not write header");
        avcodec_free_context(&codec_ctx);
        if (!(format_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&format_ctx->pb);
        avformat_free_context(format_ctx);
        return false;
    }

    // Allocate frame
    frame = av_frame_alloc();
    if (!frame) {
        Debug::Log("ERROR: Could not allocate frame");
        avcodec_free_context(&codec_ctx);
        if (!(format_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&format_ctx->pb);
        avformat_free_context(format_ctx);
        return false;
    }

    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        Debug::Log("ERROR: Could not allocate frame buffer");
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        if (!(format_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&format_ctx->pb);
        avformat_free_context(format_ctx);
        return false;
    }

    // Create exactly fps frames for 1 second duration
    // TODO: Make this configurable based on target sequence length
    int total_frames = (int)fps;

    for (int i = 0; i < total_frames; i++) {
        ret = av_frame_make_writable(frame);
        if (ret < 0) break;

        // Fill frame with black (Y=16, U=128, V=128 for YUV420P)
        memset(frame->data[0], 16, frame->linesize[0] * codec_ctx->height); // Y plane
        memset(frame->data[1], 128, frame->linesize[1] * codec_ctx->height / 2); // U plane
        memset(frame->data[2], 128, frame->linesize[2] * codec_ctx->height / 2); // V plane

        // Natural frame timing: each frame advances by 1 time unit
        frame->pts = i;

        // Encode frame
        ret = avcodec_send_frame(codec_ctx, frame);
        if (ret < 0) {
            Debug::Log("ERROR: Error sending frame to encoder");
            break;
        }

        while (ret >= 0) {
            packet = av_packet_alloc();
            ret = avcodec_receive_packet(codec_ctx, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&packet);
                break;
            } else if (ret < 0) {
                Debug::Log("ERROR: Error receiving packet from encoder");
                av_packet_free(&packet);
                break;
            }

            // Rescale packet timestamp
            av_packet_rescale_ts(packet, codec_ctx->time_base, stream->time_base);
            packet->stream_index = stream->index;

            // Write packet
            ret = av_interleaved_write_frame(format_ctx, packet);
            av_packet_free(&packet);
            if (ret < 0) {
                Debug::Log("ERROR: Error writing packet");
                break;
            }
        }
    }

    // Flush encoder
    avcodec_send_frame(codec_ctx, nullptr);
    while (ret >= 0) {
        packet = av_packet_alloc();
        ret = avcodec_receive_packet(codec_ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&packet);
            break;
        }

        av_packet_rescale_ts(packet, codec_ctx->time_base, stream->time_base);
        packet->stream_index = stream->index;
        av_interleaved_write_frame(format_ctx, packet);
        av_packet_free(&packet);
    }

    // Write trailer
    av_write_trailer(format_ctx);

    // Cleanup
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    if (!(format_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&format_ctx->pb);
    avformat_free_context(format_ctx);

    return FileExists(output_path);
}

bool DummyVideoGenerator::CreateDummyVideoWithAPI(const std::string& output_path, int width, int height, double fps, double duration) {
    Debug::Log("Creating duration-specific dummy with " + std::to_string(duration) + "s duration using FFmpeg executable");

    // Determine pixel format based on dimensions
    // yuv420p requires even dimensions (better compression)
    // yuv444p supports any dimensions (fallback for odd resolutions)
    bool has_odd_dimension = (width % 2 != 0 || height % 2 != 0);
    const char* pix_fmt = has_odd_dimension ? "yuv444p" : "yuv420p";

    if (has_odd_dimension) {
        Debug::Log("Using yuv444p for odd resolution: " + std::to_string(width) + "x" + std::to_string(height));
    }

    // Build FFmpeg command using the working approach with silent output
    std::ostringstream cmd;
    cmd << "ffmpeg.exe -y -v error -f lavfi -i color=black:size=" << width << "x" << height
        << ":duration=" << std::fixed << std::setprecision(2) << duration
        << ":rate=" << (int)fps
        << " -c:v libx264 -preset ultrafast -tune fastdecode -crf 51 -g 1 -keyint_min 1 -pix_fmt " << pix_fmt << " \""
        << output_path << "\"";

    std::string command = cmd.str();
    Debug::Log("Executing FFmpeg command: " + command);

#ifdef _WIN32
    // Windows-specific: Create process with hidden window
    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // Hide the console window

    // Convert to char array for CreateProcessA
    std::vector<char> cmd_chars(command.begin(), command.end());
    cmd_chars.push_back('\0');

    BOOL success = CreateProcessA(
        nullptr,                // No module name (use command line)
        cmd_chars.data(),       // Command line
        nullptr,                // Process handle not inheritable
        nullptr,                // Thread handle not inheritable
        FALSE,                  // Set handle inheritance to FALSE
        CREATE_NO_WINDOW,       // Creation flags - no console window
        nullptr,                // Use parent's environment block
        nullptr,                // Use parent's starting directory
        &si,                    // Pointer to STARTUPINFO structure
        &pi                     // Pointer to PROCESS_INFORMATION structure
    );

    if (!success) {
        Debug::Log("ERROR: Failed to start FFmpeg process");
        return false;
    }

    // Wait for the process to complete
    Debug::Log("Waiting for FFmpeg to complete...");
    DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000); // 30 second timeout

    DWORD exit_code = 1;
    if (wait_result == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    } else if (wait_result == WAIT_TIMEOUT) {
        Debug::Log("ERROR: FFmpeg process timed out");
        TerminateProcess(pi.hProcess, 1);
        exit_code = 1;
    }

    // Clean up handles
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code == 0) {
        Debug::Log("Successfully created duration-specific dummy video: " + output_path);
        return FileExists(output_path);
    } else {
        Debug::Log("ERROR: FFmpeg command failed with exit code: " + std::to_string(exit_code));
        return false;
    }
#else
    // Non-Windows fallback
    int result = system(command.c_str());
    if (result == 0) {
        Debug::Log("Successfully created duration-specific dummy video: " + output_path);
        return FileExists(output_path);
    } else {
        Debug::Log("ERROR: FFmpeg command failed with exit code: " + std::to_string(result));
        return false;
    }
#endif
}

std::string DummyVideoGenerator::GenerateCacheKey(int width, int height, double fps) {
    std::ostringstream key_stream;
    key_stream << width << "x" << height << "_"
               << std::fixed << std::setprecision(1) << fps;
    return key_stream.str();
}

bool DummyVideoGenerator::FileExists(const std::string& path) {
    return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
}


void DummyVideoGenerator::SetTempDirectory(const std::string& temp_dir_path) {
    temp_dir = temp_dir_path;
    if (!temp_dir.empty() && temp_dir.back() != '/' && temp_dir.back() != '\\') {
        temp_dir += "/";
    }

    // Create directory if it doesn't exist
    std::filesystem::create_directories(temp_dir);

    Debug::Log("DummyVideoGenerator temp directory set to: " + temp_dir);
}

bool DummyVideoGenerator::HasCachedDummy(int width, int height, double fps) {
    std::string cache_key = GenerateCacheKey(width, height, fps);
    auto cache_it = cache.find(cache_key);

    if (cache_it != cache.end()) {
        return FileExists(cache_it->second);
    }

    return false;
}

void DummyVideoGenerator::SetupLocalAppDataDirectory() {
    // Use custom cache path if set, otherwise use %localappdata%
    if (!custom_cache_path_.empty()) {
        std::filesystem::path custom_path = std::filesystem::path(custom_cache_path_) / "dummies";
        temp_dir = custom_path.string();

        // Ensure proper path separator
        if (!temp_dir.empty() && temp_dir.back() != std::filesystem::path::preferred_separator) {
            temp_dir += std::filesystem::path::preferred_separator;
        }

        Debug::Log("Using custom cache path for dummy cache: " + temp_dir);
        return;
    }

    // Get %localappdata% environment variable
    const char* localappdata = std::getenv("LOCALAPPDATA");

    if (localappdata) {
        // Use %localappdata%\ump\dummies\ and keep it as a proper Windows path
        std::filesystem::path app_data_path = std::filesystem::path(localappdata) / "ump" / "dummies";
        temp_dir = app_data_path.string();

        // Ensure proper path separator for the platform
        if (!temp_dir.empty() && temp_dir.back() != std::filesystem::path::preferred_separator) {
            temp_dir += std::filesystem::path::preferred_separator;
        }

        Debug::Log("Using %localappdata% for dummy cache: " + temp_dir);
    } else {
        // Fallback to relative directory if %localappdata% not available
        temp_dir = "temp/dummies/";
        Debug::Log("Warning: %localappdata% not found, using fallback: " + temp_dir);
    }
}

void DummyVideoGenerator::SetCacheConfig(const std::string& custom_path, int retention_days, int max_gb, bool clear_on_exit) {
    custom_cache_path_ = custom_path;
    cache_retention_days_ = retention_days;
    cache_max_gb_ = max_gb;
    clear_cache_on_exit_ = clear_on_exit;

    // Reinitialize if path changed
    if (initialized) {
        initialized = false;
        Initialize();
    }
}

void DummyVideoGenerator::CleanupCache() {
    Debug::Log("Cleaning up dummy video cache...");

    // Remove all cached files
    for (const auto& cache_entry : cache) {
        const std::string& file_path = cache_entry.second;
        if (FileExists(file_path)) {
            try {
                std::filesystem::remove(file_path);
                Debug::Log("Removed cached dummy: " + file_path);
            } catch (const std::exception& e) {
                Debug::Log("Failed to remove cached dummy: " + file_path + " (" + e.what() + ")");
            }
        }
    }

    // Clear cache map
    cache.clear();

    // Optionally remove temp directory if empty
    try {
        if (std::filesystem::is_empty(temp_dir)) {
            std::filesystem::remove(temp_dir);
            Debug::Log("Removed empty temp directory: " + temp_dir);
        }
    } catch (const std::exception& e) {
        Debug::Log("Could not remove temp directory: " + std::string(e.what()));
    }
}

size_t DummyVideoGenerator::ClearAllDummies() {
    Debug::Log("=== Clearing ALL dummy videos from all cache locations ===");

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

    // 1. Clear default location (%LOCALAPPDATA%\ump\dummies\)
    const char* localappdata = std::getenv("LOCALAPPDATA");
    if (localappdata) {
        std::filesystem::path default_path = std::filesystem::path(localappdata) / "ump" / "dummies";
        clear_directory(default_path.string());
    }

    // 2. Clear custom location (if set)
    if (!custom_cache_path_.empty()) {
        std::filesystem::path custom_path = std::filesystem::path(custom_cache_path_) / "dummies";
        clear_directory(custom_path.string());
    }

    // 3. Clear fallback location (temp/dummies/)
    clear_directory("temp/dummies");

    // Clear in-memory cache
    cache.clear();

    Debug::Log("=== Cleared " + std::to_string(total_bytes / 1024.0 / 1024.0) + " MB total from dummies ===");
    return total_bytes;
}

} // namespace ump