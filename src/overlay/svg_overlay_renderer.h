#pragma once

#include <imgui.h>
#include <string>
#include <vector>

struct SVGRect {
    float x, y, width, height;
    float opacity;
    ImU32 color;
};

class SVGOverlayRenderer {
public:
    SVGOverlayRenderer();

    // Discover all available SVG files in the safety folder
    std::vector<std::string> GetAvailableSVGs() const;

    // Get user-friendly names for the SVGs (filename without extension)
    std::vector<std::string> GetSVGDisplayNames() const;

    // Load and parse a simple rectangular safety overlay SVG
    bool LoadSafetyOverlaySVG(const std::string& svg_path);

    // Render overlay guides directly onto ImGui draw list
    void RenderOverlay(ImDrawList* draw_list, ImVec2 video_pos, ImVec2 video_size,
                       float opacity = 0.7f, ImU32 color = IM_COL32(255, 255, 255, 255),
                       float line_width = 2.0f);

    // Check if overlay is loaded
    bool IsLoaded() const { return is_loaded; }

    // Get overlay info
    const std::string& GetOverlayName() const { return overlay_name; }

    // Get currently loaded SVG path
    const std::string& GetCurrentSVGPath() const { return current_svg_path; }

    // Clear loaded SVG and reset state
    void ClearSVG() {
        is_loaded = false;
        current_svg_path.clear();
        overlay_name = "No overlay selected";
        guides.Clear();
    }

private:
    bool is_loaded = false;
    std::string overlay_name;
    std::string current_svg_path;

    // Structure to store parsed SVG path geometry as line segments
    struct SafetyGuides {
        // SVG path data converted to line segments
        std::vector<std::pair<ImVec2, ImVec2>> path_lines;

        // Original SVG dimensions for scaling
        float svg_width = 1920.0f;
        float svg_height = 1080.0f;

        // Clear all path data
        void Clear() {
            path_lines.clear();
        }

        // Add a line segment
        void AddLine(float x1, float y1, float x2, float y2) {
            path_lines.emplace_back(ImVec2(x1, y1), ImVec2(x2, y2));
        }
    } guides;

    // Convert SVG coordinates to screen coordinates
    ImVec2 SVGToScreen(ImVec2 svg_point, ImVec2 video_pos, ImVec2 video_size) const;

    // Parse SVG content and extract path data into line segments
    void SetupSafetyGuidesForSVG(const std::string& svg_path);
    bool ParseSVGContent(const std::string& svg_content);
    void ParseSVGPath(const std::string& path_data);
    void ParseSVGPolygon(const std::string& points_data);
    void ParseSVGPolyline(const std::string& points_data);

    // Legacy rectangle extraction (kept for fallback)
    bool ParseSVGPathForRectangles(const std::string& svg_content);
    ImVec2 ExtractRectangleFromPath(const std::string& path_data);
};