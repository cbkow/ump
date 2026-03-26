#pragma once

#ifdef __APPLE__

#include <vector>
#include <cstdint>

// Copy PNG image data to macOS clipboard via NSPasteboard
bool CopyImageToClipboard(const std::vector<uint8_t>& png_data);

// Cleanup clipboard resources (no-op on macOS — NSPasteboard manages lifecycle)
void CleanupClipboardResources();

#endif // __APPLE__
