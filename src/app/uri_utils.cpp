// ============================================================================
// URI encoding/decoding and project sharing
// ============================================================================

#include "app/application.h"
#include "project/project_manager.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <imgui.h>
#include "utils/debug_utils.h"

// ------------------------------------------------------------------------
// URI ENCODING/DECODING FOR PROJECT SHARING
// ------------------------------------------------------------------------
std::string Application::EncodeURIComponent(const std::string& str) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : str) {
        // Keep alphanumeric and certain characters
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') {
            escaped << c;
        } else {
            // Encode other characters
            escaped << '%' << std::setw(2) << int((unsigned char)c);
        }
    }

    return escaped.str();
}

std::string Application::DecodeURIComponent(const std::string& str) {
    std::ostringstream decoded;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            // Decode hex sequence
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                decoded << static_cast<char>(value);
                i += 2;
            }
        } else {
            decoded << str[i];
        }
    }
    return decoded.str();
}

std::string Application::BuildProjectURI(const std::string& project_path) {
    // Convert backslashes to forward slashes
    std::string normalized_path = project_path;
    std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');

    // Encode the path for URI
    std::string encoded_path = EncodeURIComponent(normalized_path);

    // Build URI: ump:///path
    return "ump:///" + encoded_path;
}

std::string Application::ParseProjectURI(const std::string& uri) {
    // Check if it starts with ump:///
    if (uri.substr(0, 7) != "ump:///") {
        return "";
    }

    // Extract path after ump:///
    std::string encoded_path = uri.substr(7);

    // Decode the path
    std::string decoded_path = DecodeURIComponent(encoded_path);

    // Convert back to Windows path (forward slashes to backslashes on Windows)
    #ifdef _WIN32
    std::replace(decoded_path.begin(), decoded_path.end(), '/', '\\');
    #endif

    return decoded_path;
}

void Application::ShareProject() {
    if (!project_manager) {
        Debug::Log("ShareProject: No project manager available");
        return;
    }

    // Ensure project is saved first
    project_manager->SaveProject();

    // Get the project path
    std::string project_path = project_manager->GetProjectPath();
    if (project_path.empty()) {
        Debug::Log("ShareProject: No project file saved yet");
        ImGui::OpenPopup("No Project Saved##ShareProject");
        return;
    }

    // Build the URI
    std::string uri = BuildProjectURI(project_path);
    Debug::Log("ShareProject: Generated URI: " + uri);

    // Copy to clipboard
    ImGui::SetClipboardText(uri.c_str());
    Debug::Log("ShareProject: URI copied to clipboard");

    // Show success popup
    ImGui::OpenPopup("URI Copied##ShareProject");
}
