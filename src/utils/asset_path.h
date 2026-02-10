#pragma once

#include <string>
#include <filesystem>

// Global executable directory - set at startup before any asset loading
extern std::filesystem::path g_exe_dir;

// Resolve asset paths relative to executable directory
// Use this instead of relative paths to ensure assets are found
// regardless of the current working directory
std::string GetAssetPath(const std::string& relative_path);
