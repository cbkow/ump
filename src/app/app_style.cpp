// ============================================================================
// Font loading, ImGui style, accent color utilities, drag-drop, dark mode
// ============================================================================

#include "app/application.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <vector>
#include <string>
#include <algorithm>

#ifdef _WIN32
#include <dwmapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

// Free functions defined in main.cpp
std::string GetAssetPath(const std::string& relative_path);

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_italic;
extern ImFont* font_mono;
extern ImFont* font_icons;
extern bool use_windows_accent_color;
extern int custom_accent_color_index;
extern ImVec4 custom_picker_color;
extern const ImVec4 accent_color_palette[];
extern const int accent_color_palette_count;

// SETUP & CONFIGURATION METHODS
// ------------------------------------------------------------------------
void Application::LoadCustomFonts() {
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->AddFontDefault();
    font_regular = io.Fonts->AddFontFromFileTTF(GetAssetPath("assets/fonts/Inter_18pt-Regular.ttf").c_str(), 17.0f);
    font_bold = io.Fonts->AddFontFromFileTTF(GetAssetPath("assets/fonts/Inter_18pt-Bold.ttf").c_str(), 17.0f);
    font_italic = io.Fonts->AddFontFromFileTTF(GetAssetPath("assets/fonts/Inter_18pt-Italic.ttf").c_str(), 17.0f);
    font_mono = io.Fonts->AddFontFromFileTTF(GetAssetPath("assets/fonts/JetBrainsMono-Regular.ttf").c_str(), 15.0f);

    ImFontConfig icons_config;
    icons_config.MergeMode = false;
    icons_config.PixelSnapH = true;

    static const ImWchar icons_ranges[] = { 0xE000, 0xF8FF, 0 };
    font_icons = io.Fonts->AddFontFromFileTTF(GetAssetPath("assets/fonts/MaterialSymbolsSharp-Regular.ttf").c_str(), 18.0f, &icons_config, icons_ranges);

    if (font_regular) {
        io.FontDefault = font_regular;
    }

    if (font_icons) {
    }
}

// ========================================================================
// WINDOWS ACCENT COLOR UTILITIES
// ========================================================================
ImVec4 Application::GetDefaultAccentColor() {
    return accent_color_palette[0];  // db8532 - Default amber
}

ImVec4 Application::GetCustomAccentColor() {
    // If custom picker color is selected
    if (custom_accent_color_index == -2) {
        return custom_picker_color;
    }
    // If a custom color is selected from the palette, return it
    if (custom_accent_color_index >= 0 && custom_accent_color_index < accent_color_palette_count) {
        return accent_color_palette[custom_accent_color_index];
    }
    return GetDefaultAccentColor();
}

#ifdef _WIN32
ImVec4 Application::GetWindowsAccentColor() {
    // Check if Windows accent color is enabled
    if (use_windows_accent_color) {
        DWORD colorization_color;
        BOOL opaque_blend;
        if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
            // Convert ARGB to ImVec4 RGBA
            float r = ((colorization_color >> 16) & 0xff) / 255.0f;
            float g = ((colorization_color >> 8) & 0xff) / 255.0f;
            float b = (colorization_color & 0xff) / 255.0f;
            return ImVec4(r, g, b, 1.0f);
        }
        return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback blue if API fails
    }

    // Use custom palette color or default
    return GetCustomAccentColor();
}
#else
ImVec4 Application::GetWindowsAccentColor() {
    // Check if Windows accent color is enabled (non-Windows fallback)
    if (use_windows_accent_color) {
        return GetDefaultAccentColor();  // Can't get system color on non-Windows
    }

    // Use custom palette color or default
    return GetCustomAccentColor();
}
#endif

// Color tinting utilities for creating muted variants
ImVec4 Application::TintColor(const ImVec4& color, float brightness, float saturation) {
    // brightness: 0.0 = black, 1.0 = original, >1.0 = lighter
    // saturation: 0.0 = grayscale, 1.0 = original
    ImVec4 result = color;
    result.x *= brightness;
    result.y *= brightness;
    result.z *= brightness;

    if (saturation < 1.0f) {
        float gray = result.x * 0.299f + result.y * 0.587f + result.z * 0.114f;
        result.x = gray + (result.x - gray) * saturation;
        result.y = gray + (result.y - gray) * saturation;
        result.z = gray + (result.z - gray) * saturation;
    }

    return result;
}

ImVec4 Application::MutedDark(const ImVec4& accent) { return TintColor(accent, 0.7f, 0.4f); }
ImVec4 Application::MutedLight(const ImVec4& accent) { return TintColor(accent, 1.5f, 0.8f); }
ImVec4 Application::Bright(const ImVec4& accent) { return TintColor(accent, 2.2f, 0.5f); }

// Convert ImVec4 to ImU32 for draw list operations
ImU32 Application::ToImU32(const ImVec4& color) {
    // Clamp to [0, 1] range to prevent overflow when Bright() exceeds 1.0
    auto clamp = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    return IM_COL32(
        (int)(clamp(color.x) * 255.0f),
        (int)(clamp(color.y) * 255.0f),
        (int)(clamp(color.z) * 255.0f),
        (int)(clamp(color.w) * 255.0f)
    );
}


