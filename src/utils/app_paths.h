#pragma once

#include <string>
#include <filesystem>

namespace qcview {

// Returns the application data root directory (%LOCALAPPDATA%\qcview).
// On first call, migrates from the old %LOCALAPPDATA%\ump\ directory if it
// exists and the new directory does not.
std::filesystem::path GetAppDataRoot();

} // namespace qcview
