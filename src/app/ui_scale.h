#pragma once

// UI Scale — DPI-aware pixel value scaling
//
// On macOS Retina, GLFW reports contentScale=2.0 which makes all UI elements
// 2x the physical size compared to Windows/Linux on the same monitor.
// g_ui_scale compensates: on macOS at 2x it's ~0.56 (1/1.78), on other platforms it's 1.0.
//
// Usage: wrap hardcoded pixel values with S() to make them DPI-aware:
//   ImVec2(S(200), S(30))   instead of   ImVec2(200, 30)
//   SameLine(S(200))        instead of   SameLine(200)
//
// S() is a no-op on Windows/Linux (g_ui_scale = 1.0).

extern float g_ui_scale;

inline float S(float px) { return px * g_ui_scale; }
