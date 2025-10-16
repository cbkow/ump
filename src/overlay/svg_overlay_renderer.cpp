#include "svg_overlay_renderer.h"
#include "../utils/debug_utils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <regex>

SVGOverlayRenderer::SVGOverlayRenderer() {
    // Initialize with default dimensions (will be set when SVG is loaded)
    guides.svg_width = 1920.0f;
    guides.svg_height = 1080.0f;

    // No hardcoded coordinates - everything will be parsed from SVG files
    is_loaded = false;
    overlay_name = "No overlay selected";
    Debug::Log("SVG overlay renderer initialized - ready to load SVG files");
}

bool SVGOverlayRenderer::LoadSafetyOverlaySVG(const std::string& svg_path) {
    std::ifstream file(svg_path);
    if (!file.is_open()) {
        Debug::Log("Failed to open SVG file: " + svg_path);
        return false;
    }

    // Read the file to verify it exists and is readable
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    if (content.empty()) {
        Debug::Log("SVG file is empty: " + svg_path);
        return false;
    }

    // Extract filename without extension for display name
    std::filesystem::path path(svg_path);
    overlay_name = path.stem().string();
    current_svg_path = svg_path;

    // Set safety guide coordinates based on aspect ratio and industry standards
    SetupSafetyGuidesForSVG(svg_path);

    is_loaded = true;
    Debug::Log("Successfully loaded safety overlay SVG: " + svg_path);
    return true;
}