void Application::SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    style.FramePadding = ImVec2(8.0f, 8.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);

    colors[ImGuiCol_Text] = ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.125f, 0.125f, 0.125f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.125f, 0.125f, 0.125f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.125f, 0.125f, 0.125f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.25f, 0.27f);  // Visible dock borders
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.125f, 0.125f, 0.125f, 1.0f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.095f, 0.095f, 0.095f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark] = GetWindowsAccentColor();
    colors[ImGuiCol_SliderGrab] = ImVec4(0.54f, 0.54f, 0.54f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.67f, 0.67f, 0.67f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.28f, 0.28f, 0.28f, 0.50f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.19f, 0.19f, 0.19f, 0.55f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.30f, 0.44f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.47f, 0.47f, 0.47f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.72f, 0.72f, 0.72f, 0.29f);
    colors[ImGuiCol_ResizeGripActive] = GetWindowsAccentColor();
    colors[ImGuiCol_Tab] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);                // Transparent (unselected)
    colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.28f, 0.28f, 0.50f);     // Fill on hover
    colors[ImGuiCol_TabActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);      // Solid fill (selected)
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);      // Transparent (unfocused)
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f); // Dimmer fill (selected, unfocused)
    colors[ImGuiCol_DockingPreview] = ImVec4(0.60f, 0.60f, 0.60f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.26f, 0.26f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = GetWindowsAccentColor();
    colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.01f, 0.01f, 0.85f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);

    style.WindowPadding = ImVec2(12.00f, 12.00f);
    style.CellPadding = ImVec2(6.00f, 6.00f);
    style.ItemSpacing = ImVec2(6.00f, 6.00f);
    style.ItemInnerSpacing = ImVec2(6.00f, 6.00f);
    style.TouchExtraPadding = ImVec2(0.00f, 0.00f);
    style.IndentSpacing = 25;
    style.ScrollbarSize = 15;
    style.GrabMinSize = 10;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.WindowRounding = 1.0f;
    style.ChildRounding = 1.0f;
    style.FrameRounding = 1.0f;
    style.PopupRounding = 1.0f;
    style.ScrollbarRounding = 1.0f;
    style.GrabRounding = 1.0f;
    style.LogSliderDeadzone = 4;
    style.TabRounding = 1.0f;

    // Note: HDR color conversion is handled automatically by the ImGui OpenGL shader
    // when ImGui_ImplOpenGL3_SetHDRMode() is called. No manual conversion needed here.
}

void Application::SetupDragDrop() {
    glfwSetWindowUserPointer(window, this);

    glfwSetDropCallback(window, [](GLFWwindow* window, int count, const char** paths) {
        Debug::Log("=== GLFW Drag-Drop Event STARTED ===");

        // Focus the window when files are dropped (like VS Code, Photoshop, etc.)
        // This ensures the window comes to the foreground and receives the drop
        glfwFocusWindow(window);

        Debug::Log("Files dropped: " + std::to_string(count));

        Application* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app || !app->project_manager) {
            Debug::Log("No app or project manager available for drag-drop");
            return;
        }

        Debug::Log("App and project manager available, processing files...");

        if (count == 1) {
            // Single file - act like "Open Video"
            Debug::Log("Single file dropped - acting like Open Video");
            std::string dropped_file = std::string(paths[0]);
            Debug::Log("Dropped file path: " + dropped_file);

            app->project_manager->LoadSingleFileFromDrop(dropped_file);
        }
        else if (count > 1) {
            // Multiple files - act like "Load Media"
            Debug::Log("Multiple files dropped - acting like Load Media");
            app->show_project_panel = true;
            std::vector<std::string> filePaths;
            for (int i = 0; i < count; i++) {
                filePaths.push_back(std::string(paths[i]));
                Debug::Log("  File " + std::to_string(i) + ": " + std::string(paths[i]));
            }
            app->project_manager->LoadMultipleFilesFromDrop(filePaths);
        }

        Debug::Log("=== Drag-Drop Event Complete ===");
        });

    Debug::Log("GLFW drag-drop callback registered");
}

#ifdef _WIN32
void Application::EnableDarkModeWindow(GLFWwindow* window) {

    HWND hwnd = nullptr;

#ifdef GLFW_EXPOSE_NATIVE_WIN32
    hwnd = glfwGetWin32Window(window);
#else
    OutputDebugStringA("Using FindWindowA fallback...\n");
    hwnd = FindWindowA(nullptr, "QCView - Professional Video Editor");
#endif

    if (hwnd) {

        BOOL useDarkMode = TRUE;
        HRESULT result = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

        if (SUCCEEDED(result)) {
        }
        else {
            result = DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
            if (SUCCEEDED(result)) {
            }
            else {
                OutputDebugStringA("FAILED: Both attributes failed\n");
            }
        }

        // Force window refresh
        InvalidateRect(hwnd, nullptr, TRUE);
        UpdateWindow(hwnd);

    }
    else {
        OutputDebugStringA("ERROR: Could not get window handle\n");
    }
}
#endif
