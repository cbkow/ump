#include "app_paths.h"
#include "debug_utils.h"

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

#include <mutex>

namespace qcview {

std::filesystem::path GetAppDataRoot() {
    static std::filesystem::path cached_path;
    static std::once_flag init_flag;

    std::call_once(init_flag, []() {
#ifdef _WIN32
        char localappdata[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localappdata))) {
            cached_path = std::filesystem::path(localappdata) / "qcview";
        } else {
            cached_path = std::filesystem::path(".") / "qcview";
        }
#else
        const char* home = getenv("HOME");
        if (home) {
            cached_path = std::filesystem::path(home) / ".config" / "qcview";
        } else {
            cached_path = std::filesystem::path(".") / "qcview";
        }
#endif

        // Migration: move old ump data to qcview on first run
        if (!std::filesystem::exists(cached_path)) {
            auto old_path = cached_path.parent_path() / "ump";
            if (std::filesystem::exists(old_path)) {
                std::error_code ec;
                std::filesystem::rename(old_path, cached_path, ec);
                if (ec) {
                    // rename failed (cross-device?), try copy + remove
                    std::filesystem::copy(old_path, cached_path,
                        std::filesystem::copy_options::recursive, ec);
                    if (!ec) {
                        std::filesystem::remove_all(old_path, ec);
                        Debug::Log("Migrated app data from ump/ to qcview/ (copy)");
                    } else {
                        Debug::Log("Migration from ump/ to qcview/ failed: " + ec.message());
                    }
                } else {
                    Debug::Log("Migrated app data from ump/ to qcview/ (rename)");
                }
            }
        }
    });

    return cached_path;
}

} // namespace qcview
