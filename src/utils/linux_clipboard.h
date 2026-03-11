#pragma once
#ifdef __linux__

#include <vector>
#include <cstdint>

namespace qcview {

// Copy PNG image data to the system clipboard using native Wayland/X11 APIs.
// Returns true if clipboard ownership was successfully claimed.
// The data is stored internally and served on-demand when other apps paste.
// Must be called from the main thread.
bool CopyImageToClipboard(const std::vector<uint8_t>& png_data);

// Release clipboard ownership and free stored data.
void CleanupClipboardResources();

} // namespace qcview

#endif // __linux__
