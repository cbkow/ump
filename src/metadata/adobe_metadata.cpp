#include "adobe_metadata.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <vector>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// Helper to run exiftool and capture output (cross-platform)
static std::string RunExifToolCmd(const std::string& cmdline) {
    std::string output;

#ifdef _WIN32
    HANDLE hStdOutRead, hStdOutWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
        Debug::Log("ERROR: Failed to create pipe");
        return output;
    }

    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.hStdError = hStdOutWrite;
    si.hStdOutput = hStdOutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };

    std::vector<char> cmdline_buffer(cmdline.begin(), cmdline.end());
    cmdline_buffer.push_back('\0');

    if (!CreateProcessA(NULL, cmdline_buffer.data(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        NULL, NULL, &si, &pi)) {
        Debug::Log("ERROR: Failed to create process. Error: " + std::to_string(GetLastError()));
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdOutRead);
        return output;
    }

    CloseHandle(hStdOutWrite);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    Debug::Log("ExifTool exit code: " + std::to_string(exitCode));

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdOutRead);
#else
    FILE* pipe = popen(cmdline.c_str(), "r");
    if (!pipe) {
        Debug::Log("ERROR: Failed to execute ExifTool via popen");
        return output;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    int status = pclose(pipe);
    Debug::Log("ExifTool exit code: " + std::to_string(WEXITSTATUS(status)));
#endif

    return output;
}

std::string AdobeMetadataExtractor::GetExifToolPath() {
    Debug::Log("=== GetExifToolPath START ===");

#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exe_dir = fs::path(buffer).parent_path();
    Debug::Log("Executable directory: " + exe_dir.string());

    // Check in assets/exiftool subdirectory (PRIMARY LOCATION)
    fs::path assets_exiftool_path = exe_dir / "assets" / "exiftool" / "exiftool.exe";
    Debug::Log("Checking: " + assets_exiftool_path.string());
    if (fs::exists(assets_exiftool_path)) {
        Debug::Log("FOUND in assets/exiftool: " + assets_exiftool_path.string());
        return assets_exiftool_path.string();
    }

    // Check in current working directory's assets/exiftool
    fs::path cwd_assets_exiftool = fs::current_path() / "assets" / "exiftool" / "exiftool.exe";
    Debug::Log("Checking: " + cwd_assets_exiftool.string());
    if (fs::exists(cwd_assets_exiftool)) {
        Debug::Log("FOUND in cwd/assets/exiftool: " + cwd_assets_exiftool.string());
        return cwd_assets_exiftool.string();
    }

    Debug::Log("WARNING: ExifTool not found in expected locations");
    return "exiftool.exe";
#else
    // Linux/macOS: check bundled Perl exiftool in assets/exiftool/
#ifdef __APPLE__
    // macOS: use _NSGetExecutablePath or argv[0] — /proc/self/exe doesn't exist
    // Fall back to PATH lookup since exiftool is commonly installed via Homebrew
    extern std::filesystem::path g_exe_dir;
    fs::path exe_dir = g_exe_dir;
#else
    fs::path exe_path = fs::read_symlink("/proc/self/exe");
    fs::path exe_dir = exe_path.parent_path();
#endif

    fs::path bundled_path = exe_dir / "assets" / "exiftool" / "exiftool";
    if (fs::exists(bundled_path)) {
        Debug::Log("FOUND bundled exiftool: " + bundled_path.string());
        return bundled_path.string();
    }

    fs::path cwd_path = fs::current_path() / "assets" / "exiftool" / "exiftool";
    if (fs::exists(cwd_path)) {
        Debug::Log("FOUND exiftool in cwd: " + cwd_path.string());
        return cwd_path.string();
    }

    Debug::Log("WARNING: Bundled ExifTool not found, falling back to system PATH");
    return "exiftool";
#endif
}

std::unordered_map<std::string, std::string> AdobeMetadataExtractor::ParseExifOutput(const std::string& output) {
    std::unordered_map<std::string, std::string> result;
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Trim whitespace from both key and value
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);

            if (!key.empty()) {
                result[key] = value;
            }
        }
    }

    return result;
}

