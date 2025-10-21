#include "adobe_metadata.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <vector>
#endif

namespace fs = std::filesystem;

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
#endif

    // Check in current working directory's assets/exiftool
    fs::path cwd = fs::current_path();
    Debug::Log("Current working directory: " + cwd.string());

    fs::path cwd_assets_exiftool = cwd / "assets" / "exiftool" / "exiftool.exe";
    Debug::Log("Checking: " + cwd_assets_exiftool.string());
    if (fs::exists(cwd_assets_exiftool)) {
        Debug::Log("FOUND in cwd/assets/exiftool: " + cwd_assets_exiftool.string());
        return cwd_assets_exiftool.string();
    }

    Debug::Log("WARNING: ExifTool not found in expected locations");
    return "exiftool.exe";  // Last resort - hope it's in PATH
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

    if (!fs::exists(exiftool_path)) {
        Debug::Log("ERROR: ExifTool not found at: " + exiftool_path);
        return metadata;
    }

#ifdef _WIN32
    // CMD
    std::stringstream cmdline;
    cmdline << "\"" << exiftool_path << "\" "
        << "-s "  // Short output format
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

    // Stdout
    HANDLE hStdOutRead, hStdOutWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
        Debug::Log("ERROR: Failed to create pipe");
        return metadata;
    }

    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    // Startup info - HIDE WINDOW
    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.hStdError = hStdOutWrite;
    si.hStdOutput = hStdOutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };

    std::vector<char> cmdline_buffer(cmdline_str.begin(), cmdline_str.end());
    cmdline_buffer.push_back('\0');

    if (!CreateProcessA(
        NULL,
        cmdline_buffer.data(),
        NULL, NULL, TRUE,
        CREATE_NO_WINDOW,  // CRITICAL: Hide the window
        NULL, NULL,
        &si, &pi
    )) {
        Debug::Log("ERROR: Failed to create process. Error: " + std::to_string(GetLastError()));
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdOutRead);
        return metadata;
    }

    CloseHandle(hStdOutWrite);

    // Read output
    std::string output;
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

    Debug::Log("Raw output length: " + std::to_string(output.length()));
    if (!output.empty()) {
        Debug::Log("Raw output:\n" + output);

        // Parse the output
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

        // === EXTRACT TIMECODE FIELDS ===
        Debug::Log("=== Extracting Timecode Fields ===");

        // QuickTime timecodes
        if (fields.find("StartTimecode") != fields.end()) {
            metadata->qt_start_timecode = fields["StartTimecode"];
            // Trim any extra whitespace/newlines
            metadata->qt_start_timecode.erase(0, metadata->qt_start_timecode.find_first_not_of(" \t\r\n"));
            metadata->qt_start_timecode.erase(metadata->qt_start_timecode.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found QT StartTimecode: '" + metadata->qt_start_timecode + "'");
        }
        if (fields.find("TimeCode") != fields.end()) {
            metadata->qt_timecode = fields["TimeCode"];
            // Trim any extra whitespace/newlines
            metadata->qt_timecode.erase(0, metadata->qt_timecode.find_first_not_of(" \t\r\n"));
            metadata->qt_timecode.erase(metadata->qt_timecode.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found QT TimeCode: '" + metadata->qt_timecode + "'");
        }
        if (fields.find("CreationDate") != fields.end()) {
            metadata->qt_creation_date = fields["CreationDate"];
            // Trim any extra whitespace/newlines
            metadata->qt_creation_date.erase(0, metadata->qt_creation_date.find_first_not_of(" \t\r\n"));
            metadata->qt_creation_date.erase(metadata->qt_creation_date.find_last_not_of(" \t\r\n") + 1);
            Debug::Log("Found QT CreationDate: '" + metadata->qt_creation_date + "'");
        }
        if (fields.find("MediaCreateDate") != fields.end()) {
            metadata->qt_media_create_date = fields["MediaCreateDate"];
            // Trim any extra whitespace/newlines
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

        metadata->is_loaded = true;
        Debug::Log("Adobe + Timecode metadata extraction completed successfully");
        Debug::Log("Has any timecode: " + std::string(metadata->HasAnyTimecode() ? "YES" : "NO"));
    }
    else {
        Debug::Log("WARNING: No output from ExifTool");
    }

#endif

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

#ifdef _WIN32
    // Execute ExifTool (hidden window, no console popup)
    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };

    std::vector<char> cmdline_buffer(cmdline_str.begin(), cmdline_str.end());
    cmdline_buffer.push_back('\0');

    if (!CreateProcessA(
        NULL,
        cmdline_buffer.data(),
        NULL, NULL, FALSE,
        CREATE_NO_WINDOW,
        NULL, NULL,
        &si, &pi
    )) {
        Debug::Log("ERROR: Failed to execute ExifTool. Error: " + std::to_string(GetLastError()));
        return false;
    }

    // Wait for completion
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode == 0) {
        Debug::Log("Metadata written successfully to " + output_file);
        return true;
    } else {
        Debug::Log("ERROR: ExifTool returned exit code: " + std::to_string(exitCode));
        return false;
    }
#else
    // Unix/Mac - use system() for simplicity
    int result = system(cmdline_str.c_str());
    if (result == 0) {
        Debug::Log("Metadata written successfully to " + output_file);
        return true;
    } else {
        Debug::Log("ERROR: ExifTool command failed with code: " + std::to_string(result));
        return false;
    }
#endif
}