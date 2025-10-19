#include "node_editor_theme.h"
#include <imgui.h>

#ifdef _WIN32
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// External variable from main.cpp
extern bool use_windows_accent_color;

namespace NodeEditorTheme {

    ImVec4 GetFallbackYellowColor() {
        return ImVec4(0.65f, 0.55f, 0.15f, 1.0f); // Even darker softer yellow color
    }

#ifdef _WIN32
ImVec4 GetWindowsAccentColor() {
    // Check if Windows accent color is enabled
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }

    DWORD colorization_color;
    BOOL opaque_blend;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
        float r = ((colorization_color >> 16) & 0xff) / 255.0f;
        float g = ((colorization_color >> 8) & 0xff) / 255.0f;
        float b = (colorization_color & 0xff) / 255.0f;
        return ImVec4(r, g, b, 1.0f);
    }
    return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback blue
}
#else
ImVec4 GetWindowsAccentColor() {
    // Check if Windows accent color is enabled
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }
    return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback for non-Windows
}
#endif

ImU32 ToImU32(const ImVec4& color) {
    return IM_COL32(
        (int)(color.x * 255.0f),
        (int)(color.y * 255.0f),
        (int)(color.z * 255.0f),
        (int)(color.w * 255.0f)
    );
}

    void ApplyDarkTheme() {
        ImNodesStyle& style = ImNodes::GetStyle();

        // === BACKGROUND & GRID ===
        style.Colors[ImNodesCol_GridBackground] = IM_COL32(25, 25, 25, 255);         
        style.Colors[ImNodesCol_GridLine] = IM_COL32(60, 60, 60, 100);              
        style.Colors[ImNodesCol_GridLinePrimary] = IM_COL32(80, 80, 80, 180);        

        // === NODE STYLING ===
        style.Colors[ImNodesCol_NodeBackground] = IM_COL32(45, 45, 45, 240);         
        style.Colors[ImNodesCol_NodeBackgroundHovered] = IM_COL32(55, 55, 55, 255);   
        style.Colors[ImNodesCol_NodeBackgroundSelected] = IM_COL32(65, 65, 65, 255); 
        style.Colors[ImNodesCol_NodeOutline] = IM_COL32(100, 100, 105, 255);          

        // === TITLE BAR (Default - will be overridden per node type) ===
        style.Colors[ImNodesCol_TitleBar] = IM_COL32(70, 70, 70, 255);               
        style.Colors[ImNodesCol_TitleBarHovered] = IM_COL32(85, 85, 85, 255);
        style.Colors[ImNodesCol_TitleBarSelected] = IM_COL32(100, 100, 100, 255);

        // === CONNECTIONS/LINKS ===
        // Use Windows accent color for dynamic theming
        ImVec4 accent = GetWindowsAccentColor();
        style.Colors[ImNodesCol_Link] = ToImU32(ImVec4(accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, 1.0f)); // Slightly darker accent
        style.Colors[ImNodesCol_LinkHovered] = ToImU32(accent); // Full accent color
        style.Colors[ImNodesCol_LinkSelected] = Colors::CONNECTION_SELECTED; // Keep orange for selection

        // === PINS/SOCKETS ===
        style.Colors[ImNodesCol_Pin] = IM_COL32(200, 200, 200, 255);               
        style.Colors[ImNodesCol_PinHovered] = IM_COL32(255, 255, 255, 255);          

        // === BOX SELECTION ===
        style.Colors[ImNodesCol_BoxSelector] = IM_COL32(50, 150, 250, 80);            
        style.Colors[ImNodesCol_BoxSelectorOutline] = IM_COL32(50, 150, 250, 200);    

        // === SIZING & SPACING ===
        style.NodeCornerRounding = 6.0f;                                           
        style.NodePadding = ImVec2(10.0f, 8.0f);                                     
        style.NodeBorderThickness = 1.5f;                                            
        style.LinkThickness = 3.5f;                                                  
        style.LinkLineSegmentsPerLength = 0.08f;                                     
        style.LinkHoverDistance = 12.0f;                                              
        style.PinCircleRadius = 5.0f;                                                
        style.PinQuadSideLength = 8.0f;                                              
        style.PinLineThickness = 1.5f;                                               
        style.PinHoverRadius = 12.0f;                                                
        style.PinOffset = 2.0f;                                                      

        // === MINIMAP ===
        style.MiniMapPadding = ImVec2(8.0f, 8.0f);
        style.MiniMapOffset = ImVec2(4.0f, 4.0f);
    }

    void ApplyNodeTypeColors() {
        // This function can be called when creating specific node types
        // We'll use this pattern later:
        // ImNodes::PushColorStyle(ImNodesCol_TitleBar, Colors::INPUT_NODE_TITLE);
        // ... create node ...
        // ImNodes::PopColorStyle();
    }

} 