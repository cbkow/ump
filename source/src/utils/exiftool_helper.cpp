#include "exiftool_helper.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <memory>
#include "utils/debug_utils.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

std::string ExifToolHelper::GetExifToolPath() {

#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exe_dir = fs::path(buffer).parent_path();

    // Check in assets/exiftool subdirectory (PRIMARY LOCATION)
    fs::path assets_exiftool_path = exe_dir / "assets" / "exiftool" / "exiftool.exe";
    if (fs::exists(assets_exiftool_path)) {
        return assets_exiftool_path.string();
    }
#endif

    // Check in current working directory's assets/exiftool
    fs::path cwd = fs::current_path();

    fs::path cwd_assets_exiftool = cwd / "assets" / "exiftool" / "exiftool.exe";
    if (fs::exists(cwd_assets_exiftool)) {
        return cwd_assets_exiftool.string();
    }

    // Fallback to other locations if needed
    fs::path exe_path = exe_dir / "exiftool.exe";
    if (fs::exists(exe_path)) {
        return exe_path.string();
    }

    Debug::Log("WARNING: ExifTool not found in expected locations");
    return "exiftool.exe";  // Last resort - hope it's in PATH
}

#ifdef _WIN32
#include <windows.h>
#include <vector>

std::unique_ptr<ExifToolHelper::Metadata> ExifToolHelper::ExtractMetadata(const std::string& file_path) {
    auto metadata = std::make_unique<Metadata>();

    Debug::Log("Input file: " + file_path);

    if (!fs::exists(file_path)) {
        Debug::Log("ERROR: Input file does not exist!");
        return metadata;
    }

    std::string exiftool_path = GetExifToolPath();
    Debug::Log("ExifTool path: " + exiftool_path);

    if (!fs::exists(exiftool_path)) {
        Debug::Log("ERROR: ExifTool not found at: " + exiftool_path);
        return metadata;
    }

    // CMD
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

    // Stdout
    HANDLE hStdOutRead, hStdOutWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
        Debug::Log("ERROR: Failed to create pipe");
        return metadata;
    }

    // Ensure read handle is not inherited
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    // Startup info
    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.hStdError = hStdOutWrite;
    si.hStdOutput = hStdOutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };

    // Create the process
    std::vector<char> cmdline_buffer(cmdline_str.begin(), cmdline_str.end());
    cmdline_buffer.push_back('\0');

    if (!CreateProcessA(
        NULL,
        cmdline_buffer.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    )) {
        Debug::Log("ERROR: Failed to create process. Error: " + std::to_string(GetLastError()));
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdOutRead);
        return metadata;
    }

    // Close write end of pipe
    CloseHandle(hStdOutWrite);

    // Read output
    std::string output;
    char buffer[4096];
    DWORD bytesRead;

    while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    // Wait for process to complete
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    Debug::Log("ExifTool exit code: " + std::to_string(exitCode));

    // Clean up
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdOutRead);

    if (!output.empty()) {
        Debug::Log("Raw output:\n" + output);
    }
    else {
        Debug::Log("WARNING: No output from ExifTool");
    }

    // Parse the output
    auto fields = ParseExifOutput(output);

    // Extract the Adobe project paths
    if (fields.count("AeProjectLinkFullPath")) {
        metadata->ae_project_path = fields["AeProjectLinkFullPath"];
    }

    if (fields.count("WindowsAtomUncProjectPath")) {
        std::string path = fields["WindowsAtomUncProjectPath"];
        if (path.length() > 4 && path.substr(0, 4) == "\\\\?\\") {
            path = path.substr(4);
        }
        metadata->premiere_win_path = path;
    }

    if (fields.count("MacAtomPosixProjectPath")) {
        metadata->premiere_mac_path = fields["MacAtomPosixProjectPath"];
    }

    // === EXTRACT TIMECODE FIELDS ===
    if (fields.find("StartTimecode") != fields.end()) {
        metadata->qt_start_timecode = fields["StartTimecode"];
        metadata->has_any_timecode = true;
    }
    if (fields.find("TimeCode") != fields.end()) {
        metadata->qt_timecode = fields["TimeCode"];
        metadata->has_any_timecode = true;
    }
    if (fields.find("CreationDate") != fields.end()) {
        metadata->qt_creation_date = fields["CreationDate"];
    }
    if (fields.find("MediaCreateDate") != fields.end()) {
        metadata->qt_media_create_date = fields["MediaCreateDate"];
    }

    // MXF timecodes (will have different key names from ExifTool)
    if (fields.find("StartTimecode") != fields.end()) {
        metadata->mxf_start_timecode = fields["StartTimecode"];
        metadata->has_any_timecode = true;
    }
    if (fields.find("TimecodeAtStart") != fields.end()) {
        metadata->mxf_timecode_at_start = fields["TimecodeAtStart"];
        metadata->has_any_timecode = true;
    }
    if (fields.find("StartOfContent") != fields.end()) {
        metadata->mxf_start_of_content = fields["StartOfContent"];
    }

    // XMP timecodes
    if (fields.find("StartTimecode") != fields.end()) {
        metadata->xmp_start_timecode = fields["StartTimecode"];
        metadata->has_any_timecode = true;
    }
    if (fields.find("AltTimecode") != fields.end()) {
        metadata->xmp_alt_timecode = fields["AltTimecode"];
        metadata->has_any_timecode = true;
    }
    if (fields.find("AltTimecodeTimeValue") != fields.end()) {
        metadata->xmp_alt_timecode_time_value = fields["AltTimecodeTimeValue"];
        metadata->has_any_timecode = true;
    }

    // User data timecodes
    if (fields.find("TimeCode") != fields.end()) {
        metadata->userdata_timecode = fields["TimeCode"];
        metadata->has_any_timecode = true;
    }

    return metadata;
}
#endif

std::unordered_map<std::string, std::string> ExifToolHelper::ParseExifOutput(const std::string& output) {
    std::unordered_map<std::string, std::string> result;
    std::istringstream stream(output);
    std::string line;
    int line_count = 0;

    while (std::getline(stream, line)) {
        line_count++;
        Debug::Log("Parsing line " + std::to_string(line_count) + ": [" + line + "]");

        // Find the first colon to split key:value
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos && colon_pos > 0) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Trim whitespace from key
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);

            // Trim whitespace from value
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);

            if (!key.empty()) {
                result[key] = value;
                Debug::Log("  Parsed: [" + key + "] = [" + value + "]");
            }
        }
    }

    return result;
}