void SVGOverlayRenderer::SetupSafetyGuidesForSVG(const std::string& svg_path) {
    std::ifstream file(svg_path);
    if (!file.is_open()) {
        Debug::Log("Failed to open SVG file for parsing: " + svg_path);
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    if (content.empty()) {
        Debug::Log("SVG file is empty: " + svg_path);
        return;
    }

    Debug::Log("Parsing SVG content from: " + svg_path);
    ParseSVGContent(content);
}

bool SVGOverlayRenderer::ParseSVGContent(const std::string& svg_content) {
    guides.Clear();

    // Extract viewBox dimensions
    std::regex viewbox_regex(R"(viewBox=\"([^\"]+)\")");
    std::smatch viewbox_match;
    if (std::regex_search(svg_content, viewbox_match, viewbox_regex)) {
        std::string viewbox = viewbox_match[1].str();
        std::istringstream iss(viewbox);
        float x, y, width, height;
        if (iss >> x >> y >> width >> height) {
            guides.svg_width = width;
            guides.svg_height = height;
            Debug::Log("Extracted viewBox: " + std::to_string(width) + "x" + std::to_string(height));
        }
    } else {
        Debug::Log("Could not find viewBox in SVG, using default dimensions");
        guides.svg_width = 1920.0f;
        guides.svg_height = 1080.0f;
    }

    // Find all path elements and extract their 'd' attribute
    std::regex path_regex(R"(<path[^>]*d=\"([^\"]+)\")");
    std::sregex_iterator path_iter(svg_content.begin(), svg_content.end(), path_regex);
    std::sregex_iterator path_end;

    for (auto it = path_iter; it != path_end; ++it) {
        std::string path_data = (*it)[1].str();
        Debug::Log("Found path data: " + path_data);
        ParseSVGPath(path_data);
    }

    // Find all rect elements and convert them to line segments
    std::regex rect_regex(R"(<rect[^>]*x=\"([^\"]*)\"[^>]*y=\"([^\"]*)\"[^>]*width=\"([^\"]*)\"[^>]*height=\"([^\"]*)\")");
    std::sregex_iterator rect_iter(svg_content.begin(), svg_content.end(), rect_regex);
    std::sregex_iterator rect_end;

    for (auto it = rect_iter; it != rect_end; ++it) {
        float x = it->str(1).empty() ? 0.0f : std::stof(it->str(1));
        float y = it->str(2).empty() ? 0.0f : std::stof(it->str(2));
        float width = std::stof(it->str(3));
        float height = std::stof(it->str(4));

        Debug::Log("Found rect: x=" + std::to_string(x) + " y=" + std::to_string(y) +
                   " w=" + std::to_string(width) + " h=" + std::to_string(height));

        // Convert rectangle to 4 lines (clockwise from top-left)
        guides.AddLine(x, y, x + width, y);           // Top edge
        guides.AddLine(x + width, y, x + width, y + height); // Right edge
        guides.AddLine(x + width, y + height, x, y + height); // Bottom edge
        guides.AddLine(x, y + height, x, y);          // Left edge
    }

    // Find all polygon elements and convert them to line segments
    std::regex polygon_regex(R"(<polygon[^>]*points=\"([^\"]+)\")");
    std::sregex_iterator polygon_iter(svg_content.begin(), svg_content.end(), polygon_regex);
    std::sregex_iterator polygon_end;

    for (auto it = polygon_iter; it != polygon_end; ++it) {
        std::string points_data = (*it)[1].str();
        Debug::Log("Found polygon points: " + points_data);
        ParseSVGPolygon(points_data);
    }

    // Find all line elements and convert them to line segments
    std::regex line_regex(R"(<line[^>]*x1=\"([^\"]*)\"[^>]*y1=\"([^\"]*)\"[^>]*x2=\"([^\"]*)\"[^>]*y2=\"([^\"]*)\")");
    std::sregex_iterator line_iter(svg_content.begin(), svg_content.end(), line_regex);
    std::sregex_iterator line_end;

    for (auto it = line_iter; it != line_end; ++it) {
        float x1 = it->str(1).empty() ? 0.0f : std::stof(it->str(1));
        float y1 = it->str(2).empty() ? 0.0f : std::stof(it->str(2));
        float x2 = it->str(3).empty() ? 0.0f : std::stof(it->str(3));
        float y2 = it->str(4).empty() ? 0.0f : std::stof(it->str(4));

        Debug::Log("Found line: x1=" + std::to_string(x1) + " y1=" + std::to_string(y1) +
                   " x2=" + std::to_string(x2) + " y2=" + std::to_string(y2));

        guides.AddLine(x1, y1, x2, y2);
    }

    // Find all polyline elements and convert them to line segments
    std::regex polyline_regex(R"(<polyline[^>]*points=\"([^\"]+)\")");
    std::sregex_iterator polyline_iter(svg_content.begin(), svg_content.end(), polyline_regex);
    std::sregex_iterator polyline_end;

    for (auto it = polyline_iter; it != polyline_end; ++it) {
        std::string points_data = (*it)[1].str();
        Debug::Log("Found polyline points: " + points_data);
        ParseSVGPolyline(points_data);
    }

    Debug::Log("Parsed " + std::to_string(guides.path_lines.size()) + " line segments from SVG");
    return !guides.path_lines.empty();
}

void SVGOverlayRenderer::ParseSVGPath(const std::string& path_data) {
    // Robust SVG path parser that handles commands with coordinate sequences
    // TikTok example: M1080,0v1920H0V0h1080ZM960,252H120v1028h720V361.5l1.5-1.5h118.5v-108Z

    float current_x = 0.0f, current_y = 0.0f;
    float path_start_x = 0.0f, path_start_y = 0.0f;

    std::string::size_type pos = 0;
    char current_command = '\0';

    Debug::Log("Parsing path: " + path_data);

    while (pos < path_data.length()) {
        // Skip whitespace and commas
        while (pos < path_data.length() && (std::isspace(path_data[pos]) || path_data[pos] == ',')) {
            pos++;
        }
        if (pos >= path_data.length()) break;

        // Check if current character is a command letter
        if (std::isalpha(path_data[pos])) {
            current_command = path_data[pos];
            pos++;
            Debug::Log("Found command: " + std::string(1, current_command));
        }

        // Parse coordinates based on current command
        std::vector<float> coords;
        while (pos < path_data.length()) {
            // Skip whitespace and commas
            while (pos < path_data.length() && (std::isspace(path_data[pos]) || path_data[pos] == ',')) {
                pos++;
            }
            if (pos >= path_data.length()) break;

            // Stop if we hit another command letter
            if (std::isalpha(path_data[pos])) break;

            // Parse number
            std::string::size_type start = pos;
            if (path_data[pos] == '-' || path_data[pos] == '+') pos++; // Handle sign
            while (pos < path_data.length() && (std::isdigit(path_data[pos]) || path_data[pos] == '.')) {
                pos++;
            }

            if (pos > start) {
                float value = std::stof(path_data.substr(start, pos - start));
                coords.push_back(value);
            }
        }

        // Execute command with collected coordinates
        switch (current_command) {
            case 'M': // Move to (absolute)
                if (coords.size() >= 2) {
                    current_x = coords[0];
                    current_y = coords[1];
                    path_start_x = current_x;
                    path_start_y = current_y;
                    Debug::Log("Move to: " + std::to_string(current_x) + "," + std::to_string(current_y));
                }
                break;
            case 'L': // Line to (absolute)
                if (coords.size() >= 2) {
                    guides.AddLine(current_x, current_y, coords[0], coords[1]);
                    current_x = coords[0];
                    current_y = coords[1];
                    Debug::Log("Line to: " + std::to_string(current_x) + "," + std::to_string(current_y));
                }
                break;
            case 'H': // Horizontal line to (absolute)
                if (coords.size() >= 1) {
                    guides.AddLine(current_x, current_y, coords[0], current_y);
                    current_x = coords[0];
                    Debug::Log("Horizontal to: " + std::to_string(current_x));
                }
                break;
            case 'V': // Vertical line to (absolute)
                if (coords.size() >= 1) {
                    guides.AddLine(current_x, current_y, current_x, coords[0]);
                    current_y = coords[0];
                    Debug::Log("Vertical to: " + std::to_string(current_y));
                }
                break;
            case 'h': // Horizontal line to (relative)
                if (coords.size() >= 1) {
                    float new_x = current_x + coords[0];
                    guides.AddLine(current_x, current_y, new_x, current_y);
                    current_x = new_x;
                    Debug::Log("Horizontal rel: " + std::to_string(coords[0]));
                }
                break;
            case 'v': // Vertical line to (relative)
                if (coords.size() >= 1) {
                    float new_y = current_y + coords[0];
                    guides.AddLine(current_x, current_y, current_x, new_y);
                    current_y = new_y;
                    Debug::Log("Vertical rel: " + std::to_string(coords[0]));
                }
                break;
            case 'Z':
            case 'z': // Close path
                guides.AddLine(current_x, current_y, path_start_x, path_start_y);
                current_x = path_start_x;
                current_y = path_start_y;
                Debug::Log("Close path");
                break;
        }
    }
}

void SVGOverlayRenderer::ParseSVGPolygon(const std::string& points_data) {
    // Parse polygon points and create line segments between consecutive points
    // Format: "x1,y1 x2,y2 x3,y3" or "x1 y1 x2 y2 x3 y3"

    std::vector<ImVec2> points;
    std::istringstream iss(points_data);
    std::string token;

    // Split by spaces and commas
    std::string clean_points = points_data;
    std::replace(clean_points.begin(), clean_points.end(), ',', ' ');

    std::istringstream clean_iss(clean_points);
    std::vector<float> coords;
    float value;

    while (clean_iss >> value) {
        coords.push_back(value);
    }

    // Convert pairs of coordinates to points
    for (size_t i = 0; i + 1 < coords.size(); i += 2) {
        points.emplace_back(coords[i], coords[i + 1]);
        Debug::Log("Polygon point: " + std::to_string(coords[i]) + "," + std::to_string(coords[i + 1]));
    }

    // Create lines between consecutive points
    for (size_t i = 0; i < points.size(); ++i) {
        size_t next_i = (i + 1) % points.size(); // Wrap around to close the polygon
        guides.AddLine(points[i].x, points[i].y, points[next_i].x, points[next_i].y);
        Debug::Log("Polygon line: " + std::to_string(points[i].x) + "," + std::to_string(points[i].y) +
                   " to " + std::to_string(points[next_i].x) + "," + std::to_string(points[next_i].y));
    }
}

void SVGOverlayRenderer::ParseSVGPolyline(const std::string& points_data) {
    // Parse polyline points and create line segments between consecutive points
    // Unlike polygon, polyline does NOT close back to the starting point
    // Format: "x1,y1 x2,y2 x3,y3" or "x1 y1 x2 y2 x3 y3"

    std::vector<ImVec2> points;

    // Split by spaces and commas
    std::string clean_points = points_data;
    std::replace(clean_points.begin(), clean_points.end(), ',', ' ');

    std::istringstream clean_iss(clean_points);
    std::vector<float> coords;
    float value;

    while (clean_iss >> value) {
        coords.push_back(value);
    }

    // Convert pairs of coordinates to points
    for (size_t i = 0; i + 1 < coords.size(); i += 2) {
        points.emplace_back(coords[i], coords[i + 1]);
        Debug::Log("Polyline point: " + std::to_string(coords[i]) + "," + std::to_string(coords[i + 1]));
    }

    // Create lines between consecutive points (no wrapping - polyline doesn't close)
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        guides.AddLine(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
        Debug::Log("Polyline line: " + std::to_string(points[i].x) + "," + std::to_string(points[i].y) +
                   " to " + std::to_string(points[i + 1].x) + "," + std::to_string(points[i + 1].y));
    }
}

bool SVGOverlayRenderer::ParseSVGPathForRectangles(const std::string& svg_content) {
    // Extract viewBox dimensions
    std::regex viewbox_regex(R"(viewBox=\"([^\"]+)\")");
    std::smatch viewbox_match;
    if (std::regex_search(svg_content, viewbox_match, viewbox_regex)) {
        std::string viewbox = viewbox_match[1].str();
        std::istringstream iss(viewbox);
        float x, y, width, height;
        if (iss >> x >> y >> width >> height) {
            guides.svg_width = width;
            guides.svg_height = height;
            Debug::Log("Extracted viewBox: " + std::to_string(width) + "x" + std::to_string(height));
        }
    } else {
        Debug::Log("Could not find viewBox in SVG");
        return false;
    }

    // Find all path elements and extract rectangles
    std::regex path_regex(R"(<path[^>]*d=\"([^\"]+)\")");
    std::sregex_iterator path_iter(svg_content.begin(), svg_content.end(), path_regex);
    std::sregex_iterator path_end;

    std::vector<ImVec2> rectangles;

    for (auto it = path_iter; it != path_end; ++it) {
        std::string path_data = (*it)[1].str();
        ImVec2 rect = ExtractRectangleFromPath(path_data);
        if (rect.x > 0 && rect.y > 0) {
            rectangles.push_back(rect);
            Debug::Log("Found rectangle: " + std::to_string(rect.x) + "," + std::to_string(rect.y));
        }
    }

    // Legacy rectangle parsing - no longer used since we parse full SVG paths
    // This method is kept for potential fallback but doesn't update the new path-based structure
    Debug::Log("Legacy rectangle parsing found " + std::to_string(rectangles.size()) + " rectangles, but using full path parsing instead");

    return false;
}

ImVec2 SVGOverlayRenderer::ExtractRectangleFromPath(const std::string& path_data) {
    // Look for rectangular path patterns like "M96,54h1728v972" or "M96,54H1824V1026"
    // This extracts the starting coordinates which represent the margin from edges

    std::regex rect_pattern(R"(M(\d+(?:\.\d+)?),(\d+(?:\.\d+)?))");
    std::smatch match;

    if (std::regex_search(path_data, match, rect_pattern)) {
        float x = std::stof(match[1].str());
        float y = std::stof(match[2].str());
        return ImVec2(x, y);
    }

    return ImVec2(0, 0); // Invalid rectangle
}

std::vector<std::string> SVGOverlayRenderer::GetAvailableSVGs() const {
    std::vector<std::string> svg_files;

    try {
        std::string safety_folder = "assets/safety/";

        if (std::filesystem::exists(safety_folder) && std::filesystem::is_directory(safety_folder)) {
            for (const auto& entry : std::filesystem::directory_iterator(safety_folder)) {
                if (entry.is_regular_file() && entry.path().extension() == ".svg") {
                    svg_files.push_back(entry.path().string());
                }
            }
        }

        // Sort alphabetically for consistent ordering
        std::sort(svg_files.begin(), svg_files.end());
    }
    catch (const std::exception& e) {
        Debug::Log("Error scanning safety folder: " + std::string(e.what()));
    }

    return svg_files;
}

std::vector<std::string> SVGOverlayRenderer::GetSVGDisplayNames() const {
    std::vector<std::string> display_names;
    auto svg_files = GetAvailableSVGs();

    for (const auto& svg_path : svg_files) {
        std::filesystem::path path(svg_path);
        std::string name = path.stem().string();

        // Replace underscores with spaces for better readability
        std::replace(name.begin(), name.end(), '_', ' ');

        display_names.push_back(name);
    }

    return display_names;
}

void SVGOverlayRenderer::RenderOverlay(ImDrawList* draw_list, ImVec2 video_pos, ImVec2 video_size,
                                       float opacity, ImU32 color, float line_width) {
    if (!is_loaded || !draw_list) {
        return;
    }

    // Apply opacity to the color
    ImU32 overlay_color = (color & 0x00FFFFFF) | ((ImU32)(255 * opacity) << 24);

    // Draw all path lines from parsed SVG data
    for (const auto& line : guides.path_lines) {
        ImVec2 start = SVGToScreen(line.first, video_pos, video_size);
        ImVec2 end = SVGToScreen(line.second, video_pos, video_size);
        draw_list->AddLine(start, end, overlay_color, line_width);
    }

    // Add center crosshair (keep this for reference)
    ImVec2 center = ImVec2(
        video_pos.x + video_size.x * 0.5f,
        video_pos.y + video_size.y * 0.5f
    );

    float cross_size = 10.0f;
    draw_list->AddLine(
        ImVec2(center.x - cross_size, center.y),
        ImVec2(center.x + cross_size, center.y),
        overlay_color, line_width * 0.5f
    );
    draw_list->AddLine(
        ImVec2(center.x, center.y - cross_size),
        ImVec2(center.x, center.y + cross_size),
        overlay_color, line_width * 0.5f
    );
}

ImVec2 SVGOverlayRenderer::SVGToScreen(ImVec2 svg_point, ImVec2 video_pos, ImVec2 video_size) const {
    // Calculate aspect ratios
    float svg_aspect = guides.svg_width / guides.svg_height;
    float video_aspect = video_size.x / video_size.y;

    // Use uniform scaling to maintain aspect ratio
    float scale;
    ImVec2 offset = ImVec2(0, 0);

    if (svg_aspect > video_aspect) {
        // SVG is wider - scale to fit width, center vertically
        scale = video_size.x / guides.svg_width;
        float scaled_height = guides.svg_height * scale;
        offset.y = (video_size.y - scaled_height) * 0.5f;
    } else {
        // SVG is taller - scale to fit height, center horizontally
        scale = video_size.y / guides.svg_height;
        float scaled_width = guides.svg_width * scale;
        offset.x = (video_size.x - scaled_width) * 0.5f;
    }

    return ImVec2(
        video_pos.x + offset.x + svg_point.x * scale,
        video_pos.y + offset.y + svg_point.y * scale
    );
}