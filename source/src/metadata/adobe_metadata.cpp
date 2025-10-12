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