std::unique_ptr<AdobeMetadata> AdobeMetadataExtractor::ExtractAdobePaths(const std::string& file_path) {
    auto metadata = std::make_unique<AdobeMetadata>();

    Debug::Log("=== Adobe Metadata ExtractAdobePaths START ===");
    Debug::Log("Input file: " + file_path);

    if (!fs::exists(file_path)) {
        Debug::Log("ERROR: Input file does not exist");
        return metadata;
    }

    std::string exiftool_path = GetExifToolPath();
    Debug::Log("ExifTool path: " + exiftool_path);

    // Only check fs::exists for absolute paths (system PATH fallback won't exist as a file)
    if (exiftool_path.find('/') != std::string::npos || exiftool_path.find('\\') != std::string::npos) {
        if (!fs::exists(exiftool_path)) {
            Debug::Log("ERROR: ExifTool not found at: " + exiftool_path);
            return metadata;
        }
    }

    // Build command line
    std::stringstream cmdline;
    cmdline << "\"" << exiftool_path << "\" "
        << "-s "
        << "-XMP:AeProjectLinkFullPath "
        << "-XMP:WindowsAtomUncProjectPath "
        << "-XMP:MacAtomPosixProjectPath "
        << "-QuickTime:StartTimecode "
        << "-QuickTime:TimeCode "
        << "-QuickTime:CreationDate "
        << "-QuickTime:MediaCreateDate "
        << "-QuickTime:TrackCreateDate "
        << "-MXF:StartTimecode "
        << "-MXF:TimecodeAtStart "
        << "-MXF:StartOfContent "
        << "-XMP:StartTimecode "
        << "-XMP:AltTimecode "
        << "-XMP:AltTimecodeTimeValue "
        << "-XMP:TimeCode "
        << "-UserData:TimeCode "
        << "\"" << file_path << "\"";

    std::string cmdline_str = cmdline.str();
    Debug::Log("Command line: " + cmdline_str);

    std::string output = RunExifToolCmd(cmdline_str);

    Debug::Log("Raw output length: " + std::to_string(output.length()));
    if (!output.empty()) {
        Debug::Log("Raw output:\n" + output);

        auto fields = ParseExifOutput(output);

        // Extract Adobe project paths
        if (fields.find("AeProjectLinkFullPath") != fields.end()) {
            metadata->ae_project_path = fields["AeProjectLinkFullPath"];
        }
        if (fields.find("WindowsAtomUncProjectPath") != fields.end()) {
            metadata->premiere_win_path = fields["WindowsAtomUncProjectPath"];
        }
        if (fields.find("MacAtomPosixProjectPath") != fields.end()) {
            metadata->premiere_mac_path = fields["MacAtomPosixProjectPath"];
        }

        // QuickTime timecodes
        if (fields.find("StartTimecode") != fields.end()) {
            metadata->qt_start_timecode = fields["StartTimecode"];
            metadata->qt_start_timecode.erase(0, metadata->qt_start_timecode.find_first_not_of(" \t\r\n"));
            metadata->qt_start_timecode.erase(metadata->qt_start_timecode.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found QT StartTimecode: '" + metadata->qt_start_timecode + "'");
        }
        if (fields.find("TimeCode") != fields.end()) {
            metadata->qt_timecode = fields["TimeCode"];
            metadata->qt_timecode.erase(0, metadata->qt_timecode.find_first_not_of(" \t\r\n"));
            metadata->qt_timecode.erase(metadata->qt_timecode.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found QT TimeCode: '" + metadata->qt_timecode + "'");
        }
        if (fields.find("CreationDate") != fields.end()) {
            metadata->qt_creation_date = fields["CreationDate"];
            metadata->qt_creation_date.erase(0, metadata->qt_creation_date.find_first_not_of(" \t\r\n"));
            metadata->qt_creation_date.erase(metadata->qt_creation_date.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found QT CreationDate: '" + metadata->qt_creation_date + "'");
        }
        if (fields.find("MediaCreateDate") != fields.end()) {
            metadata->qt_media_create_date = fields["MediaCreateDate"];
            metadata->qt_media_create_date.erase(0, metadata->qt_media_create_date.find_first_not_of(" \t\r\n"));
            metadata->qt_media_create_date.erase(metadata->qt_media_create_date.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found QT MediaCreateDate: '" + metadata->qt_media_create_date + "'");
        }

        // XMP timecodes
        if (fields.find("AltTimecode") != fields.end()) {
            metadata->xmp_alt_timecode = fields["AltTimecode"];
            metadata->xmp_alt_timecode.erase(0, metadata->xmp_alt_timecode.find_first_not_of(" \t\r\n"));
            metadata->xmp_alt_timecode.erase(metadata->xmp_alt_timecode.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found XMP AltTimecode: '" + metadata->xmp_alt_timecode + "'");
        }
        if (fields.find("AltTimecodeTimeValue") != fields.end()) {
            metadata->xmp_alt_timecode_time_value = fields["AltTimecodeTimeValue"];
            metadata->xmp_alt_timecode_time_value.erase(0, metadata->xmp_alt_timecode_time_value.find_first_not_of(" \t\r\n"));
            metadata->xmp_alt_timecode_time_value.erase(metadata->xmp_alt_timecode_time_value.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found XMP AltTimecodeTimeValue: '" + metadata->xmp_alt_timecode_time_value + "'");
        }

        Debug::Log("Adobe + Timecode metadata extraction completed successfully");
        Debug::Log("Has any timecode: " + std::string(metadata->HasAnyTimecode() ? "YES" : "NO"));
    } else {
        Debug::Log("ExifTool returned no output (no Adobe metadata in file)");
    }

    metadata->is_loaded = true;

    return metadata;
}

// Helper: Adjust SMPTE timecode by adding frame offset (handles HH:MM:SS:FF format at 24fps)
static std::string AdjustTimecode(const std::string& timecode, int offset_frames, double fps = 24.0) {
    if (timecode.empty() || offset_frames == 0) return timecode;

    // Parse SMPTE timecode: HH:MM:SS:FF
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
    if (sscanf(timecode.c_str(), "%d:%d:%d:%d", &hours, &minutes, &seconds, &frames) != 4) {
        Debug::Log("WARNING: Could not parse timecode: " + timecode);
        return timecode;  // Return unchanged if parsing fails
    }

    // Convert to total frames
    int total_frames = hours * 3600 * static_cast<int>(fps) +
                      minutes * 60 * static_cast<int>(fps) +
                      seconds * static_cast<int>(fps) +
                      frames;

    // Add offset
    total_frames += offset_frames;

    // Handle negative result (trim before timecode start)
    if (total_frames < 0) {
        Debug::Log("WARNING: Timecode offset resulted in negative time, clamping to 00:00:00:00");
        total_frames = 0;
    }

    // Convert back to SMPTE
    int fps_int = static_cast<int>(fps);
    hours = total_frames / (3600 * fps_int);
    total_frames %= (3600 * fps_int);
    minutes = total_frames / (60 * fps_int);
    total_frames %= (60 * fps_int);
    seconds = total_frames / fps_int;
    frames = total_frames % fps_int;

    // Format as HH:MM:SS:FF
    char result[32];
    snprintf(result, sizeof(result), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);

    Debug::Log("Adjusted timecode: " + timecode + " + " + std::to_string(offset_frames) + " frames = " + std::string(result));
    return std::string(result);
}

bool AdobeMetadataExtractor::WriteMetadata(const std::string& output_file, const AdobeMetadata* metadata, int offset_frames) {
    if (!metadata) {
        Debug::Log("WriteMetadata: No metadata provided");
        return false;
    }

    if (!fs::exists(output_file)) {
        Debug::Log("ERROR: WriteMetadata - Output file does not exist: " + output_file);
        return false;
    }

    std::string exiftool_path = GetExifToolPath();
    if (!fs::exists(exiftool_path)) {
        Debug::Log("ERROR: WriteMetadata - ExifTool not found at: " + exiftool_path);
        return false;
    }

    Debug::Log("=== Writing Adobe Metadata to " + output_file + " ===");
    if (offset_frames > 0) {
        Debug::Log("Applying timecode offset: " + std::to_string(offset_frames) + " frames");
    }

    // Build command line with all metadata fields
    std::stringstream cmdline;
    cmdline << "\"" << exiftool_path << "\" ";
    cmdline << "-overwrite_original ";  // Don't create backup files

    int field_count = 0;

    // Adobe project paths
    if (!metadata->ae_project_path.empty()) {
        cmdline << "-XMP:AeProjectLinkFullPath=\"" << metadata->ae_project_path << "\" ";
        field_count++;
        Debug::Log("  Writing AE Project: " + metadata->ae_project_path);
    }
    if (!metadata->premiere_win_path.empty()) {
        cmdline << "-XMP:WindowsAtomUncProjectPath=\"" << metadata->premiere_win_path << "\" ";
        field_count++;
        Debug::Log("  Writing Premiere Windows path: " + metadata->premiere_win_path);
    }
    if (!metadata->premiere_mac_path.empty()) {
        cmdline << "-XMP:MacAtomPosixProjectPath=\"" << metadata->premiere_mac_path << "\" ";
        field_count++;
        Debug::Log("  Writing Premiere Mac path: " + metadata->premiere_mac_path);
    }

    // QuickTime timecodes (adjusted for trim offset)
    if (!metadata->qt_start_timecode.empty()) {
        std::string adjusted_tc = AdjustTimecode(metadata->qt_start_timecode, offset_frames);
        cmdline << "-QuickTime:StartTimecode=\"" << adjusted_tc << "\" ";
        field_count++;
    }
    if (!metadata->qt_timecode.empty()) {
        std::string adjusted_tc = AdjustTimecode(metadata->qt_timecode, offset_frames);
        cmdline << "-QuickTime:TimeCode=\"" << adjusted_tc << "\" ";
        field_count++;
    }
    if (!metadata->qt_creation_date.empty()) {
        cmdline << "-QuickTime:CreationDate=\"" << metadata->qt_creation_date << "\" ";
        field_count++;
    }
    if (!metadata->qt_media_create_date.empty()) {
        cmdline << "-QuickTime:MediaCreateDate=\"" << metadata->qt_media_create_date << "\" ";
        field_count++;
    }

    // XMP timecodes (adjusted for trim offset)
    if (!metadata->xmp_start_timecode.empty()) {
        std::string adjusted_tc = AdjustTimecode(metadata->xmp_start_timecode, offset_frames);
        cmdline << "-XMP:StartTimecode=\"" << adjusted_tc << "\" ";
        field_count++;
    }
    if (!metadata->xmp_alt_timecode.empty()) {
        std::string adjusted_tc = AdjustTimecode(metadata->xmp_alt_timecode, offset_frames);
        cmdline << "-XMP:AltTimecode=\"" << adjusted_tc << "\" ";
        field_count++;
    }
    if (!metadata->xmp_alt_timecode_time_value.empty()) {
        std::string adjusted_tc = AdjustTimecode(metadata->xmp_alt_timecode_time_value, offset_frames);
        cmdline << "-XMP:AltTimecodeTimeValue=\"" << adjusted_tc << "\" ";
        field_count++;
    }
    if (!metadata->xmp_timecode.empty()) {
        std::string adjusted_tc = AdjustTimecode(metadata->xmp_timecode, offset_frames);
        cmdline << "-XMP:TimeCode=\"" << adjusted_tc << "\" ";
        field_count++;
    }

    // UserData timecode (adjusted for trim offset)
    if (!metadata->userdata_timecode.empty()) {
        std::string adjusted_tc = AdjustTimecode(metadata->userdata_timecode, offset_frames);
        cmdline << "-UserData:TimeCode=\"" << adjusted_tc << "\" ";
        field_count++;
    }

    if (field_count == 0) {
        Debug::Log("WriteMetadata: No metadata fields to write");
        return true;  // Not an error, just nothing to do
    }

    cmdline << "\"" << output_file << "\"";

    std::string cmdline_str = cmdline.str();
    Debug::Log("ExifTool command: " + cmdline_str);
    Debug::Log("Writing " + std::to_string(field_count) + " metadata fields");

    std::string output = RunExifToolCmd(cmdline_str);

    // RunExifToolCmd logs exit code; check if output contains error indicators
    if (output.find("Error") != std::string::npos) {
        Debug::Log("ERROR: ExifTool write may have failed. Output: " + output);
        return false;
    }

    Debug::Log("Metadata written successfully to " + output_file);
    return true;
}